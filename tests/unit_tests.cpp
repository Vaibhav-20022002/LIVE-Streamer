#include "capture/backoff.hpp"
#include "common/config.hpp"
#include "inference/detection.hpp"
#include "inference/yuv_overlay.hpp"
#include "muxer/playback_manager.hpp"
#include "pipeline/buffer_pool.hpp"
#include "pipeline/profiler.hpp"
#include "pipeline/ring_buffer.hpp"
#include "pipeline/stats_writer.hpp"
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <vector>

static bool g_failed       = false;
static bool g_check_failed = false;

/// Simple zero-dependency test framework macros
#define TEST_CASE(name)                                               \
  void name();                                                        \
  struct Register_##name {                                            \
    Register_##name() {                                               \
      std::cout << "[TEST_SUITE] " << #name << " ... " << std::flush; \
      try {                                                           \
        name();                                                       \
        std::cout << "PASSED\n";                                      \
      } catch (const std::exception &e) {                             \
        std::cout << "FAILED: exception: " << e.what() << "\n";       \
        g_failed = true;                                              \
      } catch (...) {                                                 \
        std::cout << "FAILED: unknown exception\n";                   \
        g_failed = true;                                              \
      }                                                               \
    }                                                                 \
  } creator_##name;                                                   \
  void name()

#define REQUIRE(cond)                                                    \
  do {                                                                   \
    if (!(cond)) {                                                       \
      throw std::runtime_error("REQUIRE failed: " + std::string(#cond)); \
    }                                                                    \
  } while (0)

#define CHECK(cond)                                     \
  do {                                                  \
    if (!(cond)) {                                      \
      std::cerr << "  CHECK failed: " << #cond << "\n"; \
      g_check_failed = true;                            \
    }                                                   \
  } while (0)

/// 1. Config parsing & validation
TEST_CASE(test_config_parsing_and_validation) {
  std::filesystem::path tmp_env = "test_temp_env.env";
  {
    std::ofstream ofs(tmp_env);
    ofs << "# Comment line\n";
    ofs << "WEB_PORT=9000\n";
    ofs << "HLS_OUTPUT_DIR=\"/tmp/hls_test\"\n";
    ofs << "AI_TARGET_CLASSES=person,car,dog\n";
    ofs << "SOURCE_WIDTH=1920\n";
  }

  auto env_map = streamer::parse_dotenv_file(tmp_env.string());
  REQUIRE(env_map["WEB_PORT"] == "9000");
  REQUIRE(env_map["HLS_OUTPUT_DIR"] == "/tmp/hls_test");
  REQUIRE(env_map["AI_TARGET_CLASSES"] == "person,car,dog");

  streamer::Config cfg = streamer::load_config_from_map(env_map);
  REQUIRE(cfg.web_port == 9000);
  REQUIRE(cfg.hls_output_dir == "/tmp/hls_test");
  REQUIRE(cfg.source_width == 1920);
  REQUIRE(cfg.ai_target_classes.count("person") == 1);
  REQUIRE(cfg.ai_target_classes.count("car") == 1);
  REQUIRE(cfg.ai_target_classes.count("dog") == 1);
  REQUIRE(cfg.ai_target_classes.count("truck") == 0);

  /// Validate valid config doesn't throw
  streamer::validate_config(cfg);

  /// Wildcard AI_TARGET_CLASSES=* clears target filter to allow all COCO classes
  auto wildcard_map                 = env_map;
  wildcard_map["AI_TARGET_CLASSES"] = "*";
  streamer::Config wildcard_cfg     = streamer::load_config_from_map(wildcard_map);
  REQUIRE(wildcard_cfg.ai_target_classes.empty());

  /// Test invalid port
  streamer::Config bad_cfg1 = cfg;
  bad_cfg1.web_port         = 70000;
  bool threw1               = false;
  try {
    streamer::validate_config(bad_cfg1);
  } catch (const std::invalid_argument &) {
    threw1 = true;
  }
  REQUIRE(threw1);

  /// Test negative width
  streamer::Config bad_cfg2 = cfg;
  bad_cfg2.source_width     = -100;
  bool threw2               = false;
  try {
    streamer::validate_config(bad_cfg2);
  } catch (const std::invalid_argument &) {
    threw2 = true;
  }
  REQUIRE(threw2);

  /// Test bad retention
  streamer::Config bad_cfg3       = cfg;
  bad_cfg3.playback_retention_sec = -5;
  bool threw3                     = false;
  try {
    streamer::validate_config(bad_cfg3);
  } catch (const std::invalid_argument &) {
    threw3 = true;
  }
  REQUIRE(threw3);

  std::filesystem::remove(tmp_env);
}

/// 2. DropOldestRingBuffer
TEST_CASE(test_drop_oldest_ring_buffer) {
  streamer::DropOldestRingBuffer<int> buf(2);
  buf.push(1);
  buf.push(2);
  bool dropped = buf.push(3);
  REQUIRE(dropped);
  REQUIRE(buf.dropped_count() == 1);

  int val = 0;
  REQUIRE(buf.try_pop(val));
  REQUIRE(val == 2);
  REQUIRE(buf.try_pop(val));
  REQUIRE(val == 3);
  REQUIRE(!buf.try_pop(val));

  /// Test shutdown waking blocked pop_wait
  std::thread t([&buf]() {
    int  v   = 0;
    bool res = buf.pop_wait(v);
    REQUIRE(!res);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  buf.shutdown();
  t.join();

  /// Multi-threaded producer/consumer stress test
  streamer::DropOldestRingBuffer<int> stress_buf(10);
  std::atomic<bool>                   stop{false};
  std::vector<std::thread>            producers;
  for (int i = 0; i < 3; ++i) {
    producers.emplace_back([&stress_buf, &stop, i]() {
      int count = 0;
      while (!stop && count < 100) {
        stress_buf.push(i * 1000 + count);
        ++count;
        std::this_thread::yield();
      }
    });
  }

  std::thread consumer([&stress_buf, &stop]() {
    int v = 0;
    while (!stop || stress_buf.size() > 0) {
      if (stress_buf.try_pop(v)) {
        /// consumed
      } else {
        std::this_thread::yield();
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop = true;
  stress_buf.shutdown();
  for (auto &p : producers) p.join();
  consumer.join();
}

/// 3. ByteBufferPool
TEST_CASE(test_byte_buffer_pool) {
  auto pool = streamer::ByteBufferPool::create(1024, 2, 4);
  REQUIRE(pool->available_count() == 2);

  auto p1 = pool->acquire();
  REQUIRE(p1->size() == 1024);
  uint8_t *addr1 = p1->data();

  /// Release p1 by letting it go out of scope -> returns to initial pool of 2
  p1.reset();
  REQUIRE(pool->available_count() == 2);

  auto     p2    = pool->acquire();
  uint8_t *addr2 = p2->data();
  REQUIRE(addr1 == addr2); //< exact same pooled memory address

  REQUIRE(pool->available_count() == 1);
  p2.reset();
  REQUIRE(pool->available_count() == 2);
}

/// 4. ExponentialBackoff
TEST_CASE(test_exponential_backoff) {
  /// zero-jitter deterministic sequence: base=100, max=500 -> 100, 200, 400,
  /// 500, 500
  streamer::ExponentialBackoff eb(100, 500, 0.0, 42);
  REQUIRE(eb.next_delay_ms() == 100);
  REQUIRE(eb.next_delay_ms() == 200);
  REQUIRE(eb.next_delay_ms() == 400);
  REQUIRE(eb.next_delay_ms() == 500);
  REQUIRE(eb.next_delay_ms() == 500);

  eb.reset();
  REQUIRE(eb.attempt_count() == 0);
  REQUIRE(eb.next_delay_ms() == 100);

  /// Jittered constructor stays within [base_ms, max_ms]
  streamer::ExponentialBackoff eb_jitter(100, 500, 0.25, 123);
  for (int i = 0; i < 20; ++i) {
    int d = eb_jitter.next_delay_ms();
    REQUIRE(d >= 100);
    REQUIRE(d <= 500);
  }
}

/// 5. YOLOv8 letterbox, decode & NMS
TEST_CASE(test_yolov8_pipeline) {
  /// Synthetic 160x90 RGB buffer
  int                  src_w = 160;
  int                  src_h = 90;
  std::vector<uint8_t> rgb(src_w * src_h * 3, 100);

  int                dst_w = 320;
  int                dst_h = 320;
  std::vector<float> out_nchw(3 * dst_w * dst_h);

  streamer::LetterboxInfo lb =
      streamer::letterbox_rgb_to_nchw(rgb.data(), src_w, src_h, dst_w, dst_h, out_nchw.data());

  /// Verify scale & padding: scale=2.0, pad_x=0, pad_y=70
  REQUIRE(std::abs(lb.scale - 2.0f) < 1e-5f);
  REQUIRE(lb.pad_x == 0);
  REQUIRE(lb.pad_y == 70);

  /// Verify gray padding pixel 114.0f/255.0f at (0,0) in NCHW
  float expected_pad = 114.0f / 255.0f;
  REQUIRE(std::abs(out_nchw[0] - expected_pad) < 1e-3f);

  /// Construct an [84, 10] float tensor
  int                num_classes = 80;
  int                num_anchors = 10;
  std::vector<float> tensor((4 + num_classes) * num_anchors, 0.0f);

  auto set_anchor = [&](int idx, float cx, float cy, float w, float h, int cls_id, float conf) {
    tensor[0 * num_anchors + idx]            = cx;
    tensor[1 * num_anchors + idx]            = cy;
    tensor[2 * num_anchors + idx]            = w;
    tensor[3 * num_anchors + idx]            = h;
    tensor[(4 + cls_id) * num_anchors + idx] = conf;
  };

  /// 1. person 1
  set_anchor(0, 100.0f, 100.0f, 40.0f, 40.0f, 0, 0.9f);
  /// 2. person 2 (overlapping person, high iou)
  set_anchor(1, 102.0f, 100.0f, 40.0f, 40.0f, 0, 0.85f);
  /// 3. car (class 2)
  set_anchor(2, 200.0f, 200.0f, 60.0f, 30.0f, 2, 0.95f);
  /// 4. low confidence
  set_anchor(3, 50.0f, 50.0f, 20.0f, 20.0f, 0, 0.2f);
  /// 5. non-target class (dog = class 16) inside active image area (y >= pad_y=70)
  set_anchor(4, 240.0f, 150.0f, 20.0f, 20.0f, 16, 0.9f);

  std::unordered_set<std::string>  targets = {"person", "car"};
  std::vector<streamer::Detection> dets    = streamer::decode_yolov8_output(
      tensor.data(), num_classes, num_anchors, lb, src_w, src_h, 0.45f, 0.5f, targets);

  REQUIRE(dets.size() == 2);
  bool found_person = false;
  bool found_car    = false;
  for (const auto &d : dets) {
    if (d.label == "person") found_person = true;
    if (d.label == "car") found_car = true;
    REQUIRE(d.x1 >= 0.0f);
    REQUIRE(d.y1 >= 0.0f);
    REQUIRE(d.x2 <= static_cast<float>(src_w));
    REQUIRE(d.y2 <= static_cast<float>(src_h));
  }
  REQUIRE(found_person);
  REQUIRE(found_car);

  /// Empty set or {"*"} allows all classes including dog (class 16)
  std::vector<streamer::Detection> all_dets_empty = streamer::decode_yolov8_output(
      tensor.data(), num_classes, num_anchors, lb, src_w, src_h, 0.45f, 0.5f, {});
  REQUIRE(all_dets_empty.size() == 3);
  std::vector<streamer::Detection> all_dets_star = streamer::decode_yolov8_output(
      tensor.data(), num_classes, num_anchors, lb, src_w, src_h, 0.45f, 0.5f, {"*"});
  REQUIRE(all_dets_star.size() == 3);
}

/// 6. YUV420P overlay clipping
TEST_CASE(test_yuv_overlay_clipping) {
  int                  w = 128;
  int                  h = 96;
  std::vector<uint8_t> y_plane(w * h, 128);
  std::vector<uint8_t> u_plane((w / 2) * (h / 2), 128);
  std::vector<uint8_t> v_plane((w / 2) * (h / 2), 128);

  std::vector<streamer::Detection> dets = {
      {0, "person", 0.9f, 10.0f, 10.0f, 50.0f, 50.0f},
      {2, "car", 0.8f, -10.0f, -5.0f, 140.0f, 110.0f} //< partially out-of-bounds
  };

  streamer::draw_detections_yuv420p(
      y_plane.data(), w, u_plane.data(), w / 2, v_plane.data(), w / 2, w, h, dets);

  bool pixel_changed = false;
  for (size_t i = 0; i < y_plane.size(); ++i) {
    if (y_plane[i] != 128) {
      pixel_changed = true;
      break;
    }
  }
  REQUIRE(pixel_changed);
}

/// 7. PlaybackManager & atomic stats
TEST_CASE(test_playback_manager_and_stats) {
  std::filesystem::path tmp_hls = "test_hls_dir";
  std::filesystem::create_directories(tmp_hls);

  /// Write synthetic live.m3u8
  {
    std::ofstream ofs(tmp_hls / "live.m3u8");
    ofs << "#EXTM3U\n";
    ofs << "#EXT-X-VERSION:6\n";
    ofs << "#EXT-X-TARGETDURATION:2\n";
    ofs << "#EXT-X-MEDIA-SEQUENCE:0\n";
    ofs << "#EXT-X-MAP:URI=\"init_0.mp4\"\n";
    ofs << "#EXTINF:2.000,\n";
    ofs << "seg_000000.m4s\n";
    ofs << "#EXTINF:2.000,\n";
    ofs << "seg_000001.m4s\n";
  }

  /// Create dummy segment files
  for (int i = 0; i < 5; ++i) {
    std::ofstream(tmp_hls / ("seg_00000" + std::to_string(i) + ".m4s")) << "dummy segment data";
  }
  std::ofstream(tmp_hls / "init_0.mp4") << "dummy init 0";
  std::ofstream(tmp_hls / "init_1.mp4") << "dummy init 1";

  streamer::PlaybackManager pm(tmp_hls,
      8); //< retention_sec = 8 (prunes seg_000000, keeps seg_000001..000004)
  bool                      sync1 = pm.sync_from_live_playlist();
  REQUIRE(sync1);
  REQUIRE(pm.active_live_count() == 2);

  /// Record detections
  pm.record_detections(1000, {{0, "person", 0.9f, 10.0f, 10.0f, 50.0f, 50.0f}});

  /// Mark discontinuity and new init file
  pm.mark_discontinuity("init_1.mp4");

  /// Update live.m3u8 to seg_000002..seg_000004
  {
    std::ofstream ofs(tmp_hls / "live.m3u8");
    ofs << "#EXTM3U\n";
    ofs << "#EXT-X-VERSION:6\n";
    ofs << "#EXT-X-TARGETDURATION:2\n";
    ofs << "#EXT-X-MEDIA-SEQUENCE:2\n";
    ofs << "#EXT-X-MAP:URI=\"init_1.mp4\"\n";
    ofs << "#EXTINF:2.000,\n";
    ofs << "seg_000002.m4s\n";
    ofs << "#EXTINF:2.000,\n";
    ofs << "seg_000003.m4s\n";
    ofs << "#EXTINF:2.000,\n";
    ofs << "seg_000004.m4s\n";
  }

  bool sync2 = pm.sync_from_live_playlist();
  REQUIRE(sync2);

  /// Prune expired with retention_sec = 8 (total=10s -> prunes seg_000000
  /// leaving 8s)
  size_t pruned = pm.prune_expired();
  REQUIRE(pruned == 1);
  REQUIRE(std::filesystem::exists(tmp_hls / "seg_000001.m4s"));  //< retained in DVR
  REQUIRE(std::filesystem::exists(tmp_hls / "seg_000004.m4s"));  //< active live never deleted
  REQUIRE(!std::filesystem::exists(tmp_hls / "seg_000000.m4s")); //< pruned

  /// Write playback manifest and events json
  REQUIRE(pm.write_playback_manifest());
  REQUIRE(pm.write_events_json());

  /// Verify playback.m3u8 content contains #EXT-X-DISCONTINUITY and both
  /// #EXT-X-MAP entries
  {
    std::ifstream ifs(tmp_hls / "playback.m3u8");
    std::string   content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("#EXT-X-DISCONTINUITY") != std::string::npos);
    REQUIRE(content.find("init_0.mp4") != std::string::npos);
    REQUIRE(content.find("init_1.mp4") != std::string::npos);
  }

  /// Test stats writer JSON serialization
  streamer::PipelineMetrics metrics;
  metrics.state        = "streaming";
  metrics.width        = 1280;
  metrics.height       = 720;
  std::string json_str = streamer::serialize_stats_json(metrics);
  REQUIRE(json_str.find("streaming") != std::string::npos);
  REQUIRE(json_str.find("1280") != std::string::npos);

  REQUIRE(streamer::write_stats_json(tmp_hls, metrics));

  std::vector<streamer::StreamDescriptor> descriptors = {
      {"cam1", "cam1", "rtsp", "/hls/cam1"},
      {"cam2", "cam2", "lavfi", "/hls/cam2"},
  };
  std::string streams_json = streamer::serialize_streams_json(descriptors, "cam1");
  REQUIRE(streams_json.find("\"default_stream\":\"cam1\"") != std::string::npos);
  REQUIRE(streams_json.find("/hls/cam2") != std::string::npos);
  REQUIRE(streamer::write_streams_json(tmp_hls, descriptors, "cam1"));
  REQUIRE(std::filesystem::exists(tmp_hls / "streams.json"));

  /// Cleanup temp dir
  std::filesystem::remove_all(tmp_hls);
}

/// 8. Multi-stream config parsing & duplicate ID check
TEST_CASE(test_multi_stream_config) {
  auto parsed =
      streamer::parse_streams_spec("cam1=rtsp|rtsp://mock-camera:8554/cam1, "
                                   "cam2=lavfi|testsrc=size=640x360:rate=30, rtsp://10.0.0.5/live",
          streamer::SourceType::File);
  REQUIRE(parsed.size() == 3);
  REQUIRE(parsed[0].id == "cam1");
  REQUIRE(parsed[0].source_type == streamer::SourceType::Rtsp);
  REQUIRE(parsed[0].source_url == "rtsp://mock-camera:8554/cam1");
  REQUIRE(parsed[1].id == "cam2");
  REQUIRE(parsed[1].source_type == streamer::SourceType::Lavfi);
  REQUIRE(parsed[2].id == "stream1");
  REQUIRE(parsed[2].source_type == streamer::SourceType::Rtsp);

  streamer::Config dup_cfg;
  dup_cfg.streams = {
      {"cam1", "cam1", streamer::SourceType::Rtsp, "rtsp://host/1"},
      {"cam1", "cam1", streamer::SourceType::Rtsp, "rtsp://host/2"},
  };
  bool threw_dup = false;
  try {
    streamer::validate_config(dup_cfg);
  } catch (const std::invalid_argument &) {
    threw_dup = true;
  }
  REQUIRE(threw_dup);
}

/// 9. StageProfiler
TEST_CASE(test_stage_profiler) {
  streamer::StageProfiler profiler;
  profiler.record(streamer::Stage::Inference, 10.0);
  profiler.record(streamer::Stage::Inference, 20.0);
  profiler.record(streamer::Stage::Inference, 30.0);
  profiler.record(streamer::Stage::Inference, 40.0);
  profiler.record(streamer::Stage::Inference, 100.0);

  streamer::StageStats stats = profiler.snapshot(streamer::Stage::Inference);
  REQUIRE(stats.count == 5);
  REQUIRE(std::abs(stats.avg_ms - 40.0) < 1e-5f);
  REQUIRE(std::abs(stats.p50_ms - 30.0) < 1e-5f);
  REQUIRE(stats.p95_ms >= 40.0);

  std::string summary = profiler.format_summary();
  REQUIRE(!summary.empty());
}

int main() {
  std::cout << "=== Running C++20 Unit Tests for CustomStreamer ===\n";
  if (g_failed || g_check_failed) {
    std::cout << "\nSome tests FAILED.\n";
    return 1;
  } else {
    std::cout << "\nAll unit tests PASSED successfully!\n";
    return 0;
  }
}
