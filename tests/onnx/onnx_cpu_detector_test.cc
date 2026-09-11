// Tests for OnnxCpuDetector against a reference image with known objects.
//
// EXPECTED VALUES ARE NOT GUESSES. They come from running YOLOX's official
// Python preprocessing/postprocessing (yolox/data/data_augment.py preproc +
// yolox/utils/demo_utils.py demo_postprocess + class-agnostic NMS) with
// onnxruntime on the same model and image:
//
//   dog      score=0.86 box=(133,207)-(326,543)
//   bicycle  score=0.80 box=( 52,137)-(572,424)
//   car      score=0.80 box=(466, 78)-(692,171)
//
// Our C++ port matches to within a few pixels. The residual difference is
// resize rounding: this implementation's bilinear resize differs from
// cv2.resize(INTER_LINEAR) by at most 1 gray level (OpenCV uses fixed-point
// arithmetic internally). Hence the pixel/score tolerances below — tight
// enough to catch a real preprocessing bug (e.g. centering instead of
// top-left padding shifts boxes by ~50px; RGB instead of BGR changes scores),
// loose enough not to fail on 1-gray-level rounding.

#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <memory>
#include <stdexcept>

#include "ridgeline/coco_classes.h"
#include "ridgeline/onnx_cpu_detector.h"

namespace {

constexpr float kBoxTolerancePx = 10.0f;
constexpr float kScoreTolerance = 0.05f;

std::unique_ptr<ridgeline::Frame> LoadFrame(const char* path) {
  const cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
  if (img.empty()) return nullptr;
  auto frame = std::make_unique<ridgeline::Frame>();
  if (!frame->CopyFrom(reinterpret_cast<const std::byte*>(img.data), img.total() * img.elemSize(),
                       static_cast<std::uint32_t>(img.cols), static_cast<std::uint32_t>(img.rows), 0, 0)) {
    return nullptr;
  }
  return frame;
}

ridgeline::OnnxDetectorConfig DefaultConfig() {
  ridgeline::OnnxDetectorConfig c;
  c.model_path = RIDGELINE_MODEL_PATH;
  c.intra_op_threads = 1;
  return c;
}

const ridgeline::Detection* FindClass(const std::vector<ridgeline::Detection>& dets, int class_id) {
  for (const auto& d : dets) {
    if (d.class_id == class_id) return &d;
  }
  return nullptr;
}

void ExpectBoxNear(const ridgeline::Detection& d, float x1, float y1, float x2, float y2, float score) {
  EXPECT_NEAR(d.x1, x1, kBoxTolerancePx);
  EXPECT_NEAR(d.y1, y1, kBoxTolerancePx);
  EXPECT_NEAR(d.x2, x2, kBoxTolerancePx);
  EXPECT_NEAR(d.y2, y2, kBoxTolerancePx);
  EXPECT_NEAR(d.score, score, kScoreTolerance);
}

class OnnxCpuDetectorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    frame_ = LoadFrame(RIDGELINE_TEST_IMAGE);
    ASSERT_NE(frame_, nullptr) << "could not load " << RIDGELINE_TEST_IMAGE
                               << " -- run ./scripts/fetch-phase1b-assets.sh";
  }
  std::unique_ptr<ridgeline::Frame> frame_;
};

}  // namespace

TEST_F(OnnxCpuDetectorTest, MatchesOfficialYoloxReferenceOnDogImage) {
  ridgeline::OnnxCpuDetector detector(DefaultConfig());
  const auto dets = detector.DetectAll(*frame_);

  ASSERT_EQ(dets.size(), 3u) << "reference pipeline finds exactly dog, bicycle, car at score>0.3";

  const auto* dog = FindClass(dets, ridgeline::kCocoDog);
  const auto* bicycle = FindClass(dets, ridgeline::kCocoBicycle);
  const auto* car = FindClass(dets, ridgeline::kCocoCar);
  ASSERT_NE(dog, nullptr);
  ASSERT_NE(bicycle, nullptr);
  ASSERT_NE(car, nullptr);

  ExpectBoxNear(*dog, 133, 207, 326, 543, 0.86f);
  ExpectBoxNear(*bicycle, 52, 137, 572, 424, 0.80f);
  ExpectBoxNear(*car, 466, 78, 692, 171, 0.80f);
}

TEST_F(OnnxCpuDetectorTest, DetectReportsBestTargetClassWithNormalizedBox) {
  auto config = DefaultConfig();
  config.target_class_ids = {ridgeline::kCocoDog};
  ridgeline::OnnxCpuDetector detector(config);

  const ridgeline::DetectionResult r = detector.Detect(*frame_);
  ASSERT_TRUE(r.positive);
  EXPECT_NEAR(r.confidence, 0.86f, kScoreTolerance);
  // Normalized by the 768x576 image size.
  EXPECT_NEAR(r.x_min, 133.0f / 768.0f, kBoxTolerancePx / 768.0f);
  EXPECT_NEAR(r.y_max, 543.0f / 576.0f, kBoxTolerancePx / 576.0f);
  EXPECT_LT(r.x_min, r.x_max);
  EXPECT_LT(r.y_min, r.y_max);
}

TEST_F(OnnxCpuDetectorTest, TargetClassFilterExcludesOtherClasses) {
  auto config = DefaultConfig();
  config.target_class_ids = {ridgeline::kCocoPerson};  // No person in this image.
  ridgeline::OnnxCpuDetector detector(config);
  EXPECT_FALSE(detector.Detect(*frame_).positive);
}

TEST(OnnxCpuDetector, UniformGrayFrameHasNoDetections) {
  auto frame = std::make_unique<ridgeline::Frame>();
  frame->width = 640;
  frame->height = 480;
  frame->format = ridgeline::PixelFormat::kBgr24;
  frame->used_bytes = 640u * 480u * 3u;
  std::fill_n(frame->MutablePixels(), frame->used_bytes, std::uint8_t{128});

  ridgeline::OnnxCpuDetector detector(DefaultConfig());
  EXPECT_TRUE(detector.DetectAll(*frame).empty());
}

TEST(OnnxCpuDetector, RejectsFramesThatAreNotBgr) {
  auto frame = std::make_unique<ridgeline::Frame>();
  frame->width = 64;
  frame->height = 64;
  frame->format = ridgeline::PixelFormat::kUnknown;
  ridgeline::OnnxCpuDetector detector(DefaultConfig());
  EXPECT_THROW(detector.DetectAll(*frame), std::invalid_argument);
}

TEST(OnnxCpuDetector, RejectsMismatchedInputSizeAtConstruction) {
  auto config = DefaultConfig();
  config.input_size = 640;  // yolox_nano expects 416.
  EXPECT_THROW(ridgeline::OnnxCpuDetector{config}, std::runtime_error);
}
