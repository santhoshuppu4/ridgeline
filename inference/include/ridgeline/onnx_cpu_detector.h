#pragma once

#include <onnxruntime_cxx_api.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ridgeline/detector.h"
#include "ridgeline/frame.h"

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES — read before the .cc file.
//
// WHAT THIS IS: a real Detector (see detector.h) that runs a YOLOX model with
// ONNX Runtime on the CPU. It plugs into the same pipeline FakeDetector does;
// nothing upstream (capture, ring buffer) or downstream (K-of-N, gRPC) knows
// or cares which one it's talking to.
//
// IMPORTANT, AND EASY TO MISREPRESENT: the stock yolox_nano.onnx model is
// trained on COCO — 80 everyday classes like person, car, dog. COCO has NO
// "smoke" or "fire" class. So Phase 1b-ii proves the inference PLUMBING works
// on real frames and gives a REAL per-frame latency number on your CPU. It
// does NOT detect wildfire smoke. That requires training/fine-tuning on a
// smoke dataset (e.g. HPWREN's FIgLib), which is a later phase. Never
// describe this build as "smoke detection" on a resume or in an interview.
//
// THE THREE STAGES, each of which must match how the model was trained:
//
// 1. Preprocess (must match YOLOX's `preproc` in yolox/data/data_augment.py):
//    - Scale the BGR image by r = min(416/h, 416/w), keeping aspect ratio.
//    - Paste it at the TOP-LEFT of a 416x416 canvas filled with gray (114).
//      (Not centered! Many YOLO variants center; YOLOX doesn't. Getting this
//      wrong shifts every box.)
//    - Keep BGR channel order and raw 0..255 float values — the official
//      YOLOX release models (0.1.1rc0+) do NOT use mean/std normalization.
//    - Reorder HWC (row, col, channel) -> CHW (channel, row, col).
//
// 2. Inference: one ONNX Runtime Session::Run. Input "images" [1,3,416,416],
//    output "output" [1,3549,85]. 3549 = 52*52 + 26*26 + 13*13 — one
//    prediction per grid cell across three strides (8, 16, 32). 85 = 4 box
//    values + 1 objectness + 80 class scores.
//
// 3. Postprocess (must match YOLOX's `demo_postprocess` + score/NMS logic):
//    - The raw box values are RELATIVE TO THEIR GRID CELL. Decode:
//        cx = (raw_x + grid_x) * stride
//        cy = (raw_y + grid_y) * stride
//        w  = exp(raw_w) * stride
//        h  = exp(raw_h) * stride
//    - score = objectness * class_score (both already sigmoid-ed in the model).
//    - Convert center/size to corners, divide by r to map back to the
//      original image's pixel coordinates.
//    - Each prediction takes its single best class (argmax), then
//      class-AGNOSTIC non-maximum suppression (NMS): among boxes overlapping
//      by more than nms_iou_threshold, keep only the highest-scoring one,
//      regardless of class. This matches YOLOX's default
//      multiclass_nms(..., class_agnostic=True).
// ---------------------------------------------------------------------------

struct OnnxDetectorConfig {
  std::string model_path;
  int input_size = 416;               // yolox_nano / yolox_tiny use 416; larger YOLOX models use 640.
  float score_threshold = 0.3f;       // Minimum objectness*class score to keep a box.
  float nms_iou_threshold = 0.45f;    // Overlapping boxes above this IoU are suppressed (class-agnostic).
  std::vector<int> target_class_ids;  // Detect() only reports these classes. Empty = any class.
  int intra_op_threads = 1;           // ONNX Runtime thread pool size. Edge devices have few cores; measure 1 vs N.
};

// One detection in ORIGINAL image pixel coordinates (not the 416x416 canvas).
struct Detection {
  int class_id = -1;
  float score = 0.0f;
  float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

class OnnxCpuDetector : public Detector {
 public:
  explicit OnnxCpuDetector(const OnnxDetectorConfig& config);

  // Detector interface: the single highest-scoring detection among
  // config.target_class_ids, with a box normalized to [0,1] for the proto.
  DetectionResult Detect(const Frame& frame) override;

  // Every detection that survives score threshold + NMS, any class. Used by
  // tests and tools; the pipeline only needs Detect().
  std::vector<Detection> DetectAll(const Frame& frame);

 private:
  // Writes the letterboxed CHW float tensor into input_tensor_ and returns r.
  float Preprocess(const Frame& frame);
  static Ort::SessionOptions MakeSessionOptions(const OnnxDetectorConfig& config);

  OnnxDetectorConfig config_;
  Ort::Env env_;
  Ort::SessionOptions session_options_;
  Ort::Session session_;
  Ort::MemoryInfo memory_info_;
  std::string input_name_;
  std::string output_name_;

  // Reused every frame: allocated once in the constructor, never on the hot
  // path — the same allocation discipline as the ring buffer.
  std::vector<float> input_tensor_;  // 3 * input_size * input_size, CHW
  std::vector<float> grid_x_, grid_y_, grid_stride_;  // one entry per prediction row (3549 for 416)
  // Bilinear resize lookup tables, one entry per destination column/row.
  std::vector<int> col_x0_, col_x1_, row_y0_, row_y1_;
  std::vector<float> col_wx_, row_wy_;
  std::vector<Detection> candidates_;  // Reused scratch space; cleared, not reallocated, each frame.
};

}  // namespace ridgeline
