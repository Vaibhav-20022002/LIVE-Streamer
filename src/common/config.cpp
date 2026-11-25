#include "common/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace streamer {

namespace {

std::string to_lower_ascii(const std::string &str) {
  std::string lower;
  lower.reserve(str.size());
  for (char c : str) {
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lower;
}

std::string trim(const std::string &str) {
  size_t start = 0;
  while (start < str.size() &&
         std::isspace(static_cast<unsigned char>(str[start]))) {
    ++start;
  }
  size_t end = str.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(str[end - 1]))) {
    --end;
  }
  return str.substr(start, end - start);
}

std::string strip_quotes_and_comment(const std::string &raw) {
  /// Strip inline '#' comments only when outside single/double quotes.
  std::string s;
  bool in_single = false;
  bool in_double = false;
  for (char c : raw) {
    if (c == '\'' && !in_double) {
      in_single = !in_single;
      s.push_back(c);
    } else if (c == '"' && !in_single) {
      in_double = !in_double;
      s.push_back(c);
    } else if (c == '#' && !in_single && !in_double) {
      break;
    } else {
      s.push_back(c);
    }
  }

  s = trim(s);
  if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') ||
                        (s.front() == '"' && s.back() == '"'))) {
    s = s.substr(1, s.size() - 2);
  }
  return trim(s);
}

bool parse_bool_or_throw(const char *key, const std::string &val_str) {
  const std::string lower = to_lower_ascii(val_str);
  if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
    return true;
  }
  if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
    return false;
  }
  throw std::invalid_argument(std::string("Invalid boolean for ") + key + ": " +
                              val_str);
}

int parse_int(const std::string &val_str) {
  try {
    size_t idx = 0;
    int res = std::stoi(val_str, &idx);
    if (idx != val_str.size()) {
      throw std::invalid_argument("Invalid integer: " + val_str);
    }
    return res;
  } catch (...) {
    throw std::invalid_argument("Invalid integer: " + val_str);
  }
}

float parse_float(const std::string &val_str) {
  try {
    size_t idx = 0;
    float res = std::stof(val_str, &idx);
    if (idx != val_str.size()) {
      throw std::invalid_argument("Invalid float: " + val_str);
    }
    return res;
  } catch (...) {
    throw std::invalid_argument("Invalid float: " + val_str);
  }
}

} //< namespace

std::string source_type_to_string(SourceType t) {
  switch (t) {
  case SourceType::Rtsp:
    return "rtsp";
  case SourceType::V4l2:
    return "v4l2";
  case SourceType::AvFoundation:
    return "avfoundation";
  case SourceType::Lavfi:
    return "lavfi";
  case SourceType::File:
    return "file";
  }
  return "rtsp";
}

SourceType parse_source_type(const std::string &s) {
  const std::string lower = to_lower_ascii(s);
  if (lower == "rtsp")
    return SourceType::Rtsp;
  if (lower == "v4l2")
    return SourceType::V4l2;
  if (lower == "avfoundation")
    return SourceType::AvFoundation;
  if (lower == "lavfi")
    return SourceType::Lavfi;
  if (lower == "file")
    return SourceType::File;
  throw std::invalid_argument("Unknown source type: " + s);
}

std::string transcode_mode_to_string(TranscodeMode m) {
  switch (m) {
  case TranscodeMode::Auto:
    return "auto";
  case TranscodeMode::Always:
    return "always";
  case TranscodeMode::Remux:
    return "remux";
  }
  return "auto";
}

TranscodeMode parse_transcode_mode(const std::string &s) {
  const std::string lower = to_lower_ascii(s);
  if (lower == "auto")
    return TranscodeMode::Auto;
  if (lower == "always")
    return TranscodeMode::Always;
  if (lower == "remux")
    return TranscodeMode::Remux;
  throw std::invalid_argument("Unknown transcode mode: " + s);
}

std::unordered_map<std::string, std::string>
parse_dotenv_file(const std::string &path) {
  std::unordered_map<std::string, std::string> map;
  std::ifstream file(path);
  if (!file.is_open()) {
    return map;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    const size_t eq_pos = trimmed.find('=');
    if (eq_pos == std::string::npos) {
      continue;
    }

    const std::string key = trim(trimmed.substr(0, eq_pos));
    const std::string val =
        strip_quotes_and_comment(trimmed.substr(eq_pos + 1));
    if (!key.empty()) {
      map[key] = val;
    }
  }
  return map;
}

Config load_config_from_map(
    const std::unordered_map<std::string, std::string> &env_map) {
  Config cfg;

  auto get_val = [&](const std::string &key) -> const std::string * {
    auto it = env_map.find(key);
    return (it != env_map.end()) ? &it->second : nullptr;
  };

  if (auto *v = get_val("WEB_PORT"))
    cfg.web_port = parse_int(*v);
  if (auto *v = get_val("HLS_OUTPUT_DIR"))
    cfg.hls_output_dir = *v;
  if (auto *v = get_val("SOURCE_TYPE"))
    cfg.source_type = parse_source_type(*v);
  if (auto *v = get_val("SOURCE_URL"))
    cfg.source_url = *v;
  if (auto *v = get_val("SOURCE_WIDTH"))
    cfg.source_width = parse_int(*v);
  if (auto *v = get_val("SOURCE_HEIGHT"))
    cfg.source_height = parse_int(*v);
  if (auto *v = get_val("SOURCE_FPS"))
    cfg.source_fps = parse_int(*v);
  if (auto *v = get_val("TRANSCODE_MODE"))
    cfg.transcode_mode = parse_transcode_mode(*v);
  if (auto *v = get_val("ENCODER_PRESET"))
    cfg.encoder_preset = *v;
  if (auto *v = get_val("ENCODER_BITRATE_KBPS"))
    cfg.encoder_bitrate_kbps = parse_int(*v);
  if (auto *v = get_val("ENCODER_THREADS"))
    cfg.encoder_threads = parse_int(*v);
  if (auto *v = get_val("HLS_SEGMENT_DURATION"))
    cfg.hls_segment_duration = parse_int(*v);
  if (auto *v = get_val("HLS_LIVE_LIST_SIZE"))
    cfg.hls_live_list_size = parse_int(*v);
  if (auto *v = get_val("PLAYBACK_RETENTION_SEC"))
    cfg.playback_retention_sec = parse_int(*v);
  if (auto *v = get_val("AI_ENABLED"))
    cfg.ai_enabled = parse_bool_or_throw("AI_ENABLED", *v);
  if (auto *v = get_val("AI_MODEL_PATH"))
    cfg.ai_model_path = *v;
  if (auto *v = get_val("AI_CONF_THRESHOLD"))
    cfg.ai_conf_threshold = parse_float(*v);
  if (auto *v = get_val("AI_THREADS"))
    cfg.ai_threads = parse_int(*v);

  if (auto *v = get_val("AI_TARGET_CLASSES")) {
    std::unordered_set<std::string> classes;
    std::stringstream ss(*v);
    std::string item;
    while (std::getline(ss, item, ',')) {
      std::string lower = to_lower_ascii(trim(item));
      if (!lower.empty()) {
        classes.insert(std::move(lower));
      }
    }
    if (classes.count("*")) {
      cfg.ai_target_classes = {"*"};
    } else {
      cfg.ai_target_classes = std::move(classes);
    }
  }

  if (auto *v = get_val("AI_SYNC_MODE")) {
    cfg.ai_sync_mode = parse_bool_or_throw("AI_SYNC_MODE", *v);
  }
  if (auto *v = get_val("USE_BUFFER_POOL")) {
    cfg.use_buffer_pool = parse_bool_or_throw("USE_BUFFER_POOL", *v);
  }
  if (auto *v = get_val("SYNTHETIC_DETECTIONS")) {
    cfg.synthetic_detections = parse_bool_or_throw("SYNTHETIC_DETECTIONS", *v);
  }
  if (auto *v = get_val("MAX_DURATION_SEC"))
    cfg.max_duration_sec = parse_int(*v);
  if (auto *v = get_val("READ_TIMEOUT_MS"))
    cfg.read_timeout_ms = parse_int(*v);
  if (auto *v = get_val("RECONNECT_BASE_MS"))
    cfg.reconnect_base_ms = parse_int(*v);
  if (auto *v = get_val("RECONNECT_MAX_MS"))
    cfg.reconnect_max_ms = parse_int(*v);
  if (auto *v = get_val("PROFILING_LOG_INTERVAL_SEC")) {
    cfg.profiling_log_interval_sec = parse_int(*v);
  }

  validate_config(cfg);
  return cfg;
}

Config load_config(const std::string &dotenv_path) {
  auto map = parse_dotenv_file(dotenv_path);

  /// Process environment overrides .env template defaults (12-factor
  /// precedence).
  const char *keys[] = {"WEB_PORT",
                        "HLS_OUTPUT_DIR",
                        "SOURCE_TYPE",
                        "SOURCE_URL",
                        "SOURCE_WIDTH",
                        "SOURCE_HEIGHT",
                        "SOURCE_FPS",
                        "TRANSCODE_MODE",
                        "ENCODER_PRESET",
                        "ENCODER_BITRATE_KBPS",
                        "ENCODER_THREADS",
                        "HLS_SEGMENT_DURATION",
                        "HLS_LIVE_LIST_SIZE",
                        "PLAYBACK_RETENTION_SEC",
                        "AI_ENABLED",
                        "AI_MODEL_PATH",
                        "AI_CONF_THRESHOLD",
                        "AI_THREADS",
                        "AI_TARGET_CLASSES",
                        "AI_SYNC_MODE",
                        "USE_BUFFER_POOL",
                        "SYNTHETIC_DETECTIONS",
                        "MAX_DURATION_SEC",
                        "READ_TIMEOUT_MS",
                        "RECONNECT_BASE_MS",
                        "RECONNECT_MAX_MS",
                        "PROFILING_LOG_INTERVAL_SEC"};

  for (const char *key : keys) {
    if (const char *env_val = std::getenv(key)) {
      map[key] = std::string(env_val);
    }
  }

  return load_config_from_map(map);
}

void validate_config(const Config &cfg) {
  if (cfg.web_port < 1 || cfg.web_port > 65535) {
    throw std::invalid_argument("Invalid web_port: must be in [1..65535]");
  }
  if (cfg.hls_output_dir.empty()) {
    throw std::invalid_argument("Invalid hls_output_dir: cannot be empty");
  }
  if (cfg.source_url.empty()) {
    throw std::invalid_argument("Invalid source_url: cannot be empty");
  }
  if (cfg.source_width <= 0 || cfg.source_height <= 0) {
    throw std::invalid_argument(
        "Invalid source dimensions: width and height must be > 0");
  }
  if (cfg.source_fps < 1 || cfg.source_fps > 120) {
    throw std::invalid_argument("Invalid source_fps: must be in [1..120]");
  }
  if (cfg.encoder_bitrate_kbps <= 0) {
    throw std::invalid_argument("Invalid encoder_bitrate_kbps: must be > 0");
  }
  if (cfg.encoder_threads < 0) {
    throw std::invalid_argument("Invalid encoder_threads: must be >= 0");
  }
  if (cfg.hls_segment_duration < 1) {
    throw std::invalid_argument("Invalid hls_segment_duration: must be >= 1");
  }
  if (cfg.hls_live_list_size < 2) {
    throw std::invalid_argument("Invalid hls_live_list_size: must be >= 2");
  }
  if (cfg.playback_retention_sec <
      cfg.hls_segment_duration * cfg.hls_live_list_size) {
    throw std::invalid_argument("Invalid playback_retention_sec: must be >= "
                                "hls_segment_duration * hls_live_list_size");
  }
  if (cfg.ai_conf_threshold <= 0.0f || cfg.ai_conf_threshold >= 1.0f) {
    throw std::invalid_argument(
        "Invalid ai_conf_threshold: must be > 0.0 and < 1.0");
  }
  if (cfg.ai_threads < 0) {
    throw std::invalid_argument("Invalid ai_threads: must be >= 0");
  }
  if (cfg.read_timeout_ms < 100) {
    throw std::invalid_argument("Invalid read_timeout_ms: must be >= 100");
  }
  if (cfg.reconnect_base_ms < 50) {
    throw std::invalid_argument("Invalid reconnect_base_ms: must be >= 50");
  }
  if (cfg.reconnect_max_ms < cfg.reconnect_base_ms) {
    throw std::invalid_argument(
        "Invalid reconnect_max_ms: must be >= reconnect_base_ms");
  }
  if (cfg.profiling_log_interval_sec < 1) {
    throw std::invalid_argument(
        "Invalid profiling_log_interval_sec: must be >= 1");
  }
}

} //< namespace streamer
