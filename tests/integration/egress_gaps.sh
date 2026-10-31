#!/usr/bin/env bash
# An egress whose MXL source misses grains: of the writer's video grains 20 % are skipped, written
# invalid, or written 120 ms late (a third each; mxl-srt-flow-writer "marks" 20). The egress must
# repeat the last good frame instead of sending its slate, keep its output rate, and the marks
# (3 white frames and a tone every 5 s) must come through an SRT loop to an ingest whole.
# Usage: tests/integration/egress_gaps.sh [gateway] [flow-writer] [avsync-check]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
WRITER="${2:-$(dirname "$BIN")/mxl-srt-flow-writer}"
CHECK="${3:-$(dirname "$BIN")/mxl-srt-avsync-check}"
SECONDS_MEASURED="${SRTGW_GAPS_SECONDS:-42}"
MISS="${SRTGW_GAPS_MISS_PERCENT:-20}"
WORK="$(mktemp -d)"
SRC_ID=5a5a5a5a-0000-4000-8000-000000000003
VIDEO=5a5a5a5a-0000-4000-8000-0000000000e1
AUDIO=5a5a5a5a-0000-4000-8000-0000000000e2
WPID=""
GW=""
trap 'rc=$?; [ "$rc" = 0 ] || tail -n 40 "$WORK/gw.log" >&2 || true; kill $GW $WPID 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat > "$WORK/config.json" <<EOF
{
  "WEB_PORT": 18170,
  "LOG_LEVEL": "info",
  "SRT_PORT_RANGE": "19500-19599",
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
      "srt": {"mode": "listener", "local_address": "127.0.0.1", "local_port": 19502, "latency_ms": 120},
      "target": {"width": 1280, "height": 720, "scan": "progressive", "rate": "25"},
      "egress": {"codec": "h264", "bitrate": 4000000, "preset": "ultrafast", "tune": "zerolatency",
                 "audio_tracks": [{"codec": "aac", "layout": "stereo", "channels": [0, 1], "language": "eng"}]}
    },
    {
      "id": "rx1",
      "label": "RX1",
      "direction": "ingest",
      "srt": {"mode": "caller", "remote_host": "127.0.0.1", "remote_port": 19502, "latency_ms": 120},
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
req = urllib.request.Request("http://127.0.0.1:18170/api/v1" + path, data=body, method=method, headers={"content-type": "application/json"})
print(urllib.request.urlopen(req, timeout=5).read().decode())
PY
}
flow() {
  api GET "/channels/$1/status" | python3 -c 'import json,sys; f=json.load(sys.stdin)["flows"]; print(f["video"] if sys.argv[1] == "video" else f["audio"][0])' "$2"
}

mkdir -p "$WORK/mxl"
"$WRITER" "$WORK/mxl/src" "$SRC_ID" "$VIDEO" 300 "$AUDIO" marks "$MISS" &
WPID=$!
for _ in $(seq 1 40); do
  [ -e "$WORK/mxl/src/$VIDEO.mxl-flow/data" ] && break
  sleep 0.05
done
SRTGW_CONFIG_FILE="$WORK/config.json" stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
GW=$!
for _ in $(seq 1 20); do
  api GET /channels >/dev/null 2>&1 && break
  sleep 0.5
done
api POST /channels/tx1/route "{\"video_active\":true,\"video_domain\":\"$SRC_ID\",\"video_flow\":\"$VIDEO\",\"audio_flow\":\"$AUDIO\"}" >/dev/null
sleep 6

api GET /channels/tx1/status > "$WORK/before.json"
"$CHECK" "$WORK/mxl/out" "$(flow rx1 video)" "$(flow rx1 audio)" "$SECONDS_MEASURED" 5 3 > "$WORK/marks.jsonl"
api GET /channels/tx1/status > "$WORK/after.json"

python3 - "$WORK/marks.jsonl" "$WORK/before.json" "$WORK/after.json" "$SECONDS_MEASURED" <<'PY'
import json, sys
lines = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
summary = next(l for l in lines if l.get("summary"))
marks = [l for l in lines if "av_ms" in l]
a, b = json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))
seconds = float(sys.argv[4])
d = lambda key: b.get(key, 0) - a.get(key, 0)
fps = d("frames") / seconds
expected = int(seconds // 5)
print("marks", summary["marks"], "valid", summary["valid"], "of about", expected, "| slate frames at the ingest", summary["slate_frames"])
print("AV (ms):", [m["av_ms"] for m in marks], "runs:", [m["run"] for m in marks])
print("egress: frames/s %.2f, repeated %d, slate %d, audio silent grains %d" % (fps, d("repeated_frames"), d("slate_frames"), d("audio_silent_grains")))
assert summary["marks"] >= expected - 1, "marks were lost"
assert summary["valid"] >= summary["marks"] - 1, "marks broken by slate frames"
assert summary["slate_frames"] == 0, "slate frames reached the ingest"
assert d("slate_frames") == 0, "the egress sent slate frames"
assert d("repeated_frames") > 0, "nothing was repeated: the source did not miss grains?"
assert fps >= 24.5, "the egress fell behind"
PY

kill -TERM "$GW" || true
wait "$GW" || true
echo "egress gaps ok"
