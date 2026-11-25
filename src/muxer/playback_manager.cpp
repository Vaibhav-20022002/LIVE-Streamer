#include "muxer/playback_manager.hpp"

#include "capture/av_raii.hpp"
#include "common/logger.hpp"
#include "pipeline/stats_writer.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace streamer {

namespace {

std::string trim_whitespace(const std::string &s) {
  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(start, end - start);
}

bool starts_with(const std::string &s, const char *prefix) {
  const size_t len = std::char_traits<char>::length(prefix);
  return s.size() >= len && s.compare(0, len, prefix) == 0;
}

bool is_init_mp4_filename(const std::string &fname) {
  constexpr const char *kPrefix = "init_";
  constexpr const char *kSuffix = ".mp4";
  const size_t prefix_len = 5;
  const size_t suffix_len = 4;
  return fname.size() > prefix_len + suffix_len &&
         fname.compare(0, prefix_len, kPrefix) == 0 &&
         fname.compare(fname.size() - suffix_len, suffix_len, kSuffix) == 0;
}

} //< namespace

PlaybackManager::PlaybackManager(std::filesystem::path hls_dir,
                                 int retention_sec)
    : hls_dir_(std::move(hls_dir)), retention_sec_(std::max(0, retention_sec)) {
}

int PlaybackManager::extract_segment_number(const std::string &filename) {
  constexpr const char *kExt = ".m4s";
  constexpr size_t kExtLen = 4;
  if (filename.size() <= kExtLen ||
      filename.compare(filename.size() - kExtLen, kExtLen, kExt) != 0) {
    return -1;
  }

  size_t end = filename.size() - kExtLen;
  size_t start = end;
  while (start > 0 &&
         std::isdigit(static_cast<unsigned char>(filename[start - 1]))) {
    --start;
  }
  if (start == end) {
    return -1;
  }

  int value = -1;
  const char *first = filename.data() + start;
  const char *last = filename.data() + end;
  auto res = std::from_chars(first, last, value);
  if (res.ec != std::errc{} || res.ptr != last) {
    return -1;
  }
  return value;
}

bool PlaybackManager::sync_from_live_playlist() {
  const std::filesystem::path live_path = hls_dir_ / "live.m3u8";
  std::ifstream ifs(live_path);
  if (!ifs.is_open()) {
    return false;
  }

  live_filenames_.clear();

  std::string map_uri;
  bool seen_discont = false;
  double pending_dur = 0.0;
  bool has_extinf = false;

  std::string line;
  while (std::getline(ifs, line)) {
    const std::string trimmed = trim_whitespace(line);
    if (trimmed.empty()) {
      continue;
    }

    if (starts_with(trimmed, "#EXT-X-MAP:")) {
      const size_t uri_pos = trimmed.find("URI=\"");
      if (uri_pos != std::string::npos) {
        const size_t val_start = uri_pos + 5;
        const size_t val_end = trimmed.find('"', val_start);
        if (val_end != std::string::npos) {
          map_uri = trimmed.substr(val_start, val_end - val_start);
        }
      }
      continue;
    }

    if (trimmed == "#EXT-X-DISCONTINUITY") {
      seen_discont = true;
      continue;
    }

    if (starts_with(trimmed, "#EXTINF:")) {
      const char *num_start = trimmed.c_str() + 8;
      char *num_end = nullptr;
      const double dur = std::strtod(num_start, &num_end);
      if (num_end != num_start && std::isfinite(dur) && dur >= 0.0) {
        pending_dur = dur;
        has_extinf = true;
      }
      continue;
    }

    if (trimmed[0] == '#') {
      continue;
    }

    /// Segment URI line
    const std::string &filename = trimmed;
    live_filenames_.insert(filename);

    if (known_filenames_.find(filename) == known_filenames_.end()) {
      SegmentEntry entry;
      entry.filename = filename;
      entry.init_uri = !map_uri.empty() ? map_uri : current_init_uri_;
      entry.duration_sec = has_extinf ? pending_dur : 0.0;
      entry.start_epoch_ms = now_epoch_ms();
      entry.discontinuity = pending_discontinuity_ || seen_discont;
      entry.seg_number = extract_segment_number(filename);

      pending_discontinuity_ = false;
      if (entry.seg_number > highest_seg_num_) {
        highest_seg_num_ = entry.seg_number;
      }

      known_filenames_.insert(filename);
      segments_.push_back(std::move(entry));
    }

    /// Reset per-segment tags so an already-known segment's tags do not bleed
    /// onto the next segment.
    seen_discont = false;
    pending_dur = 0.0;
    has_extinf = false;
  }

  return true;
}

void PlaybackManager::mark_discontinuity(const std::string &new_init_uri) {
  pending_discontinuity_ = true;
  if (!new_init_uri.empty()) {
    current_init_uri_ = new_init_uri;
  }
}

void PlaybackManager::record_detections(int64_t epoch_ms,
                                        const std::vector<Detection> &dets) {
  if (dets.empty()) {
    return;
  }
  if (last_event_epoch_ms_ != 0 && epoch_ms - last_event_epoch_ms_ < 1000) {
    return;
  }

  std::vector<std::pair<std::string, float>> objects;
  objects.reserve(dets.size());
  for (const auto &det : dets) {
    if (det.label.empty()) {
      continue;
    }
    bool found = false;
    for (auto &obj : objects) {
      if (obj.first == det.label) {
        if (det.confidence > obj.second) {
          obj.second = det.confidence;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      objects.emplace_back(det.label, det.confidence);
    }
  }

  if (objects.empty()) {
    return;
  }

  last_event_epoch_ms_ = epoch_ms;

  DetectionEvent ev;
  ev.epoch_ms = epoch_ms;
  ev.media_time_sec = total_duration_sec();
  ev.segment = segments_.empty() ? std::string() : segments_.back().filename;
  ev.objects = std::move(objects);

  {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2)
       << "Indexed detection | dvr_t=" << ev.media_time_sec
       << "s seg=" << (ev.segment.empty() ? "pending" : ev.segment)
       << " objects=[";
    for (size_t i = 0; i < ev.objects.size(); ++i) {
      if (i > 0)
        ss << ", ";
      ss << ev.objects[i].first << ":" << ev.objects[i].second;
    }
    ss << "]";
    log_info("ai", ss.str());
  }

  events_.push_back(std::move(ev));
}

size_t PlaybackManager::prune_expired() {
  double total_dur = total_duration_sec();
  double removed_dur = 0.0;
  size_t removed_count = 0;

  while (segments_.size() > 1 &&
         total_dur > static_cast<double>(retention_sec_)) {
    const SegmentEntry &front = segments_.front();
    if (live_filenames_.count(front.filename) > 0) {
      /// Never delete segments still referenced by live.m3u8.
      break;
    }

    const double seg_dur = front.duration_sec;
    const std::string seg_file = front.filename;

    total_dur -= seg_dur;
    removed_dur += seg_dur;
    pruned_duration_offset_sec_ += seg_dur;

    std::error_code ec;
    std::filesystem::remove(hls_dir_ / seg_file, ec);

    known_filenames_.erase(seg_file);
    segments_.erase(segments_.begin());
    ++media_sequence_;
    ++removed_count;
  }

  /// Shift DVR seek offsets so clicking an event in Enhanced Object Search still
  /// aligns with playback.m3u8 after front segments roll off the window.
  if (removed_dur > 0.0 && !events_.empty()) {
    for (auto &ev : events_) {
      ev.media_time_sec -= removed_dur;
    }
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [](const DetectionEvent &ev) {
                                   return ev.media_time_sec < 0.0;
                                 }),
                  events_.end());
  }

  if (removed_count > 0) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1)
       << "Pruned expired segments | count=" << removed_count
       << " removed_sec=" << removed_dur
       << " remaining_segs=" << segments_.size()
       << " media_seq=" << media_sequence_;
    log_info("dvr", ss.str());
  }

  /// Clean up unreferenced init_*.mp4 files.
  std::unordered_set<std::string> referenced_inits;
  if (!current_init_uri_.empty()) {
    referenced_inits.insert(current_init_uri_);
  }
  for (const auto &seg : segments_) {
    if (!seg.init_uri.empty()) {
      referenced_inits.insert(seg.init_uri);
    }
  }

  std::error_code dir_ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(hls_dir_, dir_ec)) {
    if (dir_ec) {
      break;
    }
    std::error_code type_ec;
    if (!entry.is_regular_file(type_ec) || type_ec) {
      continue;
    }
    const std::string fname = entry.path().filename().string();
    if (is_init_mp4_filename(fname) &&
        referenced_inits.find(fname) == referenced_inits.end()) {
      std::error_code rm_ec;
      std::filesystem::remove(entry.path(), rm_ec);
    }
  }

  return removed_count;
}

bool PlaybackManager::write_playback_manifest() const {
  if (segments_.empty()) {
    return true;
  }

  double max_dur = 0.0;
  for (const auto &seg : segments_) {
    if (seg.duration_sec > max_dur) {
      max_dur = seg.duration_sec;
    }
  }
  const int target_dur = std::max(2, static_cast<int>(std::ceil(max_dur)));

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6);
  oss << "#EXTM3U\n"
      << "#EXT-X-VERSION:7\n"
      << "#EXT-X-TARGETDURATION:" << target_dur << "\n"
      << "#EXT-X-MEDIA-SEQUENCE:" << media_sequence_ << "\n"
      << "#EXT-X-INDEPENDENT-SEGMENTS\n";

  for (size_t i = 0; i < segments_.size(); ++i) {
    const SegmentEntry &seg = segments_[i];
    const bool emit_discont = (i > 0 && seg.discontinuity);
    const bool emit_map =
        (i == 0) || emit_discont || (seg.init_uri != segments_[i - 1].init_uri);

    if (emit_discont) {
      oss << "#EXT-X-DISCONTINUITY\n";
    }
    if (emit_map && !seg.init_uri.empty()) {
      oss << "#EXT-X-MAP:URI=\"" << seg.init_uri << "\"\n";
    }
    oss << "#EXTINF:" << seg.duration_sec << ",\n" << seg.filename << "\n";
  }

  return write_atomic_file(hls_dir_ / "playback.m3u8", oss.str());
}

bool PlaybackManager::write_events_json() const {
  std::ostringstream oss;
  oss << std::fixed;
  oss << "{\n"
      << "  \"retention_sec\": " << retention_sec_ << ",\n"
      << "  \"events\": [";

  for (size_t i = 0; i < events_.size(); ++i) {
    const DetectionEvent &ev = events_[i];
    if (i > 0) {
      oss << ",";
    }
    oss << "\n    {\n"
        << "      \"timestamp\": " << std::setprecision(3)
        << (static_cast<double>(ev.epoch_ms) / 1000.0) << ",\n"
        << "      \"epoch_ms\": " << ev.epoch_ms << ",\n"
        << "      \"media_time_sec\": " << std::setprecision(3)
        << ev.media_time_sec << ",\n"
        << "      \"segment_name\": \"" << escape_json_string(ev.segment)
        << "\",\n"
        << "      \"detections\": [";

    for (size_t j = 0; j < ev.objects.size(); ++j) {
      const auto &obj = ev.objects[j];
      if (j > 0) {
        oss << ",";
      }
      oss << "{\"label\": \"" << escape_json_string(obj.first)
          << "\", \"confidence\": " << std::setprecision(4) << obj.second
          << "}";
    }

    oss << "]\n    }";
  }

  if (!events_.empty()) {
    oss << "\n  ";
  }
  oss << "]\n}\n";

  return write_atomic_file(hls_dir_ / "events.json", oss.str());
}

int PlaybackManager::next_segment_number() const {
  return highest_seg_num_ + 1;
}

double PlaybackManager::total_duration_sec() const {
  double total = 0.0;
  for (const auto &seg : segments_) {
    total += seg.duration_sec;
  }
  return total;
}

} //< namespace streamer
