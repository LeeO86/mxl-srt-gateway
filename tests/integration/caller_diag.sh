#!/usr/bin/env bash
# A caller whose far end is not there must say so at the default log level: its first failed
# attempt is one warn line, the same reason again is debug, and the channel status has the reason,
# when it happened and the count. The peer URL (status `peer_url`, the passphrase only from
# GET /channels/:id/peer-url?secrets=1) must open the channel in srt-live-transmit (and ffprobe,
# when its FFmpeg has SRT): the far end of the caller listens with it, and the gateway logs the
# recovery once; a player calls the listener with it and gets MPEG-TS.
# The passphrase must not appear in the status, the channel list, the metrics or the log.
# Usage: tests/integration/caller_diag.sh [gateway]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
WORK="$(mktemp -d)"
API=http://127.0.0.1:18190/api/v1
SECRET="diag-secret-0123"
GW=""
FAR=""
trap 'rc=$?; [ "$rc" = 0 ] || tail -n 40 "$WORK/gw.log" >&2 || true; kill $GW $FAR 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat > "$WORK/config.json" <<EOF
{
  "WEB_PORT": 18190,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19700-19799",
  "MXL_DOMAIN_SCAN_PATH": "$WORK/mxl",
  "MXL_OUTPUT_DOMAIN_DIR": "$WORK/mxl/out",
  "STATE_DIR": "$WORK/state",
  "DECODER": "cpu",
  "ENCODER": "cpu",
  "channels": [
    {
      "id": "tx-caller",
      "label": "TX caller",
      "direction": "egress",
      "srt": {"mode": "caller", "remote_host": "127.0.0.1", "remote_port": 19710, "latency_ms": 120,
              "connect_timeout_ms": 1000, "passphrase": "$SECRET", "streamid": "tx1"},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
      "egress": {"bitrate": 2000000, "preset": "ultrafast", "audio_preset": "stereo"}
    },
    {
      "id": "tx-listener",
      "label": "TX listener",
      "direction": "egress",
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19720, "latency_ms": 120,
              "passphrase": "$SECRET", "accepted_streamids": ["play"]},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
      "egress": {"bitrate": 2000000, "preset": "ultrafast", "audio_preset": "stereo"}
    }
  ]
}
EOF

field() { # field <channel> <python expression on the status s>
  python3 -c 'import json, sys, urllib.request
s = json.load(urllib.request.urlopen(sys.argv[1] + "/channels/" + sys.argv[2] + "/status"))
print(eval(sys.argv[3]))' "$API" "$1" "$2"
}
warns() { grep '"level":"warn"' "$WORK/gw.log" | grep '"event":"srt_connect_failed"' | grep -c '"channel":"tx-caller"' || true; }
no_secret() {
  python3 -c 'import sys, urllib.request; [sys.stdout.write(urllib.request.urlopen(u).read().decode()) for u in sys.argv[1:]]' \
    "$API/status" "$API/channels" "$API/config" "$API/channels/tx-caller/peer-url" "${API%/api/v1}/metrics" > "$WORK/public.txt"
  cat "$WORK/gw.log" >> "$WORK/public.txt"
  if grep -q "$SECRET" "$WORK/public.txt"; then
    echo "the passphrase leaked: $(grep -o ".\{40\}$SECRET.\{20\}" "$WORK/public.txt" | head -n 3)" >&2
    exit 1
  fi
}

mkdir -p "$WORK/mxl"
SRTGW_CONFIG_FILE="$WORK/config.json" stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
GW=$!
for _ in $(seq 1 20); do
  curl -fsS http://127.0.0.1:18190/livez 2>/dev/null | grep -q ok && break
  sleep 0.5
done

# 1. Nobody listens on 19710: at least 3 failed attempts (1 s timeout, backoff 1, 2, 4 s).
for _ in $(seq 1 40); do
  [ "$(field tx-caller 's["connect_failures"]')" -ge 3 ] && break
  sleep 0.5
done
failures="$(field tx-caller 's["connect_failures"]')"
reason="$(field tx-caller 's["last_failure"]')"
at="$(field tx-caller 's["last_failure_at"]')"
echo "dead far end: $failures failed attempts, last \"$reason\" at $at, $(warns) warn line(s)"
[ "$failures" -ge 3 ] || { echo "fewer than 3 failed attempts" >&2; exit 1; }
[[ "$reason" == srt_connect_failed:* ]] || { echo "unexpected last_failure: $reason" >&2; exit 1; }
[[ "$at" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$ ]] || { echo "last_failure_at is not UTC ISO 8601: $at" >&2; exit 1; }
[ "$(warns)" = 1 ] || { echo "expected one warn line for the same reason, got $(warns)" >&2; exit 1; }
curl -fsS http://127.0.0.1:18190/metrics | grep -q "^mxl_srt_gateway_srt_connect_failures_total{channel=\"tx-caller\"} [1-9]" \
  || { echo "no srt_connect_failures_total" >&2; exit 1; }

# 2. The peer URLs: without the passphrase in the status, with it only on ?secrets=1.
caller_url="$(field tx-caller 's["peer_url"]')"
listener_url="$(field tx-listener 's["peer_url"]')"
echo "peer URLs: $caller_url | $listener_url"
[ "$caller_url" = "srt://0.0.0.0:19710?mode=listener&latency=120" ] || { echo "caller peer_url: $caller_url" >&2; exit 1; }
[ "$listener_url" = "srt://127.0.0.1:19720?mode=caller&latency=120&streamid=play" ] || { echo "listener peer_url: $listener_url" >&2; exit 1; }
no_secret
secret_url() { curl -fsS "$API/channels/$1/peer-url?secrets=1" | python3 -c 'import json, sys; print(json.load(sys.stdin)["peer_url"])'; }
[ "$(secret_url tx-caller)" = "$caller_url&passphrase=$SECRET" ] || { echo "secrets peer URL: $(secret_url tx-caller)" >&2; exit 1; }

# 3. The far end listens with the caller's URL: the gateway connects and logs the recovery once.
srt-live-transmit -q "$(secret_url tx-caller)" file://con > "$WORK/far.ts" 2>/dev/null &
FAR=$!
for _ in $(seq 1 60); do
  grep '"event":"srt_connected"' "$WORK/gw.log" | grep -q '"channel":"tx-caller"' && break
  sleep 0.5
done
recovered="$(grep '"event":"srt_connected"' "$WORK/gw.log" | grep '"channel":"tx-caller"' | grep -c '"failed_attempts"' || true)"
echo "recovery: $(grep '"event":"srt_connected"' "$WORK/gw.log" | grep '"channel":"tx-caller"' | head -n 1)"
[ "$recovered" = 1 ] || { echo "expected one srt_connected with failed_attempts, got $recovered" >&2; exit 1; }
sleep 2
bytes=$(stat -c %s "$WORK/far.ts")
echo "far end (listener) received $bytes bytes"
[ "$bytes" -gt 18800 ] || { echo "the far end received no stream" >&2; exit 1; }
kill "$FAR" 2>/dev/null || true
wait "$FAR" 2>/dev/null || true
FAR=""

# 4. The far end is gone again: the first failure after the connect warns again.
for _ in $(seq 1 60); do
  [ "$(warns)" -ge 2 ] && break
  sleep 0.5
done
echo "after the far end left: $(warns) warn line(s), last \"$(field tx-caller 's["last_failure"]')\""
[ "$(warns)" = 2 ] || { echo "expected a second warn line after the connect, got $(warns)" >&2; exit 1; }

# 5. A player calls the listener with its URL (and the passphrase) and gets MPEG-TS.
timeout 6 srt-live-transmit -q "$(secret_url tx-listener)" file://con > "$WORK/play.ts" 2>/dev/null || true
python3 - "$WORK/play.ts" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
start = data.find(b"\x47")
packets = [data[i] for i in range(start, len(data) - 188, 188)] if start >= 0 else []
print("player (srt-live-transmit) received", len(data), "bytes,", sum(1 for b in packets if b == 0x47), "TS packets in sync")
assert len(packets) > 100 and all(b == 0x47 for b in packets), "not MPEG-TS"
PY
if ffprobe -hide_banner -protocols 2>/dev/null | grep -qw srt; then
  codecs="$(timeout 15 ffprobe -v fatal -show_entries stream=codec_name -of csv=p=0 "$(secret_url tx-listener)" | sort -u | tr '\n' ' ')" || true
  echo "player (ffprobe) streams: $codecs"
  [[ "$codecs" == *h264* ]] || { echo "ffprobe did not open the listener URL" >&2; exit 1; }
else
  echo "ffprobe has no SRT protocol: skipped"
fi
no_secret
echo "PASS: caller diagnostics and peer URLs"
