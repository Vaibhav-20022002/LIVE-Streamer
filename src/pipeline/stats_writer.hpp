#pragma once

/**
 * @file stats_writer.hpp
 * @brief Pipeline telemetry metrics structure and atomic JSON serialization
 * declarations.
 *
 * Role in Pipeline:
 * Aggregates live streaming metrics, encoding frame counts, processing stages,
 * and AI detections, and serializes them atomically to disk so monitoring tools
 * (like Nginx) never read partial JSON.
 */

#include "inference/detection.hpp"
#include "pipeline/profiler.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace streamer {

/// Comprehensive runtime telemetry metrics for the video streaming pipeline.
struct PipelineMetrics {
  std::string state =
      "connecting"; //< connecting | streaming | reconnecting | error
  std::string mode = "transcode"; //< remux | transcode
  std::string source_type = "rtsp";
  int width = 0;
  int height = 0;
  double fps_in = 0.0;
  double fps_out = 0.0;
  uint64_t frames_processed = 0;
  uint64_t frames_dropped = 0;
  uint64_t reconnect_count = 0;
  int active_live_segments = 0;
  int playback_segments = 0;
  double playback_duration_sec = 0.0;

  bool ai_enabled = false;
  std::string ai_backend = "disabled"; //< onnxruntime | synthetic | disabled
  uint64_t ai_inferences = 0;
  std::vector<Detection> latest_detections;

  StageStats decode_stats;
  StageStats inference_stats;
  StageStats encode_stats;
  StageStats mux_stats;
  StageStats total_stats;

  double uptime_sec = 0.0;
  int64_t timestamp_epoch_ms = 0;
};

/// Writes content to a file atomically via temp file and rename.
bool write_atomic_file(const std::filesystem::path &path,
                       const std::string &content);

/// Serializes PipelineMetrics into a JSON formatted string.
std::string serialize_stats_json(const PipelineMetrics &m);

/// Serialized and atomically writes pipeline stats JSON to
/// `hls_dir/stats.json`.
bool write_stats_json(const std::filesystem::path &hls_dir,
                      const PipelineMetrics &m);

/// Escapes special characters for valid JSON strings.
std::string escape_json_string(const std::string &input);

} //< namespace streamer
