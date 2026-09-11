# ADR-0001: Agent-to-gateway transport is a gRPC bidirectional stream
- **Status:** Accepted
- **Date:** 2026-09-11

## Context
Cameras sit on unreliable, low-bandwidth links. The agent sends detections and
heartbeats upstream; the gateway sends acks and config changes downstream.

## Options considered
1. Unary gRPC per event — simple, no server-push, per-event handshake overhead.
2. MQTT — built for IoT, but a second protocol/broker alongside Kafka.
3. gRPC bidirectional stream — one long-lived HTTP/2 connection, shared typed contracts, server push.

## Decision
One long-lived `IngestService.Connect` stream per device. First message must be
`Hello`. Detections carry a monotonic `seq`; the gateway replies with cumulative `Ack{up_to_seq}`.

## Consequences
- Cumulative acks let the agent truncate its WAL (Phase 1) with one number.
- The gateway detects gaps and duplicate replays per device.
- **Lesson found by the smoke test:** gRPC channels run their own reconnect
  backoff beneath the app's. Opening a stream on a non-READY channel fails
  fast and cancels the in-progress handshake. Fix: `Channel::WaitForConnected`
  before opening the stream, with the channel's backoff args aligned to the app's policy.
- Phase 0 uses the synchronous gRPC server (one thread per stream) — revisit
  before the Phase-5 fleet simulator; measure thread count and p99 under load.
