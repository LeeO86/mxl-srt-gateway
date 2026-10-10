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
defaults. Unknown environment variables are ignored. Invalid configuration
exits **78**. A port that cannot be bound, or a domain that cannot be created,
exits **75**. SIGTERM exits **143**. A clean run that is stopped another way
exits **0**.

State the process writes (the config file when `SRTGW_CONFIG_FILE` points
there, and `routes.json` for IS-05 activations) lives under `STATE_DIR`
(default `/config`). Mount that directory if it must survive a restart.
Passphrases are stored in the config file in plain text, because that file is
the secret; they are never logged and are omitted from the API unless
`GET /api/v1/config/export?secrets=1`.

| Key | Default | Meaning |
| --- | --- | --- |
| `WEB_PORT` | `8120` | UI, REST, `/livez`, `/readyz`, `/metrics` |
| `NMOS_PORT` | `3272` | IS-04/IS-05 node API. WebSocket is `NMOS_PORT`+1 |
| `SRT_PORT_RANGE` | `9000-9099` | listener ports this process may bind |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | parent of every domain directory, mirrors included |
| `MXL_OUTPUT_DOMAIN_DIR` | `/Volumes/mxl/srtgw-<seed>` | this function's domain, created if missing |
| `MXL_OUTPUT_DOMAIN_ID` | UUIDv5 from `NMOS_SEED` | stable domain id. An existing `domain_def.json` with a different id is not overwritten |
| `MXL_HISTORY_DURATION_MS` | `1000` | `history_duration` written when this process creates `options.json`. `SRTGW_HISTORY_DURATION_NS` is the old name |
| `MXL_CLEANUP_ON_EXIT` | `false` | on SIGTERM, delete only this function's output domain directory |
| `STATE_DIR` | `/config` | writable state directory |
| `SHUTDOWN_TIMEOUT_S` | `10` | bound on SIGTERM work |
| `DECODER` / `ENCODER` | `auto` | `auto`, `nvdec`/`nvenc`, or `cpu` |
| `NMOS_SEED` | `HOST_ID-srtgw` | UUIDv5 seed for the node, device, flows, senders, receivers and domain id |
| `NMOS_LABEL` | node label is `HOST_ID`, device label is `MXL SRT Gateway` | node label and device label |
| `NMOS_TAGS` | empty | JSON object of tag name to array of strings, copied onto the node and device |
| `NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT` | empty / `3210` | Registration API. Empty address means the node does not register |
| `NMOS_QUERY_ADDRESS` | the registry address | Query API host used by `/readyz` |
| `NMOS_QUERY_PORT` | registry port + 1 | Query API port. The registry's Query WebSocket is that port + 1 (`P+2` when registration is `P`) |
| `NMOS_DNS_SD` | `false` | `false` disables DNS-SD browse and mDNS advertisement (`pri` and `highest_pri` are max int). No Avahi daemon is required |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | address announced on the node, in IS-05, and in the UI request line. Must be an IPv4 literal, not `0.0.0.0`, `127.0.0.1` or a hostname. `SRTGW_PUBLIC_IP` is the same setting |
| `HOST_ID` | hostname | identity used in the default seed. It is not announced as an address |
| `LOG_LEVEL` | `info` | |
| `SRTGW_CONFIG_FILE` | unset | JSON file of the settings above plus `channels` |

`/readyz` is 200 only when channels are up and, if `NMOS_REGISTRY_ADDRESS` is
set, the Query API lists this node. `/livez` is 200 while the process is up.
`/metrics` is Prometheus text with the `mxl_srt_gateway_` prefix.

REST, all under `/api/v1`: `GET /status`, `GET /info`, `GET /nmos`,
`GET /domains`, `GET|PUT /config`, `GET /config/export` (`?format=env`,
`?secrets=1`), `POST /config/import`, `GET|POST /channels`,
`GET|PUT|DELETE /channels/:id`, `PUT /channels/:id/matrix`,
`POST /channels/:id/route`, `GET /channels/:id/status`, `GET /channels/:id/thumbnail`,
WebSocket `/api/v1/events`. `GET /info` has the versions, the node label, the
announced address and the config file. `GET /domains` lists the MXL domains
under `MXL_DOMAIN_SCAN_PATH` with their flows. `egress.preset` is the x264
preset; `egress.audio_preset` picks the audio tracks (`8x-stereo-aac`,
`16ch-302m`, `5.1+stereo`, `stereo`).

The web UI on `WEB_PORT` has the tabs Dashboard (every channel with picture,
SRT statistics and meters), Channels (all channel settings, new, enable,
delete, the egress MXL source), Audio (ingest matrix, egress tracks), NMOS &
MXL, Status and Settings (origins, export, import). Edits stay drafts until
they are applied, across tab switches and reconnects. Passphrases are
write-only.

On the platform, set `NMOS_HOST_ADDRESS` to the pod IP (or the node IP on a
host network), `NMOS_SEED` to `<production>-srtgw`, `NMOS_LABEL` and
`NMOS_TAGS`, `MXL_CLEANUP_ON_EXIT=true`, and mount `/Volumes/mxl` plus a
writable `/config`. `deploy/mxl-srt-gateway.yaml` is that shape. Two instances
on one node need distinct `WEB_PORT`, `NMOS_PORT` and `SRT_PORT_RANGE`.

The image is built with NVDEC, NVENC and the CUDA filters (`bwdif_cuda`,
`yadif_cuda`, `scale_cuda`). Those libraries are loaded when a device is
opened, so the process starts on a host with no GPU and stays on the CPU
codecs. To use the GPU, run it with the NVIDIA Container Toolkit
(`docker run --gpus all` or `runtimeClassName: nvidia`). The image sets
`NVIDIA_DRIVER_CAPABILITIES=compute,utility,video` so the toolkit mounts
`libcuda`, `libnvidia-encode` and `libnvcuvid`. `auto` tries NVDEC, the CUDA
filters on those frames, and NVENC, and falls back per stream when the device
or the graph cannot be opened.

On the CPU (no GPU, or `ENCODER=cpu`), egress encodes with libx264 at the
channel's `egress.preset` (default `veryfast`) and `egress.tune` (default
`zerolatency`). `egress.threads` sets libx264's thread count: 0 (default)
leaves it to x264, which takes 16 slice threads at 1080p; 4 used 22 % less
CPU at 8 × 1080p50 on the lab host and kept p95 encode latency at ≤ 10 ms
([docs/hardware.md](docs/hardware.md)). NVENC ignores it.

Ingest audio follows the video: each track is queued with its source
timestamps and plays `sync_latency_ms` after the time its PTS maps to, like the
video frames. At the start the gateway drops or inserts samples until the error
is within 20 ms. After that the resampler plays the audio up to 0.5 % faster or
slower, so a drifting source or a decoder that falls behind does not click. An
error beyond 100 ms (a timestamp jump, a long stall) is stepped again. Audio
that arrives in bursts later than `sync_latency_ms` (FFmpeg's MPEG-TS muxer
does that) is held back until the queue no longer runs dry, and released
slowly: a little late rather than with gaps. `audio_offset_ms` (−1000 to 1000,
+: later) moves an ingest channel's audio against its video, for a source that
is out of lip sync; egress ignores it. In the channel status,
`audio_fifo_ms` is the queued audio and `audio_drift_ppm` the first track's
resampling correction, averaged over about a minute from 10 s after the
alignment (+: the source audio runs fast against TAI). The thresholds are
fixed (`SPECIFICATION.md` §5.4).

`sync_latency_ms` (default 120, at most 2000) is how far the ingest's output
lags the source's mapped time. The ingest queues that much decoded video plus
100 ms (at least 8 frames); a queued 1080p frame takes 3–5.5 MB, so 2000 ms
at 50p holds up to about 580 MB per channel.

Egress reads each output grain's video and audio `egress.read_offset_grains`
(default 2) source frames before its time, so that the writer has finished
them, and stamps each encoded audio frame with the time of its first sample.

## Layout

`src/` is the gateway, `web/` the Vue 3 UI embedded in the binary, `tests/`
doctest plus `tests/integration/ci.sh`, `deploy/` the Kubernetes manifests and
Grafana dashboard, `docker/` the image and Compose examples.
