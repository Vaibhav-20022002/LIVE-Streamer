#pragma once

#include "inference/detection.hpp"
#include <cstdint>
#include <vector>

namespace streamer {

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
