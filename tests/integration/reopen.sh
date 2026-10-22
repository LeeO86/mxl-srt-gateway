#!/usr/bin/env bash
# An egress reads an MXL flow whose writer restarts: the writer deletes the flow on exit and
# creates it again under the same id (new files). The egress must open the new flow within ~1 s
# (video frames move again, audio is back) and keep no mapping of the deleted files. A new
# activation of the same flow and a channel restart open the flow fresh.
# Usage: tests/integration/reopen.sh [gateway] [flow-writer]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
WRITER="${2:-$(dirname "$BIN")/mxl-srt-flow-writer}"
WORK="$(mktemp -d)"
API=http://127.0.0.1:18130/api/v1
SRC_ID=5a5a5a5a-0000-4000-8000-000000000001
VIDEO=5a5a5a5a-0000-4000-8000-0000000000aa
AUDIO=5a5a5a5a-0000-4000-8000-0000000000bb
WPID=""
GW=""
trap 'rc=$?; [ "$rc" = 0 ] || tail -n 40 "$WORK/gw.log" >&2 || true; kill $GW $WPID 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat > "$WORK/config.json" <<EOF
{
  "WEB_PORT": 18130,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19100-19199",
  "MXL_DOMAIN_SCAN_PATH": "$WORK/mxl",
  "MXL_OUTPUT_DOMAIN_DIR": "$WORK/mxl/out",
  "STATE_DIR": "$WORK/state",
  "ENCODER": "cpu",
  "channels": [
    {
      "id": "tx1",
      "label": "TX1",
      "direction": "egress",
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19102, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
      "egress": {"codec": "h264", "bitrate": 2000000, "preset": "ultrafast", "tune": "zerolatency",
                 "audio_tracks": [{"codec": "s302m", "layout": "stereo", "channels": [0, 1], "language": "eng"}]}
    }
  ]
}
EOF

# A writer: video and audio (DC 0.1 on channel 1) until it is stopped. It deletes its flows when
# it exits.
start_writer() {
  "$WRITER" "$WORK/mxl/src" "$SRC_ID" "$VIDEO" 120 "$AUDIO" &
  WPID=$!
  for _ in $(seq 1 40); do
    [ -e "$WORK/mxl/src/$VIDEO.mxl-flow/data" ] && [ -e "$WORK/mxl/src/$AUDIO.mxl-flow/data" ] && return 0
    sleep 0.05
  done
  echo "writer did not create the flows" >&2
  exit 1
}
stop_writer() {
  kill -TERM "$WPID"
  wait "$WPID"
  WPID=""
  for _ in $(seq 1 40); do
    [ ! -e "$WORK/mxl/src/$VIDEO.mxl-flow" ] && [ ! -e "$WORK/mxl/src/$AUDIO.mxl-flow" ] && return 0
    sleep 0.05
  done
  echo "the flow was not deleted with its writer" >&2
  exit 1
}
# Activates (or with "false" deactivates) the route, as an IS-05 activation does.
route() {
  curl -fsS -X POST "$API/channels/tx1/route" -H 'content-type: application/json' \
    -d "{\"video_active\":${1:-true},\"video_domain\":\"$SRC_ID\",\"video_flow\":\"$VIDEO\",\"audio_flow\":\"$AUDIO\"}" >/dev/null
}
# The readers are released: no file of the flows stays mapped in the gateway (up to 10 s).
wait_released() {
  for i in $(seq 1 40); do
    grep -qE "($VIDEO|$AUDIO)\.mxl-flow/" "/proc/$GW/maps" || return 0
    sleep 0.25
  done
  echo "$1: the gateway still maps the flow" >&2
  exit 1
}
# Over 2 s the egress must pick a new source frame for nearly every output frame (a reader on
# the deleted files repeats its last frame: every output frame is a repeat).
check_moving() {
  python3 - "$1" <<'PY'
import json, sys, time, urllib.request
url = "http://127.0.0.1:18130/api/v1/channels/tx1/status"
a = json.load(urllib.request.urlopen(url))
time.sleep(2)
b = json.load(urllib.request.urlopen(url))
frames, repeats = b["frames"] - a["frames"], b["repeats"] - a["repeats"]
print(sys.argv[1], "state", b["state"], "frames", frames, "repeats", repeats)
assert b["state"] == "running", b
assert frames >= 40 and repeats <= frames // 10, (frames, repeats)
PY
}
# No mapping of a deleted flow file is left in the gateway.
check_no_deleted() {
  if grep -E "($VIDEO|$AUDIO)\.mxl-flow/.*\(deleted\)" "/proc/$GW/maps"; then
    echo "$1: the gateway still maps the deleted flow" >&2
    exit 1
  fi
}
# The egress audio, decoded: channel 1 carries the writer's DC 0.1 (-20 dBFS).
check_audio() {
  ffmpeg -hide_banner -nostats -i "srt://127.0.0.1:19102?mode=caller&transtype=live&latency=200" -t 1.5 -map 0:a:0 -af volumedetect -f null - \
    > "$WORK/audio.log" 2>&1 || true
  local peak
  peak="$(sed -n 's/.*max_volume: \(-\?[0-9.]*\) dB.*/\1/p' "$WORK/audio.log" | tail -1)"
  echo "$1 audio peak ${peak:-none} dB"
  python3 -c "import sys; sys.exit(0 if '$peak' and float('$peak') > -30 else 1)" || { tail -20 "$WORK/audio.log" >&2; exit 1; }
}
count() {
  grep -c "$1" "$WORK/gw.log" || true
}

mkdir -p "$WORK/mxl"
start_writer
SRTGW_CONFIG_FILE="$WORK/config.json" stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
GW=$!
for _ in $(seq 1 20); do
  curl -fsS http://127.0.0.1:18130/livez 2>/dev/null | grep -q ok && break
  sleep 0.5
done
route
sleep 2
check_moving "first flow"
check_audio "first flow"

# The writer restarts: same flow ids, new files.
stop_writer
start_writer
sleep 1.5
grep -q '"event":"mxl_flow_reopen","channel":"tx1","reason":"recreated"' "$WORK/gw.log"
check_moving "re-created flow"
check_no_deleted "re-created flow"
check_audio "re-created flow"
curl -fsS http://127.0.0.1:18130/metrics | grep -E '^mxl_srt_gateway_mxl_flow_reopens_total\{channel="tx1"\} [1-9]'

# A new activation of the same flow opens it again.
before="$(count '"event":"mxl_flow_open","channel":"tx1","reason":"activation"')"
route
sleep 1
after="$(count '"event":"mxl_flow_open","channel":"tx1","reason":"activation"')"
[ "$after" -gt "$before" ] || { echo "re-activation did not open the flow again" >&2; exit 1; }
check_moving "re-activated"

# IS-05 off, the writer restarts meanwhile, IS-05 on: the deactivation released the readers and
# the activation opens the new flow.
route false
wait_released "deactivated route"
stop_writer
start_writer
route
sleep 1.5
check_moving "deactivated and activated again"
check_no_deleted "deactivated and activated again"
check_audio "deactivated and activated again"

# The channel stops, the writer restarts meanwhile, the channel starts again: the restarted
# channel reads the new flow, not the reader the stopped one had open.
curl -fsS -X PUT "$API/channels/tx1" -H 'content-type: application/json' -d '{"enabled": false}' >/dev/null
wait_released "stopped channel"
stop_writer
start_writer
check_no_deleted "stopped channel"
before="$(count '"event":"mxl_flow_open","channel":"tx1","reason":"activation"')"
curl -fsS -X PUT "$API/channels/tx1" -H 'content-type: application/json' -d '{"enabled": true}' >/dev/null
sleep 2
after="$(count '"event":"mxl_flow_open","channel":"tx1","reason":"activation"')"
[ "$after" -gt "$before" ] || { echo "the restarted channel did not open the flow" >&2; exit 1; }
check_moving "restarted channel"
check_no_deleted "restarted channel"
check_audio "restarted channel"

kill -TERM "$GW" || true
wait "$GW" || true
echo "reopen ok"
