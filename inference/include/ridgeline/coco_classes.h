#pragma once

#include <array>
#include <string_view>

namespace ridgeline {

// The 80 COCO class names, in the index order YOLOX's COCO-trained models
// emit. Used for logging and tests only. Reminder: none of these is "smoke".
inline constexpr std::array<std::string_view, 80> kCocoClasses = {
    "person",        "bicycle",      "car",           "motorcycle",    "airplane",     "bus",
    "train",         "truck",        "boat",          "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench",        "bird",          "cat",           "dog",          "horse",
    "sheep",         "cow",          "elephant",      "bear",          "zebra",        "giraffe",
    "backpack",      "umbrella",     "handbag",       "tie",           "suitcase",     "frisbee",
    "skis",          "snowboard",    "sports ball",   "kite",          "baseball bat", "baseball glove",
    "skateboard",    "surfboard",    "tennis racket", "bottle",        "wine glass",   "cup",
    "fork",          "knife",        "spoon",         "bowl",          "banana",       "apple",
    "sandwich",      "orange",       "broccoli",      "carrot",        "hot dog",      "pizza",
    "donut",         "cake",         "chair",         "couch",         "potted plant", "bed",
    "dining table",  "toilet",       "tv",            "laptop",        "mouse",        "remote",
    "keyboard",      "cell phone",   "microwave",     "oven",          "toaster",      "sink",
    "refrigerator",  "book",         "clock",         "vase",          "scissors",     "teddy bear",
    "hair drier",    "toothbrush",
};

inline constexpr int kCocoPerson = 0;
inline constexpr int kCocoBicycle = 1;
inline constexpr int kCocoCar = 2;
inline constexpr int kCocoTruck = 7;
inline constexpr int kCocoDog = 16;

inline std::string_view CocoClassName(int id) {
  return (id >= 0 && id < static_cast<int>(kCocoClasses.size())) ? kCocoClasses[static_cast<std::size_t>(id)]
                                                                 : std::string_view{"unknown"};
}

}  // namespace ridgeline
