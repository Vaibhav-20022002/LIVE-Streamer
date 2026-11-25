#pragma once

#include "inference/detection.hpp"
#include <cstdint>
#include <vector>

namespace streamer {

/// Draws bounding boxes and class/confidence labels directly onto YUV420P
/// planes (zero RGB roundtrip), safely clipping all coordinates to frame
/// boundaries.
void draw_detections_yuv420p(uint8_t *y_plane, int y_stride, uint8_t *u_plane,
                             int u_stride, uint8_t *v_plane, int v_stride,
                             int width, int height,
                             const std::vector<Detection> &detections);

} //< namespace streamer
