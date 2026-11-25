#pragma once

/// @file yuv_overlay.hpp
/// @brief Zero-RGB-roundtrip planar YUV420P bounding box and text overlay
/// engine header.

#include "inference/detection.hpp"
#include <cstdint>
#include <vector>

namespace streamer {

/// @brief Draws bounding boxes and class/confidence labels directly onto
/// YUV420P planes
///        (avoiding any RGB roundtrips), using 2x2 chroma-subsampled U/V blocks
///        and built-in 5x7 bitmap font rendering with safe coordinate clipping.
void draw_detections_yuv420p(uint8_t *y_plane,
    int                               y_stride,
    uint8_t                          *u_plane,
    int                               u_stride,
    uint8_t                          *v_plane,
    int                               v_stride,
    int                               width,
    int                               height,
    const std::vector<Detection>     &detections);

} //< namespace streamer
