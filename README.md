# mxl-srt-gateway

Bridges compressed SRT contribution streams and uncompressed MXL flows, in both
directions, on one host.

- **Ingest (SRT → MXL):** MPEG-TS over SRT, decode, adapt to the target raster,
  frame-synchronise to TAI, write `video/v210` and `audio/float32`.
- **Egress (MXL → SRT):** read MXL, optionally adapt, encode, mux MPEG-TS, send SRT.

One process serves many channels. A failing channel does not stop the others.
The MXL side is a normal NMOS node (IS-04 / IS-05, BCP-007-03). The SRT side is
configured in the UI, the REST API or `SRTGW_CONFIG_FILE`.

The container image links libx264 and libx265, so the image is
**GPL-2.0-or-later**. The gateway sources in this repository stay MIT. See
`THIRD_PARTY_NOTICES.md`.

## Prefer caller mode

When the far end can listen, configure the channel as **caller**. Only outbound
UDP is required. Use an internet-facing **listener** only with the NAT rule in
`docs/network.md`, a passphrase (AES-256 by default) and a stream-id allow-list.
`exposure=internet` refuses to start without those.

SRT is UDP and does not pass a corporate HTTP proxy.

## Build

C++20, CMake ≥ 3.24, Ninja, GCC ≥ 12 or Clang ≥ 16. The unit tests need no
media libraries. The `mxl-srt-gateway` binary needs FFmpeg (libavformat,
libavcodec, libavfilter, libswresample, libswscale) and libsrt.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/mxl;$HOME/mxl/build/vcpkg_installed/x64-linux" \
  -DSRTGW_WITH_NMOS=ON -DNMOS_CPP_DIR=/tmp/nmos-cpp/Development
cmake --build build -j"$(nproc)"
./build/unit-tests
SRTGW_TEST_SECONDS=8 tests/integration/ci.sh ./build/mxl-srt-gateway
```

Pins, recorded in `IMPLEMENTATION_PLAN.md`:

| Component | Pin |
| --- | --- |
| MXL `release/v1.1` | `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` (`-DMXL_ENABLE_FABRICS_OFI=OFF`) |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` |
| libsrt | `v1.5.4` (OpenSSL encryption in the image) |
| FFmpeg | `n7.1.5` |

`docker/Dockerfile` is the build that produces `ghcr.io/leeo86/mxl-srt-gateway`.
It is labelled `io.dmf.mxl.revision`.

## Run

Environment overrides the JSON file (`SRTGW_CONFIG_FILE`), which overrides the
defaults. Invalid configuration exits **78**. A domain that cannot be created
exits **75**. SIGTERM exits **143**.

| Key | Default |
| --- | --- |
| `WEB_PORT` | `8120` |
| `NMOS_PORT` | `3272` |
| `SRT_PORT_RANGE` | `9000-9099` |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` |
| `MXL_OUTPUT_DOMAIN_DIR` | `/Volumes/mxl/srtgw-<seed>` |
| `DECODER` / `ENCODER` | `auto` (`nvdec`/`nvenc` or `cpu`) |
| `SRTGW_PUBLIC_IP` | address shown to remote callers |

Health and metrics: `/livez`, `/readyz`, `/statusz`, `/metrics`.
The UI and REST API are on `/` and `/api/v1/`. Passphrases are write-only.

Without a GPU the process still starts and uses the CPU codecs. With a GPU,
`auto` tries NVDEC/NVENC and CUDA filters (`bwdif_cuda`, `scale_cuda`) and
falls back per stream.

## Layout

`src/` is the gateway, `web/` the Vue 3 UI embedded in the binary, `tests/`
doctest plus `tests/integration/ci.sh`, `deploy/` the Kubernetes manifests and
Grafana dashboard, `docker/` the image and Compose examples.
