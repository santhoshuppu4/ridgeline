#include "ridgeline/onnx_cpu_detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace ridgeline {

namespace {

float IoU(const Detection& a, const Detection& b) {
  const float ix1 = std::max(a.x1, b.x1);
  const float iy1 = std::max(a.y1, b.y1);
  const float ix2 = std::min(a.x2, b.x2);
  const float iy2 = std::min(a.y2, b.y2);
  const float iw = std::max(0.0f, ix2 - ix1);
  const float ih = std::max(0.0f, iy2 - iy1);
  const float inter = iw * ih;
  const float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
  const float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
  const float uni = area_a + area_b - inter;
  return uni > 0.0f ? inter / uni : 0.0f;
}

// Bilinear sample positions for one axis, using OpenCV's INTER_LINEAR pixel-
// center convention: src = (dst + 0.5) * (src_len / dst_len) - 0.5. Matching
// the convention the model was trained with (cv2.resize) matters: a half-pixel
// shift is small, but it's the kind of silent mismatch that quietly costs
// accuracy and is miserable to track down later.
void BuildAxisTable(int src_len, int dst_len, std::vector<int>& i0, std::vector<int>& i1, std::vector<float>& w) {
  const double scale = static_cast<double>(src_len) / static_cast<double>(dst_len);
  for (int d = 0; d < dst_len; ++d) {
    double f = (d + 0.5) * scale - 0.5;
    if (f < 0.0) f = 0.0;
    int lo = static_cast<int>(std::floor(f));
    if (lo > src_len - 1) lo = src_len - 1;
    const int hi = std::min(lo + 1, src_len - 1);
    i0[static_cast<std::size_t>(d)] = lo;
    i1[static_cast<std::size_t>(d)] = hi;
    w[static_cast<std::size_t>(d)] = static_cast<float>(f - lo);
  }
}

}  // namespace

Ort::SessionOptions OnnxCpuDetector::MakeSessionOptions(const OnnxDetectorConfig& config) {
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(config.intra_op_threads);
  options.SetInterOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  return options;
}

OnnxCpuDetector::OnnxCpuDetector(const OnnxDetectorConfig& config)
    : config_(config),
      env_(ORT_LOGGING_LEVEL_WARNING, "ridgeline"),
      session_options_(MakeSessionOptions(config)),
      session_(env_, config.model_path.c_str(), session_options_),
      memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
  if (session_.GetInputCount() != 1 || session_.GetOutputCount() != 1) {
    throw std::runtime_error("expected a YOLOX model with exactly one input and one output");
  }
  Ort::AllocatorWithDefaultOptions allocator;
  input_name_ = session_.GetInputNameAllocated(0, allocator).get();
  output_name_ = session_.GetOutputNameAllocated(0, allocator).get();

  // Fail at startup, not on frame 1, if the model doesn't match our config.
  const auto in_shape = session_.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
  if (in_shape.size() != 4 || in_shape[1] != 3 || in_shape[2] != config.input_size || in_shape[3] != config.input_size) {
    throw std::runtime_error("model input shape does not match [1,3,input_size,input_size]");
  }

  const auto S = static_cast<std::size_t>(config.input_size);
  input_tensor_.resize(3 * S * S);
  col_x0_.resize(S); col_x1_.resize(S); col_wx_.resize(S);
  row_y0_.resize(S); row_y1_.resize(S); row_wy_.resize(S);

  // Precompute the grid offsets for the three YOLOX strides, in the same
  // row-major order the model emits predictions (y outer, x inner).
  for (int stride : {8, 16, 32}) {
    const int cells = config.input_size / stride;
    for (int y = 0; y < cells; ++y) {
      for (int x = 0; x < cells; ++x) {
        grid_x_.push_back(static_cast<float>(x));
        grid_y_.push_back(static_cast<float>(y));
        grid_stride_.push_back(static_cast<float>(stride));
      }
    }
  }
  candidates_.reserve(256);
}

float OnnxCpuDetector::Preprocess(const Frame& frame) {
  if (frame.format != PixelFormat::kBgr24 || frame.width == 0 || frame.height == 0) {
    throw std::invalid_argument("OnnxCpuDetector requires a non-empty BGR24 frame");
  }
  const int S = config_.input_size;
  const int w = static_cast<int>(frame.width);
  const int h = static_cast<int>(frame.height);
  const float r = std::min(static_cast<float>(S) / static_cast<float>(h), static_cast<float>(S) / static_cast<float>(w));
  // Python's int() truncates toward zero; match YOLOX's preproc exactly.
  const int nw = std::clamp(static_cast<int>(static_cast<float>(w) * r), 1, S);
  const int nh = std::clamp(static_cast<int>(static_cast<float>(h) * r), 1, S);

  // Gray (114) padding everywhere; the resized image overwrites the top-left.
  std::fill(input_tensor_.begin(), input_tensor_.end(), 114.0f);

  BuildAxisTable(w, nw, col_x0_, col_x1_, col_wx_);
  BuildAxisTable(h, nh, row_y0_, row_y1_, row_wy_);

  const std::uint8_t* src = frame.Pixels();
  const std::size_t plane = static_cast<std::size_t>(S) * static_cast<std::size_t>(S);
  const std::size_t stride_bytes = static_cast<std::size_t>(w) * 3;

  for (int dy = 0; dy < nh; ++dy) {
    const auto uy = static_cast<std::size_t>(dy);
    const std::size_t row0 = static_cast<std::size_t>(row_y0_[uy]) * stride_bytes;
    const std::size_t row1 = static_cast<std::size_t>(row_y1_[uy]) * stride_bytes;
    const float wy = row_wy_[uy];
    for (int dx = 0; dx < nw; ++dx) {
      const auto ux = static_cast<std::size_t>(dx);
      const std::size_t c0 = static_cast<std::size_t>(col_x0_[ux]) * 3;
      const std::size_t c1 = static_cast<std::size_t>(col_x1_[ux]) * 3;
      const float wx = col_wx_[ux];
      const std::size_t dst = uy * static_cast<std::size_t>(S) + ux;
      for (std::size_t c = 0; c < 3; ++c) {
        const float top = (1.0f - wx) * src[row0 + c0 + c] + wx * src[row0 + c1 + c];
        const float bot = (1.0f - wx) * src[row1 + c0 + c] + wx * src[row1 + c1 + c];
        // cv2.resize on a uint8 image rounds back to uint8 before YOLOX
        // casts to float32, so round here too.
        input_tensor_[c * plane + dst] = std::round((1.0f - wy) * top + wy * bot);
      }
    }
  }
  return r;
}

std::vector<Detection> OnnxCpuDetector::DetectAll(const Frame& frame) {
  const float r = Preprocess(frame);

  const std::array<std::int64_t, 4> shape{1, 3, config_.input_size, config_.input_size};
  Ort::Value input = Ort::Value::CreateTensor<float>(memory_info_, input_tensor_.data(), input_tensor_.size(),
                                                    shape.data(), shape.size());
  const char* input_names[] = {input_name_.c_str()};
  const char* output_names[] = {output_name_.c_str()};
  auto outputs = session_.Run(Ort::RunOptions{nullptr}, input_names, &input, 1, output_names, 1);

  const auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
  if (out_shape.size() != 3 || static_cast<std::size_t>(out_shape[1]) != grid_x_.size() || out_shape[2] < 6) {
    throw std::runtime_error("unexpected YOLOX output shape");
  }
  const auto rows = static_cast<std::size_t>(out_shape[1]);
  const auto dims = static_cast<std::size_t>(out_shape[2]);
  const std::size_t num_classes = dims - 5;
  const float* out = outputs[0].GetTensorData<float>();

  const float img_w = static_cast<float>(frame.width);
  const float img_h = static_cast<float>(frame.height);

  candidates_.clear();
  for (std::size_t i = 0; i < rows; ++i) {
    const float* p = out + i * dims;
    const float objectness = p[4];
    // score = objectness * class_score, and class_score <= 1, so a row whose
    // objectness is already below threshold can never pass. Skipping it early
    // avoids an 80-way argmax on the ~99% of rows that are background.
    if (objectness <= config_.score_threshold) continue;

    std::size_t best_class = 0;
    float best_class_score = p[5];
    for (std::size_t c = 1; c < num_classes; ++c) {
      if (p[5 + c] > best_class_score) {
        best_class_score = p[5 + c];
        best_class = c;
      }
    }
    const float score = objectness * best_class_score;
    if (score <= config_.score_threshold) continue;

    const float cx = (p[0] + grid_x_[i]) * grid_stride_[i];
    const float cy = (p[1] + grid_y_[i]) * grid_stride_[i];
    const float bw = std::exp(p[2]) * grid_stride_[i];
    const float bh = std::exp(p[3]) * grid_stride_[i];

    Detection d;
    d.class_id = static_cast<int>(best_class);
    d.score = score;
    d.x1 = std::clamp((cx - bw / 2.0f) / r, 0.0f, img_w);
    d.y1 = std::clamp((cy - bh / 2.0f) / r, 0.0f, img_h);
    d.x2 = std::clamp((cx + bw / 2.0f) / r, 0.0f, img_w);
    d.y2 = std::clamp((cy + bh / 2.0f) / r, 0.0f, img_h);
    candidates_.push_back(d);
  }

  // Greedy class-agnostic NMS: highest score first; drop anything that
  // overlaps an already-kept box too much.
  std::sort(candidates_.begin(), candidates_.end(),
            [](const Detection& a, const Detection& b) { return a.score > b.score; });
  std::vector<Detection> kept;
  for (const auto& cand : candidates_) {
    bool suppressed = false;
    for (const auto& k : kept) {
      if (IoU(cand, k) > config_.nms_iou_threshold) {
        suppressed = true;
        break;
      }
    }
    if (!suppressed) kept.push_back(cand);
  }
  return kept;
}

DetectionResult OnnxCpuDetector::Detect(const Frame& frame) {
  DetectionResult result;
  const auto detections = DetectAll(frame);
  for (const auto& d : detections) {  // Sorted by score, so the first match is the best.
    const bool wanted = config_.target_class_ids.empty() ||
                        std::find(config_.target_class_ids.begin(), config_.target_class_ids.end(), d.class_id) !=
                            config_.target_class_ids.end();
    if (!wanted) continue;
    result.positive = true;
    result.confidence = d.score;
    result.x_min = d.x1 / static_cast<float>(frame.width);
    result.y_min = d.y1 / static_cast<float>(frame.height);
    result.x_max = d.x2 / static_cast<float>(frame.width);
    result.y_max = d.y2 / static_cast<float>(frame.height);
    break;
  }
  return result;
}

}  // namespace ridgeline
