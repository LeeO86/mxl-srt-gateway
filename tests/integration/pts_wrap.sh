#!/usr/bin/env bash
# An ingest whose source MPEG-TS PTS wraps (33 bits at 90 kHz, after 26.5 h) must go on writing
# grains with no resync and no memory growth. 1.4.6 stepped the video mapping by 26.5 h and put
# that much silence before the audio (18 GB per channel): out of memory within a minute.
# 1. A source that starts 4 s before the wrap (FFmpeg's demuxer undoes this wrap itself).
# 2. A source that starts at 0 (FFmpeg then undoes no wrap, as on the platform) and jumps to 4 s
#    before the wrap in the same connection: the 26.5 h step that 1.4.6 ran out of memory on.
# Usage: tests/integration/pts_wrap.sh [gateway] [flow-verify]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
VERIFY="${2:-$(dirname "$BIN")/mxl-srt-flow-verify}"
WORK="$(mktemp -d)"
GW=""
SRC=""
trap 'rc=$?; [ "$rc" = 0 ] || tail -n 40 "$WORK/gw.log" >&2 || true; kill $GW $SRC 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat > "$WORK/config.json" <<EOF
{
  "WEB_PORT": 18150,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19300-19399",
  "MXL_DOMAIN_SCAN_PATH": "$WORK/mxl",
  "MXL_OUTPUT_DOMAIN_DIR": "$WORK/mxl/out",
  "STATE_DIR": "$WORK/state",
  "DECODER": "cpu",
  "ENCODER": "cpu",
  "channels": [
    {
      "id": "rx-wrap",
      "label": "RX wrap",
      "direction": "ingest",
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19310, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}
    }
  ]
}
EOF

# watch_wrap <what>: 20 s of memory and resyncs, then the grains. A resync of an hour or more is
# the wrap (or a jump) taken as a step.
MARK=0
watch_wrap() {
  local what="$1" peak=0 rss big video
  for _ in $(seq 1 40); do
    sleep 0.5
    kill -0 "$GW" 2>/dev/null || { echo "$what: gateway died" >&2; exit 1; }
    rss=$(awk '/VmRSS/ {print int($2 / 1024)}' "/proc/$GW/status")
    [ "$rss" -gt "$peak" ] && peak=$rss
  done
  echo "$what: peak RSS ${peak} MiB over 20 s"
  [ "$peak" -lt 1024 ] || { echo "$what: memory grew" >&2; exit 1; }
  big=$(tail -n +$((MARK + 1)) "$WORK/gw.log" | grep '"av_resync"' | python3 -c '
import json, sys
print(sum(1 for line in sys.stdin if abs(int(json.loads(line).get("jump_ms", "0"))) >= 3600000))') || true
  MARK=$(wc -l < "$WORK/gw.log")
  echo "$what: resyncs of an hour or more: $big"
  [ "$big" = 0 ] || { echo "$what: the wrap was taken as a step" >&2; exit 1; }
  video="$(python3 -c 'import json, urllib.request; print(json.load(urllib.request.urlopen("http://127.0.0.1:18150/api/v1/channels/rx-wrap/status"))["flows"]["video"])')"
  "$VERIFY" "$WORK/mxl/out" "$video" 10 > "$WORK/verify.txt" 2>&1 || true
  echo "$what: after the wrap $(tail -n 1 "$WORK/verify.txt")"
  grep -qE "grains ([5-9]|10)$" "$WORK/verify.txt" || { echo "$what: no video grains written after the wrap" >&2; exit 1; }
  python3 - "$what" <<'PY'
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:18150/api/v1/channels/rx-wrap/status"))
print(sys.argv[1] + ": state", s["state"], "decode_fps", round(s["decode_fps"], 1), "audio peak", round(max(s["meters"] or [0]), 3))
assert s["state"] == "running" and s["decode_fps"] > 10, s
assert s["meters"] and max(s["meters"]) > 0.05, s["meters"]
PY
}

mkdir -p "$WORK/mxl"
SRTGW_CONFIG_FILE="$WORK/config.json" stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
GW=$!
for _ in $(seq 1 20); do
  curl -fsS http://127.0.0.1:18150/livez 2>/dev/null | grep -q ok && break
  sleep 0.5
done

# 2^33 / 90000 = 95443.72 s: the PTS wraps about 4 s into the stream.
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc=size=1280x720:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
  -t 40 -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 25 -c:a aac -ac 2 \
  -output_ts_offset 95440 -f mpegts "srt://127.0.0.1:19310?mode=caller&latency=120" &
SRC=$!
watch_wrap "starts before the wrap"
kill -KILL "$SRC" 2>/dev/null || true
wait "$SRC" 2>/dev/null || true
sleep 3

# One connection: 6 s from PTS 0, then 30 s from 4 s before the wrap, paced at its bit rate.
segment() {
  ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc=size=1280x720:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -t "$1" -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 25 -b:v 4M -c:a aac -ac 2 \
    -output_ts_offset "$2" -f mpegts "$3"
}
segment 6 0 "$WORK/a.ts"
segment 30 95440 "$WORK/b.ts"
cat "$WORK/a.ts" "$WORK/b.ts" > "$WORK/ab.ts"
python3 -c '
import sys, time
data = open(sys.argv[1], "rb").read()
rate = len(data) / 36.0
start = time.monotonic()
for i in range(0, len(data), 1316 * 10):
    while time.monotonic() - start < i / rate:
        time.sleep(0.002)
    sys.stdout.buffer.write(data[i:i + 1316 * 10])
    sys.stdout.buffer.flush()
' "$WORK/ab.ts" | srt-live-transmit -q -chunk:1316 file://con "srt://127.0.0.1:19310?mode=caller&latency=120" &
SRC=$!
sleep 6
# The jump is read as about 10 s back (a bounded resync); the wrap 4 s later is no step at all.
watch_wrap "starts at 0, jumps to the wrap"
echo "PASS: PTS wrap"
