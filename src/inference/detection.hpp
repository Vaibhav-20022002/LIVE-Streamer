#pragma once

/// @file detection.hpp
/// @brief YOLOv8 object detection data structures, letterbox transformation
/// math,
///        Intersection over Union (IoU), per-class NMS, and output tensor
///        decoding.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace streamer {

/// @brief Represents a single detected bounding box with class, label,
/// confidence, and coordinates.
struct Detection {
  int class_id = -1;
  std::string label;
  float confidence = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  float x2 = 0.0f;
  float y2 = 0.0f;
};

/// @brief Holds scaling factor and padding offsets for letterbox resizing
/// preserving aspect ratio.
struct LetterboxInfo {
  float scale = 1.0f;
  int pad_x = 0;
  int pad_y = 0;
  int dst_w = 320;
  int dst_h = 320;
};

/// @brief Returns the COCO class name string for a given class ID [0, 79].
const char *coco_class_name(int class_id);

/// @brief Resizes source RGB frame into destination NCHW float tensor using
/// letterboxing.
LetterboxInfo letterbox_rgb_to_nchw(const uint8_t *rgb, int src_w, int src_h,
                                    int dst_w, int dst_h, float *out_nchw);

/// @brief Computes Intersection over Union (IoU) between two bounding boxes.
float compute_iou(const Detection &a, const Detection &b);

/// @brief Applies greedy Non-Maximum Suppression (NMS) independently per class
/// ID.
std::vector<Detection> nms_per_class(std::vector<Detection> detections,
                                     float iou_threshold);

/// @brief Decodes YOLOv8 ONNX output tensor (handling both [1, 84, N] and [1,
/// N, 84] layouts),
///        applies confidence thresholding, un-letterboxes coordinates back to
///        original frame space, and executes per-class NMS.
std::vector<Detection>
decode_yolov8_output(const float *output, int num_classes, int num_anchors,
                     const LetterboxInfo &lb, int orig_w, int orig_h,
                     float conf_threshold, float iou_threshold,
                     const std::unordered_set<std::string> &target_classes);

} //< namespace streamer
