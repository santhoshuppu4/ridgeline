#pragma once

#include <memory>
#include <string>

#include "ridgeline/frame.h"

namespace ridgeline {

// Reads frames from a video file (or an image sequence pattern OpenCV
// understands) and writes them as BGR24 into a Frame — designed to be called
// from inside SpscRingBuffer::TryPushWith, the same way SyntheticFrameSource is.
//
// STUDY NOTE: why OpenCV is hidden behind a pImpl (the `Impl` struct in the
// .cc file) instead of including <opencv2/...> here. Anything that includes
// this header — the edge tool, tests — would otherwise also need OpenCV's
// include paths and would recompile whenever OpenCV headers change. Keeping
// cv::VideoCapture inside the .cc file confines the dependency to exactly one
// translation unit. Same "depend on an interface" idea as detector.h, applied
// to a build dependency instead of a runtime one.
//
// Frames larger than Frame::kMaxWidth x kMaxHeight are downscaled (keeping
// aspect ratio) at this boundary, since Frame's storage is fixed-size.
class VideoFileFrameSource {
 public:
  // Throws std::runtime_error if the file can't be opened.
  explicit VideoFileFrameSource(const std::string& path, bool loop = false);
  ~VideoFileFrameSource();

  VideoFileFrameSource(const VideoFileFrameSource&) = delete;
  VideoFileFrameSource& operator=(const VideoFileFrameSource&) = delete;

  // Decodes the next frame into `out`. Returns false at end of file (unless
  // looping) or on a decode error.
  bool Next(Frame& out);

  // Frame rate reported by the container, or 0 if unknown. Used to pace
  // playback like a live camera.
  double Fps() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ridgeline
