#!/usr/bin/env bash
# SIGTERM must exit 143, remove only this function's domain, and DELETE the NMOS node.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-srt-gateway}"
WORK="$(mktemp -d)"
REG_PORT=13910
WEB_PORT=18121
NMOS_PORT=13280
GW=""
REG=""
cleanup() {
  if [[ -n "${GW}" ]]; then kill -KILL "$GW" 2>/dev/null || true; fi
  if [[ -n "${REG}" ]]; then kill -KILL "$REG" 2>/dev/null || true; fi
  rm -rf "$WORK"
}
trap cleanup EXIT

start_gateway() {
  SRTGW_CONFIG_FILE="$WORK/config.json" \
    WEB_PORT="$WEB_PORT" \
    NMOS_PORT="$NMOS_PORT" \
    NMOS_DNS_SD=false \
    NMOS_SEED=shutdown-test \
    NMOS_LABEL="Shutdown test" \
    MXL_DOMAIN_SCAN_PATH="$WORK/mxl" \
    MXL_OUTPUT_DOMAIN_DIR="$WORK/mxl/own" \
    MXL_CLEANUP_ON_EXIT=true \
    STATE_DIR="$WORK/state" \
    SHUTDOWN_TIMEOUT_S=8 \
    DECODER=cpu \
    ENCODER=cpu \
    LOG_LEVEL=info \
    stdbuf -oL -eL "$BIN" > "$WORK/gw.log" 2>&1 &
  GW=$!
}

wait_ready() {
  local tries=$1
  for _ in $(seq 1 "$tries"); do
    if curl -sf "http://127.0.0.1:${WEB_PORT}/readyz" | grep -q '"ready":true'; then
      return 0
    fi
    if ! kill -0 "$GW" 2>/dev/null; then
      return 1
    fi
    sleep 0.25
  done
  return 1
}

stop_and_check() {
  kill -TERM "$GW"
  set +e
  wait "$GW"
  local code=$?
  set -e
  GW=""
  if [[ "$code" != 143 ]]; then
    echo "expected exit 143, got $code" >&2
    cat "$WORK/gw.log" >&2 || true
    exit 1
  fi
  if [[ -e "$WORK/mxl/own" ]]; then
    echo "own domain directory was not removed" >&2
    cat "$WORK/gw.log" >&2 || true
    exit 1
  fi
}

echo '{"channels":[]}' > "$WORK/config.json"
mkdir -p "$WORK/mxl" "$WORK/state"
start_gateway
if ! wait_ready 20; then
  echo "gateway did not become ready without a registry" >&2
  cat "$WORK/gw.log" >&2 || true
  exit 1
fi
test -f "$WORK/mxl/own/domain_def.json"
stop_and_check
echo "shutdown without registry removed the domain"

python3 - "$WORK/reg.log" "$REG_PORT" <<'PY' &
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
log_path, port = sys.argv[1], int(sys.argv[2])
seen = {"posted": False}

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def _body(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        return self.rfile.read(n) if n else b""
    def _send(self, code, payload=b""):
        data = payload or b""
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        if data:
            self.send_header("Content-Type", "application/json")
        self.end_headers()
        if data:
            self.wfile.write(data)
    def do_GET(self):
        with open(log_path, "a", encoding="utf-8") as log:
            log.write("GET " + self.path + "\n")
        if "/nodes/" in self.path and seen["posted"]:
            self._send(200, b'{"id":"node"}')
        elif "/nodes/" in self.path:
            self._send(404, b'{}')
        else:
            self._send(200, b"[]")
    def do_POST(self):
        body = self._body() or b"{}"
        with open(log_path, "a", encoding="utf-8") as log:
            log.write("POST " + self.path + "\n")
            log.flush()
        if "/health/" in self.path:
            self._send(200, b"{}")
            return
        seen["posted"] = True
        self._send(201, body)
    def do_DELETE(self):
        with open(log_path, "a", encoding="utf-8") as log:
            log.write("DELETE " + self.path + "\n")
            log.flush()
        self._send(204)
    def log_message(self, fmt, *args):
        return

ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
PY
REG=$!
for _ in $(seq 1 20); do
  curl -sf -o /dev/null "http://127.0.0.1:${REG_PORT}/x-nmos/query/v1.3/nodes" && break
  sleep 0.1
done
: > "$WORK/gw.log"
mkdir -p "$WORK/mxl" "$WORK/state"
export NMOS_REGISTRY_ADDRESS=127.0.0.1
export NMOS_REGISTRY_PORT="$REG_PORT"
export NMOS_QUERY_ADDRESS=127.0.0.1
export NMOS_QUERY_PORT="$REG_PORT"
start_gateway
if ! wait_ready 40; then
  echo "gateway did not register" >&2
  cat "$WORK/gw.log" >&2 || true
  echo "--- registry ---" >&2
  cat "$WORK/reg.log" >&2 || true
  exit 1
fi
stop_and_check
if ! grep -q "DELETE " "$WORK/reg.log"; then
  echo "registry did not see a DELETE" >&2
  cat "$WORK/reg.log" >&2 || true
  cat "$WORK/gw.log" >&2 || true
  exit 1
fi
echo "shutdown contract ok"
