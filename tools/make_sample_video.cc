// ridgeline_make_sample_video: writes a deterministic test video from the
// reference image — alternating runs of "dog visible" and plain gray frames.
// Because you know exactly which frames contain the target, you know exactly
// how many confirmed episodes ridgeline_edge should report. Use it to sanity
// check the pipeline before pointing it at real footage.
//
// Usage:
//   ./build/tools/ridgeline_make_sample_video --out=third_party/testdata/sample.avi --fps=15 --segments=4 --frames-per-segment=30

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  std::string image = RIDGELINE_DEFAULT_IMAGE;
  std::string out = "third_party/testdata/sample.avi";
  double fps = 15.0;
  int segments = 4;
  int per_segment = 30;
  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    auto val = [&](std::string_view key) { return a.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr; };
    if (auto v = val("--image=")) image = v;
    else if (auto v2 = val("--out=")) out = v2;
    else if (auto v3 = val("--fps=")) fps = std::atof(v3);
    else if (auto v4 = val("--segments=")) segments = std::atoi(v4);
    else if (auto v5 = val("--frames-per-segment=")) per_segment = std::atoi(v5);
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
  }
  const cv::Mat target = cv::imread(image, cv::IMREAD_COLOR);
  if (target.empty()) { std::fprintf(stderr, "could not read %s\n", image.c_str()); return 1; }
  const cv::Mat gray(target.size(), CV_8UC3, cv::Scalar(128, 128, 128));

  cv::VideoWriter writer(out, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), fps, target.size());
  if (!writer.isOpened()) { std::fprintf(stderr, "could not open %s for writing\n", out.c_str()); return 1; }
  for (int s = 0; s < segments; ++s) {
    for (int i = 0; i < per_segment; ++i) writer.write(target);
    for (int i = 0; i < per_segment; ++i) writer.write(gray);
  }
  writer.release();
  std::printf("wrote %s: %d frames @ %.1f fps, %d target segments of %d frames each\n", out.c_str(),
              segments * 2 * per_segment, fps, segments, per_segment);
  std::printf("expected confirmed episodes with --classes=16 (dog): %d\n", segments);
  return 0;
}
