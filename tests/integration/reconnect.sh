#!/usr/bin/env bash
# An ingest whose SRT source drops and comes back must write MXL grains again within a few
# seconds, video and audio: as SRT caller (the remote listener restarts, with a new PTS base) and
# as SRT listener (the caller restarts from another source port, also with another format).
# Usage: tests/integration/reconnect.sh [gateway] [flow-verify]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
VERIFY="${2:-$(dirname "$BIN")/mxl-srt-flow-verify}"
WORK="$(mktemp -d)"
API=http://127.0.0.1:18140/api/v1
GW=""
SRC=""
trap 'rc=$?; [ "$rc" = 0 ] || tail -n 60 "$WORK/gw.log" >&2 || true; kill $GW $SRC 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat > "$WORK/config.json" <<EOF
{
  "WEB_PORT": 18140,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19200-19299",
  "MXL_DOMAIN_SCAN_PATH": "$WORK/mxl",
  "MXL_OUTPUT_DOMAIN_DIR": "$WORK/mxl/out",
  "STATE_DIR": "$WORK/state",
  "DECODER": "cpu",
  "ENCODER": "cpu",
  "channels": [
    {
      "id": "rx-caller",
      "label": "RX caller",
      "direction": "ingest",
      "srt": {"mode": "caller", "remote_host": "127.0.0.1", "remote_port": 19210, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}
    },
    {
      "id": "rx-listener",
      "label": "RX listener",
      "direction": "ingest",
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19220, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}
    }
  ]
}
EOF

# send <srt url> <seconds> <size> <rate> <codec> <pts offset s> [extra ffmpeg output args]
# A sender killed without an SRT shutdown, as a codec that drops off the network.
send() {
  local url="$1" seconds="$2" size="$3" rate="$4" codec="$5" offset="$6"
  shift 6
  ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc=size=$size:rate=$rate" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -t "$seconds" -c:v "$codec" -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g "$rate" -c:a aac -ac 2 \
    -output_ts_offset "$offset" "$@" -f mpegts "$url" &
  SRC=$!
}
stop_source() {
  kill -KILL "$SRC" 2>/dev/null || true
  wait "$SRC" 2>/dev/null || true
  SRC=""
}
flow() {
  python3 - "$1" "$2" <<'PY'
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:18140/api/v1/channels/%s/status" % sys.argv[1]))
f = s["flows"]
print(f["video"] if sys.argv[2] == "video" else f["audio"][0])
PY
}
# Valid video grains are being written now, and the ingest decodes again.
check_writing() {
  local channel="$1" what="$2"
  local video
  video="$(flow "$channel" video)"
  "$VERIFY" "$WORK/mxl/out" "$video" 10 > "$WORK/verify.txt" 2>&1 || true
  echo "$what: $(cat "$WORK/verify.txt")"
  grep -qE "grains ([5-9]|10)$" "$WORK/verify.txt" || { echo "$what: no video grains written" >&2; exit 1; }
  # Running, decoding, and the tone (peak 0.125) on the audio meters within 5 s.
  python3 - "$channel" "$what" <<'PY'
import json, sys, time, urllib.request
url = "http://127.0.0.1:18140/api/v1/channels/%s/status" % sys.argv[1]
for _ in range(10):
    s = json.load(urllib.request.urlopen(url))
    if s["state"] == "running" and s["decode_fps"] > 10 and s["meters"] and max(s["meters"]) > 0.05:
        break
    time.sleep(0.5)
print(sys.argv[2], "state", s["state"], "decode_fps", round(s["decode_fps"], 1), "source", s["source_format"], "audio peak", round(max(s["meters"] or [0]), 3))
assert s["state"] == "running", s
assert s["decode_fps"] > 10, s
assert s["meters"] and max(s["meters"]) > 0.05, s["meters"]
PY
}

mkdir -p "$WORK/mxl"
SRTGW_CONFIG_FILE="$WORK/config.json" stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
GW=$!
for _ in $(seq 1 20); do
  curl -fsS http://127.0.0.1:18140/livez 2>/dev/null | grep -q ok && break
  sleep 0.5
done

# Caller: the remote listener (a codec such as a Tieline) drops and comes back.
send "srt://127.0.0.1:19210?mode=listener&latency=120" 30 1280x720 25 libx264 0
sleep 5
check_writing rx-caller "caller, first connection"
stop_source
sleep 3
send "srt://127.0.0.1:19210?mode=listener&latency=120" 30 1280x720 25 libx264 5000
sleep 6
check_writing rx-caller "caller, remote listener restarted (new PTS base)"
stop_source
sleep 3
send "srt://127.0.0.1:19210?mode=listener&latency=120" 30 1920x1080 25 libx265 100 -mpegts_pmt_start_pid 0x1100 -mpegts_start_pid 0x22
sleep 6
check_writing rx-caller "caller, remote listener restarted (HEVC 1080p25, other PIDs)"
stop_source

# Listener: the caller drops and comes back from another source port, also in another format.
send "srt://127.0.0.1:19220?mode=caller&latency=120" 30 1280x720 25 libx264 0
sleep 5
check_writing rx-listener "listener, first connection"
stop_source
sleep 3
send "srt://127.0.0.1:19220?mode=caller&latency=120" 30 1280x720 25 libx264 7000
sleep 6
check_writing rx-listener "listener, caller restarted from another port (new PTS base)"
stop_source
sleep 3
send "srt://127.0.0.1:19220?mode=caller&latency=120" 30 1920x1080 50 libx264 0
sleep 6
check_writing rx-listener "listener, caller restarted (1080p50)"
stop_source

kill -TERM "$GW" || true
wait "$GW" || true
echo "reconnect ok"
