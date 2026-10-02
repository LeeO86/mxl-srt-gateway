#!/usr/bin/env bash
# AMWA IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 against a running node.
# Not part of default CI: the harness image is large. Start the gateway first
# (NMOS_PORT, default 3272) with a registry the suite can see.
set -euo pipefail

HOST="${NMOS_HOST:-127.0.0.1}"
PORT="${NMOS_PORT:-3272}"
IMAGE="${NMOS_TEST_IMAGE:-amwa/nmos-testing:latest}"

for suite in IS-04-01 IS-05-01 IS-05-02 BCP-007-03-01; do
  echo "Suite ${suite} against ${HOST}:${PORT}"
  docker run --rm --network host \
    -e "TEST_TARGET_HOST=${HOST}" \
    -e "TEST_TARGET_PORT=${PORT}" \
    "${IMAGE}" \
    python3 nmos-test.py --suite "${suite}" --host "${HOST}" --port "${PORT}"
done
