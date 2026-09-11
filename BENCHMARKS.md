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

See `context/adr/0003-ring-buffer-capacity-sizing.md` for why a slow-consumer
run (200fps-equivalent) is deliberately NOT reported as a latency number here.