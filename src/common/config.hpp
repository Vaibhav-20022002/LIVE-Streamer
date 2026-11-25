#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace streamer {

enum class SourceType { Rtsp, V4l2, AvFoundation, Lavfi, File };
enum class TranscodeMode { Auto, Always, Remux };

struct Config {
  int web_port = 8999;
  std::string hls_output_dir = "/data/hls";

  SourceType source_type = SourceType::Rtsp;
  std::string source_url = "rtsp://mock-camera:8554/live";
  int source_width = 1280;
  int source_height = 720;
  int source_fps = 30;

  TranscodeMode transcode_mode = TranscodeMode::Auto;
  std::string encoder_preset = "ultrafast";
  int encoder_bitrate_kbps = 2000;
  int encoder_threads = 2;
  int hls_segment_duration = 2;
  int hls_live_list_size = 5;
  int playback_retention_sec = 1800;

  bool ai_enabled = true;
  std::string ai_model_path = "/app/models/yolov8n_320.onnx";
  float ai_conf_threshold = 0.45f;
  int ai_threads = 2;
  std::unordered_set<std::string> ai_target_classes = {"person", "car", "truck",
                                                       "bicycle"};

  bool ai_sync_mode = false;
  bool use_buffer_pool = true;
  bool synthetic_detections = false;
  int max_duration_sec = 0;

  int read_timeout_ms = 5000;
  int reconnect_base_ms = 1000;
  int reconnect_max_ms = 30000;
  int profiling_log_interval_sec = 5;
};

std::string source_type_to_string(SourceType t);
SourceType parse_source_type(const std::string &s);
std::string transcode_mode_to_string(TranscodeMode m);
TranscodeMode parse_transcode_mode(const std::string &s);

std::unordered_map<std::string, std::string>
parse_dotenv_file(const std::string &path);
Config load_config_from_map(
    const std::unordered_map<std::string, std::string> &env_map);
Config load_config(const std::string &dotenv_path = ".env");
void validate_config(const Config &cfg);

} //< namespace streamer
