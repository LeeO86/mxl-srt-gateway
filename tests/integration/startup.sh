#!/usr/bin/env bash
# Many ingest channels start at once on an output domain that still holds the stale flows of a
# killed run (same flow ids). Every start must create all its flow writers: the start-up garbage
# collection must not delete a stale flow while a writer is opening it.
# Usage: tests/integration/startup.sh [gateway] [runs]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
RUNS="${2:-20}"
WORK="$(mktemp -d)"
GW=""
trap 'kill -KILL $GW 2>/dev/null || true; rm -rf "$WORK"' EXIT

channels=""
for i in 1 2 3 4 5 6 7 8; do
  channels="$channels{\"id\":\"rx$i\",\"label\":\"RX$i\",\"direction\":\"ingest\",\"srt\":{\"mode\":\"listener\",\"local_address\":\"127.0.0.1\",\"local_port\":$((19300 + i)),\"latency_ms\":120},\"target\":{\"width\":1280,\"height\":720,\"scan\":\"progressive\",\"rate\":\"25\"}},"
done
for i in 1 2; do
  channels="$channels{\"id\":\"tx$i\",\"label\":\"TX$i\",\"direction\":\"egress\",\"srt\":{\"mode\":\"listener\",\"local_address\":\"127.0.0.1\",\"local_port\":$((19350 + i)),\"latency_ms\":120},\"target\":{\"width\":1280,\"height\":720,\"scan\":\"progressive\",\"rate\":\"25\"},\"egress\":{\"codec\":\"h264\",\"preset\":\"ultrafast\"}},"
done
cat > "$WORK/config.json" <<JSON
{
  "WEB_PORT": 18150,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19300-19399",
  "MXL_DOMAIN_SCAN_PATH": "$WORK/mxl",
  "MXL_OUTPUT_DOMAIN_DIR": "$WORK/mxl/out",
  "STATE_DIR": "$WORK/state",
  "DECODER": "cpu",
  "ENCODER": "cpu",
  "channels": [${channels%,}]
}
JSON

failed=0
retries=0
for run in $(seq 1 "$RUNS"); do
  SRTGW_CONFIG_FILE="$WORK/config.json" "$BIN" > "$WORK/gw.log" 2>&1 &
  GW=$!
  sleep 2.5
  if grep -q 'flow writer failed\|Failed to create flow\|Could not open shared memory' "$WORK/gw.log"; then
    failed=$((failed + 1))
    echo "run $run: $(grep -o '"error":"[^"]*flow writer failed[^"]*"\|Could not open shared memory[^"]*' "$WORK/gw.log" | head -2 | tr '\n' ' ')"
  fi
  retries=$((retries + $(grep -c '"event":"mxl_flow_writer_retry"' "$WORK/gw.log" || true)))
  # Killed: its flows stay in the domain, unlocked, for the next run to find.
  kill -KILL "$GW"
  wait "$GW" 2>/dev/null || true
  GW=""
done
echo "flows in the domain: $(ls -d "$WORK"/mxl/out/*.mxl-flow 2>/dev/null | wc -l); failed starts: $failed of $RUNS; writer retries: $retries"
# The garbage collection waits for writer creation: no retry should be needed either.
[ "$failed" = 0 ] && [ "$retries" = 0 ]
echo "startup ok"
