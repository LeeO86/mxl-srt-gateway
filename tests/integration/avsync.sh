#!/usr/bin/env bash
# Lip sync after a source restart, measured on the ingest's MXL output (a white frame and a tone
# onset every 5 s, mxl-srt-avsync-check):
# - loop: an MXL writer -> egress (H.264 + AAC) -> SRT -> ingest; the writer restarts (its flows
#   are deleted and created again, its timestamps go on on TAI);
# - an ffmpeg sender -> SRT -> ingest; the sender restarts with a new PTS base.
# After each restart the AV offset must be back at its value before within 3 s: within 25 ms for
# the loop, 80 ms for the ffmpeg sender (its own offset differs by up to about 40 ms from one
# connection to the next, also in 1.4.4).
# Usage: tests/integration/avsync.sh [gateway] [flow-writer] [avsync-check]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
WRITER="${2:-$(dirname "$BIN")/mxl-srt-flow-writer}"
CHECK="${3:-$(dirname "$BIN")/mxl-srt-avsync-check}"
WORK="$(mktemp -d)"
API=http://127.0.0.1:18160/api/v1
SRC_ID=5a5a5a5a-0000-4000-8000-000000000002
VIDEO=5a5a5a5a-0000-4000-8000-0000000000cc
AUDIO=5a5a5a5a-0000-4000-8000-0000000000dd
WPID=""
GW=""
SRC=""
trap 'rc=$?; [ "$rc" = 0 ] || tail -n 40 "$WORK/gw.log" >&2 || true; kill $GW $WPID $SRC 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat > "$WORK/config.json" <<EOF
{
  "WEB_PORT": 18160,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19400-19499",
  "MXL_DOMAIN_SCAN_PATH": "$WORK/mxl",
  "MXL_OUTPUT_DOMAIN_DIR": "$WORK/mxl/out",
  "STATE_DIR": "$WORK/state",
  "DECODER": "cpu",
  "ENCODER": "cpu",
  "channels": [
    {
      "id": "tx1",
      "label": "TX1",
      "direction": "egress",
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19402, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
      "egress": {"codec": "h264", "bitrate": 4000000, "preset": "ultrafast", "tune": "zerolatency",
                 "audio_tracks": [{"codec": "aac", "layout": "stereo", "channels": [0, 1], "language": "eng"}]}
    },
    {
      "id": "rx-loop",
      "label": "RX loop",
      "direction": "ingest",
      "srt": {"mode": "caller", "remote_host": "127.0.0.1", "remote_port": 19402, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}
    },
    {
      "id": "rx-ffmpeg",
      "label": "RX ffmpeg",
      "direction": "ingest",
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19420, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}
    }
  ]
}
EOF

api() {
  python3 - "$@" <<'PY'
import json, sys, urllib.request
method, path = sys.argv[1], sys.argv[2]
body = sys.argv[3].encode() if len(sys.argv) > 3 else None
req = urllib.request.Request("http://127.0.0.1:18160/api/v1" + path, data=body, method=method, headers={"content-type": "application/json"})
print(urllib.request.urlopen(req, timeout=5).read().decode())
PY
}
flow() {
  api GET "/channels/$1/status" | python3 -c 'import json,sys; f=json.load(sys.stdin)["flows"]; print(f["video"] if sys.argv[1] == "video" else f["audio"][0])' "$2"
}
start_writer() {
  "$WRITER" "$WORK/mxl/src" "$SRC_ID" "$VIDEO" 300 "$AUDIO" marks &
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
  wait "$WPID" || true
  WPID=""
}
# An ffmpeg sender with the same marks: a white frame and a 100 ms 1 kHz tone every 5 s.
send() {
  "${FFMPEG:-ffmpeg}" -hide_banner -loglevel error -re \
    -f lavfi -i "color=c=black:s=1280x720:r=25,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='lt(mod(t\,5)\,0.04)'" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000,volume=0:enable='gte(mod(t\,5)\,0.1)'" \
    -t 120 -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 25 -c:a aac -ac 2 -muxdelay 0 \
    -output_ts_offset "$1" -f mpegts "srt://127.0.0.1:19420?mode=caller&latency=120" &
  SRC=$!
}
stop_source() {
  kill -KILL "$SRC" 2>/dev/null || true
  wait "$SRC" 2>/dev/null || true
  SRC=""
}
# check <channel> <what> <seconds> <restart> <tolerance ms>: measures the ingest's output for
# <seconds>; <restart> (a shell function) runs 17 s in. The marks 3 s or more after the restart
# must be within <tolerance ms> of the median before it.
check() {
  local channel="$1" what="$2" seconds="$3" restart="$4" tolerance="$5"
  "$CHECK" "$WORK/mxl/out" "$(flow "$channel" video)" "$(flow "$channel" audio)" "$seconds" > "$WORK/marks.jsonl" &
  local cpid=$!
  sleep 17
  local at
  at="$(date +%s.%N)"
  "$restart"
  wait "$cpid"
  python3 - "$WORK/marks.jsonl" "$at" "$what" "$tolerance" <<'PY'
import json, statistics, sys
marks = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
at, what, tolerance = float(sys.argv[2]), sys.argv[3], float(sys.argv[4])
before = [m["av_ms"] for m in marks if m["unix_s"] < at]
after = [(m["unix_s"] - at, m["av_ms"]) for m in marks if m["unix_s"] >= at]
print(what, "AV before (ms):", before, "after (s after the restart, ms):", [(round(t, 1), a) for t, a in after])
assert len(before) >= 2, "too few marks before the restart"
base = statistics.median(before)
settled = [a for t, a in after if t >= 3]
assert len(settled) >= 2, "too few marks after the restart"
bad = [a for a in settled if abs(a - base) > tolerance]
assert not bad, f"{what}: AV offset {bad} ms after the restart, {base} ms before"
PY
}
restart_writer() {
  stop_writer
  sleep 1.5
  start_writer
}
restart_sender() {
  stop_source
  sleep 1.5
  send 3000
}

mkdir -p "$WORK/mxl"
start_writer
SRTGW_CONFIG_FILE="$WORK/config.json" stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
GW=$!
for _ in $(seq 1 20); do
  api GET /channels >/dev/null 2>&1 && break
  sleep 0.5
done
api POST /channels/tx1/route "{\"video_active\":true,\"video_domain\":\"$SRC_ID\",\"video_flow\":\"$VIDEO\",\"audio_flow\":\"$AUDIO\"}" >/dev/null
sleep 2
send 0
sleep 4

check rx-loop "loop, MXL source restarted" 40 restart_writer 25
check rx-ffmpeg "ffmpeg sender restarted (new PTS base)" 40 restart_sender 80
grep '"event":"av_resync"' "$WORK/gw.log" || true

kill -TERM "$GW" || true
wait "$GW" || true
echo "avsync ok"
