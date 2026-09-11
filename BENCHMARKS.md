# Benchmarks
Every number on the resume or README must trace to a row here.

## Hardware
| Field | Value |
|---|---|
| CPU | <paste from lscpu> |
| RAM | <paste from free -h> |
| OS | Windows 11 + WSL2 Ubuntu 24.04 |
| Compiler | g++ 13.3.0 |
| Cores | 14 |

## Results
| Date | Commit | Component | Metric | p50 | p95 | p99 | Conditions | Command |
|---|---|---|---|---|---|---|---|---|
| 2026-09-11 | <hash> | ring_buffer | throughput | — | — | 5,608,636 items/sec | idle consumer, capacity=4096 | `./ridgeline_bench --duration-s=5 --consumer-delay-us=0` |
| 2026-09-11 | <hash> | ring_buffer | enqueue-to-dequeue latency | 0.22 us | 235.14 us | 503.74 us | idle consumer, capacity=4096 | `./ridgeline_bench --duration-s=5 --consumer-delay-us=0` |
| 2026-09-11 | 7c20ffe | onnx_inference | preprocess+infer+postprocess (threads=1) | 23.52 ms | 32.51 ms | 39.05 ms | yolox_nano, 416x416, 768x576 input image, intra_op_threads=1 | `./ridgeline_infer_bench --threads=1 --iterations=200` |
| 2026-09-11 | 7c20ffe | onnx_inference | preprocess+infer+postprocess (threads=4) | 17.00 ms | 24.13 ms | 32.52 ms | yolox_nano, 416x416, 768x576 input image, intra_op_threads=4 | `./ridgeline_infer_bench --threads=4 --iterations=200` |
| 2026-09-11 | 7c20ffe | ridgeline_edge | capture-to-decision, full pipeline (realtime, 15fps source) | 33.70 ms | 47.89 ms | 52.09 ms | 4-slot Frame ring, K=3/N=5, single COCO class filter, 14-core CPU | `./ridgeline_edge --video=third_party/testdata/sample.avi --classes=16` |
See `context/adr/0003-ring-buffer-capacity-sizing.md` for why a slow-consumer
run (200fps-equivalent) is deliberately NOT reported as a latency number here.