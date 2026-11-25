#include "inference/detection.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace streamer {

static const char *const kCocoClassNames[80] = {
    "person",        "bicycle",      "car",
    "motorcycle",    "airplane",     "bus",
    "train",         "truck",        "boat",
    "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench",        "bird",
    "cat",           "dog",          "horse",
    "sheep",         "cow",          "elephant",
    "bear",          "zebra",        "giraffe",
    "backpack",      "umbrella",     "handbag",
    "tie",           "suitcase",     "frisbee",
    "skis",          "snowboard",    "sports ball",
    "kite",          "baseball bat", "baseball glove",
    "skateboard",    "surfboard",    "tennis racket",
    "bottle",        "wine glass",   "cup",
    "fork",          "knife",        "spoon",
    "bowl",          "banana",       "apple",
    "sandwich",      "orange",       "broccoli",
    "carrot",        "hot dog",      "pizza",
    "donut",         "cake",         "chair",
    "couch",         "potted plant", "bed",
    "dining table",  "toilet",       "tv",
    "laptop",        "mouse",        "remote",
    "keyboard",      "cell phone",   "microwave",
    "oven",          "toaster",      "sink",
    "refrigerator",  "book",         "clock",
    "vase",          "scissors",     "teddy bear",
    "hair drier",    "toothbrush"};

const char *coco_class_name(int class_id) {
  if (class_id < 0 || class_id >= 80) {
    return "unknown";
  }
  return kCocoClassNames[class_id];
}

LetterboxInfo letterbox_rgb_to_nchw(const uint8_t *rgb, int src_w, int src_h,
                                    int dst_w, int dst_h, float *out_nchw) {
  LetterboxInfo lb;
  lb.dst_w = dst_w;
  lb.dst_h = dst_h;
  lb.scale = 1.0f;
  lb.pad_x = 0;
  lb.pad_y = 0;

  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !rgb ||
      !out_nchw) {
    return lb;
  }

  float scale_w = static_cast<float>(dst_w) / static_cast<float>(src_w);
  float scale_h = static_cast<float>(dst_h) / static_cast<float>(src_h);
  float scale = std::min(scale_w, scale_h);

  int new_w = static_cast<int>(std::round(src_w * scale));
  int new_h = static_cast<int>(std::round(src_h * scale));

  int pad_x = (dst_w - new_w) / 2;
  int pad_y = (dst_h - new_h) / 2;

  lb.scale = scale;
  lb.pad_x = pad_x;
  lb.pad_y = pad_y;

  /// Fill out_nchw (3 * dst_w * dst_h floats) with 114.0f / 255.0f
  const float fill_val = 114.0f / 255.0f;
  size_t plane_size = static_cast<size_t>(dst_w) * dst_h;
  for (size_t i = 0; i < 3 * plane_size; ++i) {
    out_nchw[i] = fill_val;
  }

  float *r_plane = out_nchw;
  float *g_plane = out_nchw + plane_size;
  float *b_plane = out_nchw + 2 * plane_size;

  /// Bilinear or nearest sampling from src into letterbox region [pad_y ..
  /// pad_y + new_h) x [pad_x
  /// .. pad_x + new_w)
  for (int y = 0; y < new_h; ++y) {
    int dst_y = pad_y + y;
    if (dst_y < 0 || dst_y >= dst_h)
      continue;

    /// Map y back to src space
    float src_y_f = (y + 0.5f) / scale - 0.5f;
    int sy0 = static_cast<int>(std::floor(src_y_f));
    int sy1 = sy0 + 1;
    float fy = src_y_f - sy0;
    sy0 = std::clamp(sy0, 0, src_h - 1);
    sy1 = std::clamp(sy1, 0, src_h - 1);

    for (int x = 0; x < new_w; ++x) {
      int dst_x = pad_x + x;
      if (dst_x < 0 || dst_x >= dst_w)
        continue;

      float src_x_f = (x + 0.5f) / scale - 0.5f;
      int sx0 = static_cast<int>(std::floor(src_x_f));
      int sx1 = sx0 + 1;
      float fx = src_x_f - sx0;
      sx0 = std::clamp(sx0, 0, src_w - 1);
      sx1 = std::clamp(sx1, 0, src_w - 1);

      /// Assuming rgb is packed RGB24 (3 bytes per pixel)
      auto get_rgb = [&](int px, int py, int channel) {
        size_t idx = static_cast<size_t>(py) * src_w * 3 +
                     static_cast<size_t>(px) * 3 + channel;
        return static_cast<float>(rgb[idx]) / 255.0f;
      };

      for (int c = 0; c < 3; ++c) {
        float c00 = get_rgb(sx0, sy0, c);
        float c10 = get_rgb(sx1, sy0, c);
        float c01 = get_rgb(sx0, sy1, c);
        float c11 = get_rgb(sx1, sy1, c);

        float val = (1.0f - fx) * (1.0f - fy) * c00 + fx * (1.0f - fy) * c10 +
                    (1.0f - fx) * fy * c01 + fx * fy * c11;

        size_t dst_idx = static_cast<size_t>(dst_y) * dst_w + dst_x;
        if (c == 0)
          r_plane[dst_idx] = val;
        else if (c == 1)
          g_plane[dst_idx] = val;
        else
          b_plane[dst_idx] = val;
      }
    }
  }

  return lb;
}

float compute_iou(const Detection &a, const Detection &b) {
  float inter_x1 = std::max(a.x1, b.x1);
  float inter_y1 = std::max(a.y1, b.y1);
  float inter_x2 = std::min(a.x2, b.x2);
  float inter_y2 = std::min(a.y2, b.y2);

  float inter_w = std::max(0.0f, inter_x2 - inter_x1);
  float inter_h = std::max(0.0f, inter_y2 - inter_y1);
  float inter_area = inter_w * inter_h;

  if (inter_area <= 0.0f) {
    return 0.0f;
  }

  float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
  float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);

  float union_area = area_a + area_b - inter_area;
  if (union_area <= 0.0f) {
    return 0.0f;
  }

  return inter_area / union_area;
}

std::vector<Detection> nms_per_class(std::vector<Detection> detections,
                                     float iou_threshold) {
  if (detections.empty()) {
    return {};
  }

  /// Sort descending by confidence
  std::sort(detections.begin(), detections.end(),
            [](const Detection &a, const Detection &b) {
              return a.confidence > b.confidence;
            });

  std::vector<Detection> result;
  std::vector<bool> suppressed(detections.size(), false);

  for (size_t i = 0; i < detections.size(); ++i) {
    if (suppressed[i])
      continue;
    result.push_back(detections[i]);

    for (size_t j = i + 1; j < detections.size(); ++j) {
      if (suppressed[j])
        continue;
      if (detections[i].class_id == detections[j].class_id) {
        float iou = compute_iou(detections[i], detections[j]);
        if (iou > iou_threshold) {
          suppressed[j] = true;
        }
      }
    }
  }

  return result;
}

std::vector<Detection>
decode_yolov8_output(const float *output, int num_classes, int num_anchors,
                     const LetterboxInfo &lb, int orig_w, int orig_h,
                     float conf_threshold, float iou_threshold,
                     const std::unordered_set<std::string> &target_classes) {
  if (!output || num_classes <= 0 || num_anchors <= 0 || orig_w <= 0 ||
      orig_h <= 0) {
    return {};
  }

  /// Output tensor is row-major [4 + num_classes, num_anchors]
  /// row 0: cx, row 1: cy, row 2: w, row 3: h, rows 4 + c: class c score for
  /// anchor a.
  std::vector<Detection> candidates;
  candidates.reserve(num_anchors);

  for (int a = 0; a < num_anchors; ++a) {
    /// Find max class score & class_id
    float max_score = 0.0f;
    int best_cid = -1;

    for (int c = 0; c < num_classes; ++c) {
      float score = output[(4 + c) * num_anchors + a];
      if (score > max_score) {
        max_score = score;
        best_cid = c;
      }
    }

    if (max_score < conf_threshold) {
      continue;
    }

    const char *label_ptr = coco_class_name(best_cid);
    std::string label(label_ptr);

    if (!target_classes.empty() &&
        target_classes.find(label) == target_classes.end()) {
      continue;
    }

    float cx = output[0 * num_anchors + a];
    float cy = output[1 * num_anchors + a];
    float w = output[2 * num_anchors + a];
    float h = output[3 * num_anchors + a];

    /// Convert from letterbox space to original image coordinates
    float x1 = ((cx - w * 0.5f) - lb.pad_x) / lb.scale;
    float y1 = ((cy - h * 0.5f) - lb.pad_y) / lb.scale;
    float x2 = ((cx + w * 0.5f) - lb.pad_x) / lb.scale;
    float y2 = ((cy + h * 0.5f) - lb.pad_y) / lb.scale;

    /// Clamp to [0, orig_w] and [0, orig_h]
    x1 = std::clamp(x1, 0.0f, static_cast<float>(orig_w));
    y1 = std::clamp(y1, 0.0f, static_cast<float>(orig_h));
    x2 = std::clamp(x2, 0.0f, static_cast<float>(orig_w));
    y2 = std::clamp(y2, 0.0f, static_cast<float>(orig_h));

    if (x2 <= x1 || y2 <= y1) {
      continue;
    }

    Detection det;
    det.class_id = best_cid;
    det.label = label;
    det.confidence = max_score;
    det.x1 = x1;
    det.y1 = y1;
    det.x2 = x2;
    det.y2 = y2;

    candidates.push_back(det);
  }

  return nms_per_class(std::move(candidates), iou_threshold);
}

} //< namespace streamer
