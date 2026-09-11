#include "ridgeline/video_file_frame_source.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "ridgeline/time.h"

namespace ridgeline {

struct VideoFileFrameSource::Impl {
  cv::VideoCapture capture;
  cv::Mat decoded;
  cv::Mat resized;
  bool loop = false;
  std::uint64_t next_index = 0;
};

VideoFileFrameSource::VideoFileFrameSource(const std::string& path, bool loop) : impl_(std::make_unique<Impl>()) {
  impl_->loop = loop;
  if (!impl_->capture.open(path)) {
    throw std::runtime_error("could not open video: " + path);
  }
}

VideoFileFrameSource::~VideoFileFrameSource() = default;

double VideoFileFrameSource::Fps() const { return impl_->capture.get(cv::CAP_PROP_FPS); }

bool VideoFileFrameSource::Next(Frame& out) {
  if (!impl_->capture.read(impl_->decoded) || impl_->decoded.empty()) {
    if (!impl_->loop) return false;
    impl_->capture.set(cv::CAP_PROP_POS_FRAMES, 0);
    if (!impl_->capture.read(impl_->decoded) || impl_->decoded.empty()) return false;
  }
  // Timestamp as close to "frame became available" as we can get.
  const std::int64_t captured_ns = NowUnixNs();

  cv::Mat* bgr = &impl_->decoded;
  if (bgr->type() != CV_8UC3) {
    return false;  // Grayscale/alpha sources aren't supported yet; fail rather than mislabel the format.
  }
  if (static_cast<std::size_t>(bgr->cols) > Frame::kMaxWidth || static_cast<std::size_t>(bgr->rows) > Frame::kMaxHeight) {
    const double scale = std::min(static_cast<double>(Frame::kMaxWidth) / bgr->cols,
                                  static_cast<double>(Frame::kMaxHeight) / bgr->rows);
    cv::resize(*bgr, impl_->resized, cv::Size(), scale, scale, cv::INTER_AREA);
    bgr = &impl_->resized;
  }

  const auto width = static_cast<std::size_t>(bgr->cols);
  const auto height = static_cast<std::size_t>(bgr->rows);
  const std::size_t row_bytes = width * Frame::kBytesPerPixel;
  // Copy row by row: an OpenCV Mat isn't guaranteed to be one contiguous block.
  for (std::size_t y = 0; y < height; ++y) {
    std::memcpy(out.MutablePixels() + y * row_bytes, bgr->ptr<std::uint8_t>(static_cast<int>(y)), row_bytes);
  }
  out.width = static_cast<std::uint32_t>(width);
  out.height = static_cast<std::uint32_t>(height);
  out.used_bytes = row_bytes * height;
  out.format = PixelFormat::kBgr24;
  out.capture_time_unix_ns = captured_ns;
  out.frame_index = impl_->next_index++;
  return true;
}

}  // namespace ridgeline
