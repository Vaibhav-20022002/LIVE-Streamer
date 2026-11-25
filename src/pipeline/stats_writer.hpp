#pragma once

#include "inference/detection.hpp"
#include "pipeline/profiler.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace streamer {

struct PipelineMetrics {
  std::string state                 = "connecting";
  std::string mode                  = "transcode";
  std::string source_type           = "rtsp";
  int         width                 = 0;
  int         height                = 0;
  double      fps_in                = 0.0;
  double      fps_out               = 0.0;
  uint64_t    frames_processed      = 0;
  uint64_t    frames_dropped        = 0;
  uint64_t    reconnect_count       = 0;
  int         active_live_segments  = 0;
  int         playback_segments     = 0;
  double      playback_duration_sec = 0.0;

  bool                   ai_enabled    = false;
  std::string            ai_backend    = "disabled";
  uint64_t               ai_inferences = 0;
  std::vector<Detection> latest_detections;

  StageStats decode_stats;
  StageStats inference_stats;
  StageStats encode_stats;
  StageStats mux_stats;
  StageStats total_stats;

  double  uptime_sec         = 0.0;
  int64_t timestamp_epoch_ms = 0;
};

/// Write atomically via temp file and rename.
bool write_atomic_file(const std::filesystem::path &path, const std::string &content);

std::string serialize_stats_json(const PipelineMetrics &m);

bool write_stats_json(const std::filesystem::path &hls_dir, const PipelineMetrics &m);

std::string escape_json_string(const std::string &input);

} //< namespace streamer
