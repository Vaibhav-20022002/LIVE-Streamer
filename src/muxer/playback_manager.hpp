#pragma once

/// @file playback_manager.hpp
/// @brief DVR playback manager and HLS synchronization header.

#include "inference/detection.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace streamer {

/// @brief Metadata entry for a single completed HLS segment.
struct SegmentEntry {
  std::string filename;
  std::string init_uri;
  double      duration_sec   = 0.0;
  int64_t     start_epoch_ms = 0;
  bool        discontinuity  = false;
  int         seg_number     = -1;
};

/// @brief Detection event mapped to rolling DVR media timeline.
struct DetectionEvent {
  int64_t                                    epoch_ms       = 0;
  double                                     media_time_sec = 0.0;
  std::string                                segment;
  std::vector<std::pair<std::string, float>> objects;
};

/// @brief Manages atomic `playback.m3u8` and `events.json` updates,
/// `#EXT-X-DISCONTINUITY` +
///        `#EXT-X-MAP` stitching, and live-segment-safe DVR pruning.
class PlaybackManager {
public:
  PlaybackManager(std::filesystem::path hls_dir, int retention_sec);

  /// Parses <hls_dir>/live.m3u8 and appends newly completed segments into DVR
  /// list.
  bool sync_from_live_playlist();

  /// Marks an upcoming discontinuity (e.g. after upstream camera reconnect) and
  /// new init file.
  void mark_discontinuity(const std::string &new_init_uri);

  /// Records a throttled detection event (~1/sec) mapped to rolling DVR media
  /// time.
  void record_detections(int64_t epoch_ms, const std::vector<Detection> &dets);

  /// Prunes segments older than retention_sec without touching segments still
  /// in live.m3u8.
  size_t prune_expired();

  bool write_playback_manifest() const;
  bool write_events_json() const;

  int    next_segment_number() const;
  int    active_live_count() const { return static_cast<int>(live_filenames_.size()); }
  int    playback_count() const { return static_cast<int>(segments_.size()); }
  double total_duration_sec() const;
  const std::vector<SegmentEntry>   &segments() const { return segments_; }
  const std::vector<DetectionEvent> &events() const { return events_; }

  static int extract_segment_number(const std::string &filename);

private:
  std::filesystem::path           hls_dir_;
  int                             retention_sec_;
  std::vector<SegmentEntry>       segments_;
  std::unordered_set<std::string> known_filenames_;
  std::unordered_set<std::string> live_filenames_;
  std::vector<DetectionEvent>     events_;
  uint64_t                        media_sequence_             = 0;
  double                          pruned_duration_offset_sec_ = 0.0;
  int64_t                         last_event_epoch_ms_        = 0;
  int                             highest_seg_num_            = -1;
  bool                            pending_discontinuity_      = false;
  std::string                     current_init_uri_           = "init_0.mp4";
};

} //< namespace streamer
