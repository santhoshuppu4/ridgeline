#!/usr/bin/env bash
# Chaos test for Phase 1c durability: kill -9 the AGENT while it holds events
# that exist ONLY in its write-ahead log, restart it, and verify every event it
# ever durably committed reached the gateway.
#
# WHY THE ORACLE CHECKS IDENTITY, NOT JUST SEQUENCE GAPS: an earlier version of
# this test only checked the gateway's "lost=0" counter, and it still PASSED
# with WAL appends completely disabled. Without a WAL, a restarted agent just
# resumes numbering from its last persisted ack, reusing sequence numbers for
# brand-new events -- the gateway sees a contiguous sequence and cannot tell
# that the original events were lost. So this test now compares identities:
# the agent prints "[commit] seq=N capture_ns=T" after each fsync'd append,
# the gateway prints "[recv] ... seq=N capture_ns=T", and every commit must
# appear in the received set. A reused seq with a different capture time is
# reported as the loss it is.
#
# SCENARIO (deterministic, not timing-lucky):
#   1. gateway up, agent streaming
#   2. gateway stopped -> agent keeps committing events it CANNOT send
#   3. kill -9 the agent: those events now exist only in the WAL
#   4. new gateway, restarted agent with the same state dir
#   5. oracle: all committed events received, no seq reused for a different event
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50598}"
WORK="$(mktemp -d)"
STATE_DIR="$WORK/agent-state"
AGENT="$BUILD_DIR/agent/ridgeline_agent"
GATEWAY="$BUILD_DIR/gateway/ridgeline_gateway"
COMMON=(--gateway="127.0.0.1:$PORT" --device-id=cam-chaos --state-dir="$STATE_DIR" --rate-hz=30 --log-commits)

GW_PID=""; AGENT_PID=""
cleanup() { [[ -n "$GW_PID" ]] && kill "$GW_PID" 2>/dev/null || true; [[ -n "$AGENT_PID" ]] && kill -9 "$AGENT_PID" 2>/dev/null || true; }
trap cleanup EXIT

echo "work dir: $WORK"

"$GATEWAY" --listen="127.0.0.1:$PORT" --log-events >"$WORK/gateway1.log" 2>&1 & GW_PID=$!
sleep 0.5
"$AGENT" "${COMMON[@]}" >"$WORK/agent1.log" 2>&1 & AGENT_PID=$!
sleep 1.5

echo "step 2: stopping gateway; agent keeps committing events it cannot deliver"
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true; GW_PID=""
sleep 1.5

echo "step 3: kill -9 agent (undelivered events now exist only in the WAL)"
kill -9 "$AGENT_PID"; wait "$AGENT_PID" 2>/dev/null || true; AGENT_PID=""

echo "step 4: new gateway, restarted agent, same state dir"
"$GATEWAY" --listen="127.0.0.1:$PORT" --log-events >"$WORK/gateway2.log" 2>&1 & GW_PID=$!
sleep 0.5
AGENT2_STATUS=0
"$AGENT" "${COMMON[@]}" --duration-s=3 >"$WORK/agent2.log" 2>&1 || AGENT2_STATUS=$?
sleep 0.3
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true; GW_PID=""

grep -h "replayed" "$WORK/agent2.log" || true
[[ $AGENT2_STATUS -eq 0 ]] || { echo "FAIL: restarted agent exited $AGENT2_STATUS (unacked events remained)"; exit 1; }

echo "step 5: oracle"
python3 - "$WORK" <<'PY'
import re, sys, pathlib
w = pathlib.Path(sys.argv[1])
pat = re.compile(r"seq=(\d+) capture_ns=(-?\d+)")
def pairs(files, tag):
    out = []
    for f in files:
        for line in (w / f).read_text().splitlines():
            if line.startswith(tag):
                m = pat.search(line); out.append((int(m.group(1)), int(m.group(2))))
    return out

committed1 = pairs(["agent1.log"], "[commit]")
committed = committed1 + pairs(["agent2.log"], "[commit]")
received = pairs(["gateway1.log", "gateway2.log"], "[recv]")
received_set = set(received)
max_recv_by_gw1 = max((s for s, _ in pairs(["gateway1.log"], "[recv]")), default=0)

# Sanity: the scenario must actually have created WAL-only events, or this test proves nothing.
wal_only = [c for c in committed1 if c[0] > max_recv_by_gw1]
if not wal_only:
    print("FAIL: scenario did not produce any committed-but-undelivered events before the kill"); sys.exit(1)

missing = [c for c in committed if c not in received_set]
by_seq = {}
for s, t in received: by_seq.setdefault(s, set()).add(t)
reused = {s: ts for s, ts in by_seq.items() if len(ts) > 1}

print(f"  committed events:              {len(committed)} (agent run 1: {len(committed1)})")
print(f"  WAL-only at time of kill -9:   {len(wal_only)}")
print(f"  received (incl. redeliveries): {len(received)}")
if missing:
    print(f"FAIL: {len(missing)} committed event(s) never reached the gateway, e.g. {missing[:3]}"); sys.exit(1)
if reused:
    print(f"FAIL: {len(reused)} seq number(s) reused for different events, e.g. {list(reused.items())[:2]}"); sys.exit(1)
print("PASS: every committed event delivered after kill -9; no sequence numbers reused")
PY
