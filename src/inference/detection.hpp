#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace streamer {

struct Detection {
  int         class_id = -1;
  std::string label;
  float       confidence = 0.0f;
  float       x1         = 0.0f;
  float       y1         = 0.0f;
  float       x2         = 0.0f;
  float       y2         = 0.0f;
};

struct LetterboxInfo {
  float scale = 1.0f;
  int   pad_x = 0;
  int   pad_y = 0;
  int   dst_w = 320;
  int   dst_h = 320;
};

const char *coco_class_name(int class_id);

LetterboxInfo letterbox_rgb_to_nchw(const uint8_t *rgb,
    int                                            src_w,
    int                                            src_h,
    int                                            dst_w,
    int                                            dst_h,
    float                                         *out_nchw);

float compute_iou(const Detection &a, const Detection &b);

std::vector<Detection> nms_per_class(std::vector<Detection> detections, float iou_threshold);

std::vector<Detection> decode_yolov8_output(const float *output,
    int                                                  num_classes,
    int                                                  num_anchors,
    const LetterboxInfo                                 &lb,
    int                                                  orig_w,
    int                                                  orig_h,
    float                                                conf_threshold,
    float                                                iou_threshold,
    const std::unordered_set<std::string>               &target_classes);

} //< namespace streamer
