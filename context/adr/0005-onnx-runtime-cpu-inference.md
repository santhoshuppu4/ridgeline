# ADR-0005: Real inference with ONNX Runtime (CPU) and YOLOX-nano

- **Status:** Accepted
- **Date:** 2026-09-11

## Context
Phase 1b-i proved the capture -> ring buffer -> detector -> K-of-N wiring with
a FakeDetector. Phase 1b-ii replaces it with real video decoding and a real
neural network, on a CPU-only development machine.

## Decisions

**Runtime: ONNX Runtime 1.23.2 prebuilt CPU package.** Portable (same model
file runs on x86 today, ARM edge hardware later), no GPU required, and a
stable C++ API. Pinned version + SHA-256 in `scripts/fetch-phase1b-assets.sh`.

**Model: official `yolox_nano.onnx` (0.1.1rc0 release), Apache-2.0.** Small
(3.6MB, 416x416 input) and permissively licensed, unlike Ultralytics YOLO (AGPL).

**The model detects COCO classes, not smoke.** COCO has no smoke/fire class.
This phase measures real inference cost and proves the plumbing. Smoke
detection requires fine-tuning on a smoke dataset (e.g. HPWREN FIgLib) in a
later phase. Do not describe this build as smoke detection.

**Preprocessing and postprocessing are ported from YOLOX's own code and
verified against it.** The official Python pipeline (`preproc`,
`demo_postprocess`, class-agnostic NMS) was run on the same model and image.
C++ output matches: identical classes and scores, boxes within 3px. The
residual comes from resize rounding (our bilinear vs. OpenCV's fixed-point
INTER_LINEAR differ by at most 1 gray level on ~12% of pixels). Tests use
10px / 0.05 score tolerances, and mutation testing confirmed they fail on an
RGB/BGR swap and on a missing grid offset.

**Frame switched from NV12 to BGR24.** OpenCV decodes to BGR and YOLOX expects
BGR, so storing BGR avoids a per-frame color conversion. Frame grew from
~1.3MB to ~2.6MB (1280x720x3). Consequence: frame rings must be small
(ridgeline_edge uses 4 slots, 3 usable), consistent with ADR-0003.

**Zero-copy ring buffer API (TryPushWith / TryPopWith).** TryPush(T) takes its
argument by value; for a 2.6MB Frame that meant two full copies and 2.6MB of
stack per frame. The new API fills and consumes slots in place. Same ownership
argument as TryPush/TryPop; TSan-verified, and a mutation that publishes the
slot before writing it is caught as a data race.

**ONNX Runtime's shipped CMake config is broken for the tarball layout.** It
references `lib64/` and `include/onnxruntime/`, which don't exist in the
archive. `cmake/Dependencies.cmake` defines the imported target manually.

**ONNX builds are excluded from TSan.** Prebuilt ONNX Runtime is not
TSan-instrumented and its thread pool produces false positives. The ring
buffer and pipeline logic remain TSan-covered via the core tests. ONNX tests
do run under ASan+UBSan.

## Consequences
- Detector accuracy for the actual product (smoke) is still unmeasured.
- Inference latency on the developer's CPU is now measurable:
  `ridgeline_infer_bench` (inference alone) and `ridgeline_edge` (full
  pipeline, including queueing and drops). Ring capacity should be revisited
  with those numbers per ADR-0003.
