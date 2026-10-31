#!/usr/bin/env bash
# SIGTERM must stop a gateway with several ingest and egress channels within 5 s (Kubernetes
# gives 30 s, deploy/*.yaml), exit 143, and not spin on SRT's released epolls. Channels: an
# egress listener with its ingest caller connected and routed to a writer, an egress listener
# nobody calls, an egress caller and an ingest caller whose far end is not there (backing off),
# an ingest listener nobody calls. 1.4.5 hung for good with two egress channels (the second one
# blocked in route() while the channels were stopped under the lock route() takes).
# Usage: tests/integration/stop.sh [gateway] [flow-writer]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
WRITER="${2:-$(dirname "$BIN")/mxl-srt-flow-writer}"
WORK="$(mktemp -d)"
SRC_ID=5a5a5a5a-0000-4000-8000-000000000004
VIDEO=5a5a5a5a-0000-4000-8000-0000000000f1
AUDIO=5a5a5a5a-0000-4000-8000-0000000000f2
WPID=""
GW=""
trap 'rc=$?; [ "$rc" = 0 ] || tail -n 40 "$WORK/gw.log" >&2 || true; kill -KILL $GW 2>/dev/null || true; kill $WPID 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat > "$WORK/config.json" <<EOF
{
  "WEB_PORT": 18180,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19600-19699",
  "MXL_DOMAIN_SCAN_PATH": "$WORK/mxl",
  "MXL_OUTPUT_DOMAIN_DIR": "$WORK/mxl/out",
  "STATE_DIR": "$WORK/state",
  "DECODER": "cpu",
  "ENCODER": "cpu",
  "channels": [
    {"id": "tx-conn", "label": "TX connected", "direction": "egress",
     "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19602, "latency_ms": 120},
     "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
     "egress": {"codec": "h264", "bitrate": 2000000, "preset": "ultrafast", "tune": "zerolatency"}},
    {"id": "tx-idle", "label": "TX idle", "direction": "egress",
     "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19604, "latency_ms": 120},
     "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
     "egress": {"codec": "h264", "bitrate": 2000000, "preset": "ultrafast", "tune": "zerolatency"}},
    {"id": "tx-caller", "label": "TX caller", "direction": "egress",
     "srt": {"mode": "caller", "remote_host": "127.0.0.1", "remote_port": 19698, "latency_ms": 120},
     "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
     "egress": {"codec": "h264", "bitrate": 2000000, "preset": "ultrafast", "tune": "zerolatency"}},
    {"id": "rx-conn", "label": "RX connected", "direction": "ingest",
     "srt": {"mode": "caller", "remote_host": "127.0.0.1", "remote_port": 19602, "latency_ms": 120},
     "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}},
    {"id": "rx-listen", "label": "RX listener", "direction": "ingest",
     "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19620, "latency_ms": 120},
     "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}},
    {"id": "rx-backoff", "label": "RX caller", "direction": "ingest",
     "srt": {"mode": "caller", "remote_host": "127.0.0.1", "remote_port": 19699, "latency_ms": 120},
     "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}}
  ]
}
EOF

api() {
  python3 - "$@" <<'PY'
import sys, urllib.request
method, path = sys.argv[1], sys.argv[2]
body = sys.argv[3].encode() if len(sys.argv) > 3 else None
req = urllib.request.Request("http://127.0.0.1:18180/api/v1" + path, data=body, method=method, headers={"content-type": "application/json"})
print(urllib.request.urlopen(req, timeout=15).read().decode())
PY
}

mkdir -p "$WORK/mxl"
"$WRITER" "$WORK/mxl/src" "$SRC_ID" "$VIDEO" 300 "$AUDIO" &
WPID=$!
# Each round runs that long before the SIGTERM: soon after the start, and with the backoffs at
# 4-8 s and at their 10 s ceiling.
for run in 3 9 16; do
  SRTGW_CONFIG_FILE="$WORK/config.json" stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
  GW=$!
  for _ in $(seq 1 30); do
    api GET /channels >/dev/null 2>&1 && break
    sleep 0.5
  done
  api POST /channels/tx-conn/route "{\"video_active\":true,\"video_domain\":\"$SRC_ID\",\"video_flow\":\"$VIDEO\",\"audio_flow\":\"$AUDIO\"}" >/dev/null
  sleep "$run"
  api GET /channels/rx-conn/status | python3 -c 'import json,sys; s=json.load(sys.stdin); assert s["srt"]["connected"], s["state"]'
  t0=$(date +%s%N)
  kill -TERM "$GW"
  code=0
  for _ in $(seq 1 100); do
    kill -0 "$GW" 2>/dev/null || break
    sleep 0.05
  done
  if kill -0 "$GW" 2>/dev/null; then
    echo "after ${run} s: still running 5 s after SIGTERM" >&2
    exit 1
  fi
  wait "$GW" || code=$?
  GW=""
  ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  spins=$(grep -c 'INVALID' "$WORK/gw.log" || true)
  echo "after ${run} s: exit $code in ${ms} ms, ${spins} EID INVALID lines"
  [ "$code" = 143 ] || { echo "expected exit 143" >&2; exit 1; }
  # At most one per listener (a wait that was in flight when its epoll was released).
  [ "$spins" -le 3 ] || { echo "spun on released epolls" >&2; exit 1; }
done
echo "stop ok"
