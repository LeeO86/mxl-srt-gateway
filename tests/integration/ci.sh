#!/usr/bin/env bash
# CPU integration checks that do not need a GPU. Durations are shortened with
# SRTGW_TEST_SECONDS (default 8). The specification's 60 s TAI check is the
# same script with SRTGW_TEST_SECONDS=60.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
SECONDS_RUN="${SRTGW_TEST_SECONDS:-8}"
WORK="$(mktemp -d)"
trap 'kill "$GW" 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat > "$WORK/config.json" <<EOF
{
  "WEB_PORT": 18120,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19000-19099",
  "MXL_DOMAIN_SCAN_PATH": "$WORK/mxl",
  "MXL_OUTPUT_DOMAIN_DIR": "$WORK/mxl/out",
  "DECODER": "cpu",
  "ENCODER": "cpu",
  "channels": [
    {
      "id": "ingest-listen",
      "label": "Listen",
      "direction": "ingest",
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19000, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
      "sync_latency_ms": 80,
      "hold_ms": 400
    },
    {
      "id": "egress-listen",
      "label": "Out",
      "direction": "egress",
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19002, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
      "egress": {"codec": "h264", "bitrate": 3000000, "preset": "ultrafast", "tune": "zerolatency", "audio_tracks": [{"codec": "aac", "layout": "stereo", "channels": [0, 1], "bitrate": 128000, "language": "eng"}]}
    }
  ]
}
EOF

SRTGW_CONFIG_FILE="$WORK/config.json" stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
GW=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if curl -fsS http://127.0.0.1:18120/livez 2>/dev/null | grep -q ok; then
    break
  fi
  sleep 0.5
done
if ! curl -fsS http://127.0.0.1:18120/livez | grep -q ok; then
  echo "gateway did not become live" >&2
  cat "$WORK/gw.log" >&2 || true
  exit 1
fi
curl -fsS http://127.0.0.1:18120/readyz | grep -q true

ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc=size=1280x720:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
  -t "$SECONDS_RUN" -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 25 -c:a aac -f mpegts \
  "srt://127.0.0.1:19000?mode=caller&latency=120"
python3 - <<'PY'
import json, os, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:18120/api/v1/channels/ingest-listen/status"))
need = int(os.environ.get("SRTGW_TEST_SECONDS", "8"))
assert s["source_format"] == "720p25", s
assert s["target_format"] == "720p25", s
assert s["decoder"].startswith("h264"), s
assert s["frames"] >= max(10, need * 10), s
print("ingest", s["state"], s["frames"], s["decoder"])
PY

ffmpeg -hide_banner -loglevel info -i "srt://127.0.0.1:19002?mode=caller&transtype=live&latency=200" -t 1 -f null - \
  > "$WORK/recv.log" 2>&1 || true
grep -q "1280x720" "$WORK/recv.log"
grep -q "h264" "$WORK/recv.log"
echo "egress stream identified"

# Caller ingest: gateway dials ffmpeg's listener.
curl -fsS -X POST http://127.0.0.1:18120/api/v1/channels -H 'content-type: application/json' -d '{
  "id": "ingest-call",
  "label": "Call",
  "direction": "ingest",
  "srt": {"mode": "caller", "remote_host": "127.0.0.1", "remote_port": 19010, "latency_ms": 120},
  "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}
}' >/dev/null
ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc=size=1280x720:rate=25" -t 3 -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 25 -f mpegts \
  "srt://127.0.0.1:19010?mode=listener&latency=120" &
sleep 2
python3 - <<'PY'
import json, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:18120/api/v1/channels/ingest-call/status"))
assert s["frames"] > 5, s
print("ingest caller", s["state"], s["frames"])
PY

# Internet listener rejects a caller with the wrong stream id.
curl -fsS -X POST http://127.0.0.1:18120/api/v1/channels -H 'content-type: application/json' -d '{
  "id": "edge",
  "label": "Edge",
  "direction": "ingest",
  "srt": {
    "mode": "listener", "local_address": "127.0.0.1", "local_port": 19020, "latency_ms": 120,
    "exposure": "internet", "passphrase": "correct-horse-battery", "pbkeylen": 32,
    "accepted_streamids": ["news-1"]
  },
  "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"}
}' >/dev/null
set +e
ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc=size=320x180:rate=25" -t 2 -c:v libx264 -preset ultrafast -f mpegts \
  "srt://127.0.0.1:19020?mode=caller&passphrase=correct-horse-battery&pbkeylen=32&streamid=wrong" >/dev/null 2>&1
set -e
grep -q '"reason":"streamid"' "$WORK/gw.log"
echo "internet streamid rejected"

# Passphrases never come back from the API.
curl -fsS http://127.0.0.1:18120/api/v1/channels/edge | grep -q passphrase_set
if curl -fsS http://127.0.0.1:18120/api/v1/channels/edge | grep -q "correct-horse"; then
  echo "passphrase leaked" >&2
  exit 1
fi
echo "passphrase masked"

kill -TERM "$GW" || true
for _ in $(seq 1 40); do
  if ! kill -0 "$GW" 2>/dev/null; then
    break
  fi
  sleep 0.25
done
if kill -0 "$GW" 2>/dev/null; then
  kill -KILL "$GW" || true
fi
wait "$GW" || true
echo "integration ok"
