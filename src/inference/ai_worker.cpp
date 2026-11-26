#include "inference/ai_worker.hpp"

#include "common/logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#if defined(ENABLE_AI) && ENABLE_AI == 1
#include <array>
#include <filesystem>
#include <onnxruntime_cxx_api.h>
#include <sstream>
#endif

namespace streamer {

namespace {

constexpr int   kDefaultModelWidth      = 320;
constexpr int   kDefaultModelHeight     = 320;
constexpr int   kCocoNumClasses         = 80;
constexpr int   kYolov8NumAnchors320    = 2100;
constexpr float kDefaultNmsIouThreshold = 0.45f;

} //< namespace

struct AiWorker::Impl {
  int                model_w = kDefaultModelWidth;
  int                model_h = kDefaultModelHeight;
  std::vector<float> input_tensor;
  std::vector<float> synthetic_output;

#if defined(ENABLE_AI) && ENABLE_AI == 1
  std::unique_ptr<Ort::Session> session;
  std::string                   input_name;
  std::string                   output_name;
  std::vector<float>            transposed_output;
#endif
};

#if defined(ENABLE_AI) && ENABLE_AI == 1
namespace {
Ort::Env &shared_ort_env() {
  static Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "live_streamer"};
  return env;
}
} //< namespace
#endif

AiWorker::AiWorker(const Config &cfg, StageProfiler &profiler)
    : cfg_(cfg)
    , profiler_(profiler)
    , impl_(std::make_unique<Impl>()) {
  impl_->input_tensor.assign(static_cast<size_t>(3) * impl_->model_w * impl_->model_h, 0.0f);

  if (!cfg_.ai_enabled) {
    backend_name_ = "disabled";
    active_       = false;
    return;
  }

#if defined(ENABLE_AI) && ENABLE_AI == 1
  std::error_code ec;
  std::string     model_path = cfg_.ai_model_path;
  if (model_path.empty() || !std::filesystem::exists(model_path, ec)) {
    if (std::filesystem::exists("/opt/models/yolov8n.onnx", ec)) {
      model_path = "/opt/models/yolov8n.onnx";
      log_info("ai",
          "Configured model not found at '" + cfg_.ai_model_path +
              "'; falling back to '/opt/models/yolov8n.onnx'");
    }
  }

  if (!model_path.empty() && std::filesystem::exists(model_path, ec)) {
    try {
      Ort::SessionOptions opts;
      opts.SetIntraOpNumThreads(std::max(1, cfg_.ai_threads));
      opts.SetInterOpNumThreads(1);
      /// Prevent ORT worker threads from busy-spinning between inference calls.
      opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
      opts.AddConfigEntry("session.inter_op.allow_spinning", "0");
      opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

      impl_->session = std::make_unique<Ort::Session>(shared_ort_env(), model_path.c_str(), opts);

      Ort::AllocatorWithDefaultOptions allocator;
      if (impl_->session->GetInputCount() > 0) {
        auto in_name = impl_->session->GetInputNameAllocated(0, allocator);
        if (in_name) {
          impl_->input_name = in_name.get();
        }
        /// Keep Ort::TypeInfo alive; GetTensorTypeAndShapeInfo() returns a
        /// view.
        auto in_type_info = impl_->session->GetInputTypeInfo(0);
        auto in_info      = in_type_info.GetTensorTypeAndShapeInfo();
        auto shape        = in_info.GetShape();
        if (shape.size() == 4) {
          if (shape[2] > 0) {
            impl_->model_h = static_cast<int>(shape[2]);
          }
          if (shape[3] > 0) {
            impl_->model_w = static_cast<int>(shape[3]);
          }
          impl_->input_tensor.assign(
              static_cast<size_t>(3) * impl_->model_w * impl_->model_h, 0.0f);
        }
      }
      if (impl_->session->GetOutputCount() > 0) {
        auto out_name = impl_->session->GetOutputNameAllocated(0, allocator);
        if (out_name) {
          impl_->output_name = out_name.get();
        }
      }

      if (!impl_->input_name.empty() && !impl_->output_name.empty()) {
        backend_name_ = "onnxruntime";
        active_       = true;
        std::ostringstream ss;
        ss << "Loaded ONNX model '" << model_path << "' input=" << impl_->input_name << " ("
           << impl_->model_w << "x" << impl_->model_h << ") output=" << impl_->output_name
           << " threads=" << std::max(1, cfg_.ai_threads);
        log_info("ai", ss.str());
      } else {
        log_warn("ai", "ONNX model missing input/output tensor names: " + model_path);
        impl_->session.reset();
      }
    } catch (const Ort::Exception &e) {
      log_error("ai", "Failed to load ONNX Runtime model '" + model_path + "': " + e.what());
      impl_->session.reset();
    }
  }
#endif

  if (!active_) {
    if (cfg_.synthetic_detections) {
      backend_name_ = "synthetic";
      active_       = true;
      impl_->synthetic_output.assign(
          static_cast<size_t>(4 + kCocoNumClasses) * kYolov8NumAnchors320, 0.0f);
      log_info("ai", "Synthetic detection backend enabled");
    } else {
      log_warn("ai",
          "Model unavailable at '" + cfg_.ai_model_path +
              "' and '/opt/models/yolov8n.onnx'. Export yolov8n_320.onnx "
              "via scripts/export_yolov8n.py or set "
              "SYNTHETIC_DETECTIONS=true.");
      backend_name_ = "disabled";
      active_       = false;
    }
  }
}

AiWorker::~AiWorker() {
  stop();
}

void AiWorker::start() {
  if (!active_ || running_.exchange(true)) {
    return;
  }
  queue_.reset();
  thread_ = std::thread(&AiWorker::worker_loop, this);
}

void AiWorker::stop() {
  running_.store(false);
  queue_.shutdown();
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool AiWorker::prepare_job(const uint8_t *rgb_data,
    int                                   stride,
    int                                   width,
    int                                   height,
    InferenceJob                         &out_job) {
  if (!active_ || !rgb_data || width <= 0 || height <= 0) {
    return false;
  }
  const size_t row_bytes = static_cast<size_t>(width) * 3;
  if (stride < static_cast<int>(row_bytes)) {
    return false;
  }
  const size_t frame_bytes = row_bytes * static_cast<size_t>(height);

  std::shared_ptr<std::vector<uint8_t>> buf;
  if (cfg_.use_buffer_pool) {
    if (!buffer_pool_ || buffer_pool_->buffer_bytes() != frame_bytes) {
      buffer_pool_ = ByteBufferPool::create(frame_bytes, 4, 16);
    }
    buf = buffer_pool_->acquire();
  } else {
    buf = std::make_shared<std::vector<uint8_t>>(frame_bytes);
  }

  if (stride == static_cast<int>(row_bytes)) {
    std::memcpy(buf->data(), rgb_data, frame_bytes);
  } else {
    for (int y = 0; y < height; ++y) {
      std::memcpy(buf->data() + static_cast<size_t>(y) * row_bytes,
          rgb_data + static_cast<size_t>(y) * stride,
          row_bytes);
    }
  }

  out_job.rgb_buf = std::move(buf);
  out_job.width   = width;
  out_job.height  = height;
  out_job.seq     = ++job_seq_;
  return true;
}

void AiWorker::submit_rgb_frame(const uint8_t *rgb_data, int stride, int width, int height) {
  if (!running_.load()) {
    return;
  }
  InferenceJob job;
  if (prepare_job(rgb_data, stride, width, height, job)) {
    queue_.push(std::move(job));
  }
}

std::vector<Detection>
AiWorker::infer_sync(const uint8_t *rgb_data, int stride, int width, int height) {
  InferenceJob job;
  if (!prepare_job(rgb_data, stride, width, height, job)) {
    return {};
  }

  std::vector<Detection> detections = run_inference_on_job(job);
  {
    std::lock_guard<std::mutex> lock(det_mu_);
    latest_detections_ = detections;
  }
  return detections;
}

std::vector<Detection> AiWorker::latest_detections() const {
  std::lock_guard<std::mutex> lock(det_mu_);
  return latest_detections_;
}

void AiWorker::worker_loop() {
  InferenceJob job;
  while (running_.load() && queue_.pop_wait(job)) {
    busy_.store(true, std::memory_order_relaxed);
    std::vector<Detection> detections = run_inference_on_job(job);
    busy_.store(false, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(det_mu_);
      latest_detections_ = std::move(detections);
    }
  }
}

std::vector<Detection>
AiWorker::generate_synthetic_detections(int width, int height, uint64_t seq) const {
  if (width <= 0 || height <= 0) {
    return {};
  }

  const float wf = static_cast<float>(width);
  const float hf = static_cast<float>(height);
  const float t  = static_cast<float>(seq) * 0.08f;

  auto make_box = [&](int                     class_id,
                      float                   conf,
                      float                   cx_norm,
                      float                   cy_norm,
                      float                   bw_norm,
                      float                   bh_norm,
                      std::vector<Detection> &out) {
    if (conf < cfg_.ai_conf_threshold) {
      return;
    }
    const char *name = coco_class_name(class_id);
    std::string label(name);
    if (!cfg_.ai_target_classes.empty() &&
        cfg_.ai_target_classes.find("*") == cfg_.ai_target_classes.end() &&
        cfg_.ai_target_classes.find(label) == cfg_.ai_target_classes.end()) {
      return;
    }
    const float cx = cx_norm * wf;
    const float cy = cy_norm * hf;
    const float bw = bw_norm * wf;
    const float bh = bh_norm * hf;

    Detection det;
    det.class_id   = class_id;
    det.label      = std::move(label);
    det.confidence = conf;
    det.x1         = std::clamp(cx - bw * 0.5f, 0.0f, wf);
    det.y1         = std::clamp(cy - bh * 0.5f, 0.0f, hf);
    det.x2         = std::clamp(cx + bw * 0.5f, 0.0f, wf);
    det.y2         = std::clamp(cy + bh * 0.5f, 0.0f, hf);
    if (det.x2 > det.x1 && det.y2 > det.y1) {
      out.push_back(std::move(det));
    }
  };

  std::vector<Detection> result;
  make_box(0,
      0.87f,
      0.30f + 0.14f * std::sin(t),
      0.52f + 0.05f * std::cos(t * 0.5f),
      0.14f,
      0.36f,
      result);
  make_box(2,
      0.81f,
      0.68f + 0.12f * std::cos(t * 0.7f),
      0.60f + 0.04f * std::sin(t * 0.6f),
      0.24f,
      0.22f,
      result);
  return result;
}

std::vector<Detection> AiWorker::run_inference_on_job(const InferenceJob &job) {
  if (!job.rgb_buf || job.width <= 0 || job.height <= 0) {
    return {};
  }

  LetterboxInfo lb;
  {
    StageProfiler::ScopedTimer timer(profiler_, Stage::Preprocess);
    lb = letterbox_rgb_to_nchw(job.rgb_buf->data(),
        job.width,
        job.height,
        impl_->model_w,
        impl_->model_h,
        impl_->input_tensor.data());
  }

  std::vector<Detection> detections;
  {
    StageProfiler::ScopedTimer timer(profiler_, Stage::Inference);
#if defined(ENABLE_AI) && ENABLE_AI == 1
    if (backend_name_ == "onnxruntime" && impl_->session) {
      try {
        std::array<int64_t, 4> input_shape{1, 3, impl_->model_h, impl_->model_w};
        Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value      input_val = Ort::Value::CreateTensor<float>(mem_info,
            impl_->input_tensor.data(),
            impl_->input_tensor.size(),
            input_shape.data(),
            input_shape.size());

        const char *input_names[]  = {impl_->input_name.c_str()};
        const char *output_names[] = {impl_->output_name.c_str()};

        auto output_tensors = impl_->session->Run(
            Ort::RunOptions{nullptr}, input_names, &input_val, 1, output_names, 1);

        if (!output_tensors.empty() && output_tensors[0].IsTensor()) {
          auto         info    = output_tensors[0].GetTensorTypeAndShapeInfo();
          auto         shape   = info.GetShape();
          const float *raw_out = output_tensors[0].GetTensorData<float>();
          if (shape.size() == 3 && shape[0] == 1 && raw_out) {
            const int dim1 = static_cast<int>(shape[1]);
            const int dim2 = static_cast<int>(shape[2]);
            if (dim1 <= dim2 && dim1 > 4) {
              detections = decode_yolov8_output(raw_out,
                  dim1 - 4,
                  dim2,
                  lb,
                  job.width,
                  job.height,
                  cfg_.ai_conf_threshold,
                  kDefaultNmsIouThreshold,
                  cfg_.ai_target_classes);
            } else if (dim2 > 4) {
              const int num_anchors = dim1;
              const int num_rows    = dim2;
              impl_->transposed_output.resize(static_cast<size_t>(num_rows) * num_anchors);
              for (int a = 0; a < num_anchors; ++a) {
                for (int r = 0; r < num_rows; ++r) {
                  impl_->transposed_output[static_cast<size_t>(r) * num_anchors + a] =
                      raw_out[static_cast<size_t>(a) * num_rows + r];
                }
              }
              detections = decode_yolov8_output(impl_->transposed_output.data(),
                  num_rows - 4,
                  num_anchors,
                  lb,
                  job.width,
                  job.height,
                  cfg_.ai_conf_threshold,
                  kDefaultNmsIouThreshold,
                  cfg_.ai_target_classes);
            }
          }
        }
      } catch (const Ort::Exception &e) {
        log_error("ai", std::string("ONNX Runtime inference error: ") + e.what());
      }
    } else
#endif
        if (backend_name_ == "synthetic") {
      /// Fill 84 x 2100 anchors so synthetic mode runs decode + NMS.
      std::fill(impl_->synthetic_output.begin(), impl_->synthetic_output.end(), 0.0f);
      auto      synth      = generate_synthetic_detections(job.width, job.height, job.seq);
      const int anchors[2] = {312, 1140};
      for (size_t i = 0; i < synth.size() && i < 2; ++i) {
        const Detection &d  = synth[i];
        const float      cx = ((d.x1 + d.x2) * 0.5f) * lb.scale + static_cast<float>(lb.pad_x);
        const float      cy = ((d.y1 + d.y2) * 0.5f) * lb.scale + static_cast<float>(lb.pad_y);
        const float      bw = (d.x2 - d.x1) * lb.scale;
        const float      bh = (d.y2 - d.y1) * lb.scale;

        for (int dup = 0; dup < 2; ++dup) {
          const int a                                           = anchors[i] + dup;
          impl_->synthetic_output[0 * kYolov8NumAnchors320 + a] = cx + static_cast<float>(dup);
          impl_->synthetic_output[1 * kYolov8NumAnchors320 + a] = cy + static_cast<float>(dup);
          impl_->synthetic_output[2 * kYolov8NumAnchors320 + a] = bw;
          impl_->synthetic_output[3 * kYolov8NumAnchors320 + a] = bh;
          impl_->synthetic_output[(4 + d.class_id) * kYolov8NumAnchors320 + a] =
              d.confidence - (dup * 0.04f);
        }
      }
      detections = decode_yolov8_output(impl_->synthetic_output.data(),
          kCocoNumClasses,
          kYolov8NumAnchors320,
          lb,
          job.width,
          job.height,
          cfg_.ai_conf_threshold,
          kDefaultNmsIouThreshold,
          cfg_.ai_target_classes);
    }
  }

  ++inference_count_;
  return detections;
}

} //< namespace streamer
