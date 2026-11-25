#include "pipeline/stats_writer.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace streamer {

std::string escape_json_string(const std::string &input) {
  std::string output;
  output.reserve(input.size() + 2);
  for (unsigned char c : input) {
    switch (c) {
      case '\"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (c < 0x20) {
          std::ostringstream oss;
          oss << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
          output += oss.str();
        } else {
          output += static_cast<char>(c);
        }
        break;
    }
  }
  return output;
}

/// Write to a temp file and rename so readers never see partial JSON.
bool write_atomic_file(const std::filesystem::path &path, const std::string &content) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return false;
    }
  }

  std::string           tmp_path_str = path.string() + ".tmp";
  std::filesystem::path tmp_path(tmp_path_str);

  {
    std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.flush();
    if (!ofs.good()) {
      return false;
    }
  }

  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::copy_file(
        tmp_path, path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      std::filesystem::remove(tmp_path, ec);
      return false;
    }
    std::filesystem::remove(tmp_path, ec);
  }

  return true;
}

namespace {

double safe_double(double val) {
  return std::isfinite(val) ? val : 0.0;
}

void serialize_stage_stats(std::ostringstream &oss,
    const char                                *name,
    const StageStats                          &stats,
    bool                                       add_comma) {
  oss << "\"" << name << "\":{"
      << "\"avg_ms\":" << safe_double(stats.avg_ms) << ","
      << "\"p50_ms\":" << safe_double(stats.p50_ms) << ","
      << "\"p95_ms\":" << safe_double(stats.p95_ms) << ","
      << "\"p99_ms\":" << safe_double(stats.p99_ms) << ","
      << "\"count\":" << stats.count << "}";
  if (add_comma) {
    oss << ",";
  }
}

} //< namespace

std::string serialize_stats_json(const PipelineMetrics &m) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3);

  oss << "{";
  oss << "\"state\":\"" << escape_json_string(m.state) << "\",";
  oss << "\"mode\":\"" << escape_json_string(m.mode) << "\",";
  oss << "\"source_type\":\"" << escape_json_string(m.source_type) << "\",";
  oss << "\"width\":" << m.width << ",";
  oss << "\"height\":" << m.height << ",";

  const double fps_in_val  = safe_double(m.fps_in);
  const double fps_out_val = safe_double(m.fps_out);
  oss << "\"ingest_fps\":" << fps_in_val << ",";
  oss << "\"output_fps\":" << fps_out_val << ",";

  oss << "\"frames_processed\":" << m.frames_processed << ",";
  oss << "\"frames_dropped\":" << m.frames_dropped << ",";
  oss << "\"reconnect_count\":" << m.reconnect_count << ",";

  oss << "\"active_live_segments\":" << m.active_live_segments << ",";
  oss << "\"playback_segments\":" << m.playback_segments << ",";
  const double playback_dur = safe_double(m.playback_duration_sec);
  oss << "\"dvr_window_sec\":" << playback_dur << ",";

  oss << "\"ai_enabled\":" << (m.ai_enabled ? "true" : "false") << ",";
  oss << "\"ai_backend\":\"" << escape_json_string(m.ai_backend) << "\",";
  oss << "\"ai_inferences\":" << m.ai_inferences << ",";

  double ai_avg = safe_double(m.inference_stats.avg_ms);
  double ai_p95 = safe_double(m.inference_stats.p95_ms);
  oss << "\"ai_avg_ms\":" << ai_avg << ",";
  oss << "\"ai_p95_ms\":" << ai_p95 << ",";

  oss << "\"active_detections\":[";
  for (size_t i = 0; i < m.latest_detections.size(); ++i) {
    const auto &det = m.latest_detections[i];
    if (i > 0) {
      oss << ",";
    }
    oss << "{"
        << "\"class_id\":" << det.class_id << ","
        << "\"label\":\"" << escape_json_string(det.label) << "\","
        << "\"confidence\":" << safe_double(det.confidence) << ","
        << "\"box\":[" << safe_double(det.x1) << "," << safe_double(det.y1) << ","
        << safe_double(det.x2) << "," << safe_double(det.y2) << "]"
        << "}";
  }
  oss << "],";

  oss << "\"stages\":{";
  serialize_stage_stats(oss, "decode", m.decode_stats, true);
  serialize_stage_stats(oss, "inference", m.inference_stats, true);
  serialize_stage_stats(oss, "encode", m.encode_stats, true);
  serialize_stage_stats(oss, "mux", m.mux_stats, true);
  serialize_stage_stats(oss, "total", m.total_stats, false);
  oss << "},";

  oss << "\"uptime_sec\":" << safe_double(m.uptime_sec) << ",";
  oss << "\"timestamp_epoch_ms\":" << m.timestamp_epoch_ms;

  oss << "}";

  return oss.str();
}

bool write_stats_json(const std::filesystem::path &hls_dir, const PipelineMetrics &m) {
  std::filesystem::path stats_path = hls_dir / "stats.json";
  std::string           json_str   = serialize_stats_json(m);
  return write_atomic_file(stats_path, json_str);
}

} //< namespace streamer
