# mxl-srt-gateway — implementation plan

This is the map from `SPECIFICATION.md` draft v0.3 to the tree, including the
places the code does not follow a library's marketing name literally.

## Pins and the FFmpeg configure line

| Piece | Pin |
| --- | --- |
| MXL | `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` on `dmf-mxl/mxl`, `MXL_ENABLE_FABRICS_OFI=OFF` |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` (same commit as mxl-decklink / mxl-multiviewer; it has the MXL transport) |
| libsrt | `v1.5.4`, `ENABLE_ENCRYPTION=ON`, `USE_ENCLIB=openssl` |
| FFmpeg | `n7.1.5` |
| nv-codec-headers | `n12.2.72.0` |

FFmpeg configure line used by `docker/Dockerfile`:

```text
./configure \
  --prefix=/usr/local \
  --enable-gpl --enable-version3 \
  --enable-shared --disable-static \
  --disable-doc --disable-programs \
  --enable-libx264 --enable-libx265 --enable-libsoxr \
  --enable-openssl \
  --enable-ffnvcodec --enable-nvdec --enable-nvenc \
  --enable-cuda-llvm \
  --nvcc=clang \
  --nvccflags="--cuda-gpu-arch=sm_75 -O2" \
  --extra-cflags="-I/usr/local/cuda/include -I/usr/local/include" \
  --extra-ldflags="-L/usr/local/cuda/lib64 -L/usr/local/lib"
```

An image built that way is GPL-2.0-or-later because of libx264 and libx265.
CI on GitHub uses the distro FFmpeg 6.1 and libsrt 1.5.3 (GnuTLS) packages so
the job does not rebuild FFmpeg. The calls we make (`swr_alloc_set_opts2`,
the `AVChannelLayout` API, `AV_CODEC_ID_V210`, MPEG-TS `pcr_period`) exist on
both.

## Module map

- `src/config` — env over JSON file over defaults. Exit 78 on `ConfigError`.
- `src/media/adapt.cpp` — the §5.3 decision table and the FFmpeg filter string.
- `src/media/framesync.cpp` — repeat when the source is late, drop at most one
  source frame per output tick when it is early.
- `src/media/format.cpp` — 48 kHz grain cadence. 29.97 (30000/1001) alternates
  1601/1602. 59.94 (60000/1001) alternates 800/801. See the deviation below.
- `src/media/matrix.cpp` — gain, mute, and the named ingest/egress presets.
- `src/media/engine.cpp` — ingest and egress pipelines. libsrt is owned here;
  libavformat uses a custom AVIO.
- `src/mxl` — domain directory, `domain_def.json`, flow JSON, readers and writers.
  Compiled to no-ops when `find_package(mxl)` fails, so unit tests and a CPU
  SRT loop still run.
- `src/nmos` — UUIDv5 ids and, when `SRTGW_WITH_NMOS=ON`, one nmos-cpp node.
- `src/ops` — HTTP, WebSocket `/api/v1/events`, Prometheus text, the Vue UI.
- `web/` — Vue 3, Vite, inlined to one HTML file.

## Deviations

1. **v210 packing.** When the raster needs no adaptation, the CPU path converts
   between 8-bit 4:2:0 and v210 itself (`src/media/v210.cpp`, SSSE3 with a
   portable fallback, the arithmetic of the CUDA kernels): ingest packs the
   decoded picture straight into the grain, egress reads the grain in place
   into the encoder's 4:2:0 frame. Adapted pictures go through the FFmpeg
   `v210` encoder (and decoder) from `yuv422p10le`; `AV_PIX_FMT_V210` is not a
   pixel format in the FFmpeg 6.1 headers this tree also builds against, the
   codec is. The bytes are SMPTE v210 with rows padded to 128 bytes (48
   pixels), as MXL lays them out, and the flow media type is `video/v210`.
2. **59.94 audio cadence.** The specification's "1601/1602 at 59.94" matches
   29.97 fps (`30000/1001`): 48000 × 1001 / 30000 ≈ 1601.6. At 59.94 fps
   (`60000/1001`) the same rounding is 800/801. Writing 1602 samples into a
   59.94 grain would play audio at double speed. Both cadences are unit-tested.
3. **Listener bind address.** An empty local address binds `0.0.0.0`. Binding
   only the node IP rejects clients that arrive on another address of the same
   host (including 127.0.0.1 in tests and hairpin NAT). The UI still prints the
   public or node address a remote caller must use.
4. **Frame-rate conversion** is repeat/drop in the synchroniser. There is no
   motion interpolator (out of scope, §13).
5. **CUDA filters** (`bwdif_cuda`, `yadif_cuda`, `scale_cuda`) are compiled with
   Clang's NVPTX backend (`--enable-cuda-llvm`, `--cuda-gpu-arch=sm_75`).
   FFmpeg 7.1 marks `--enable-cuda-nvcc` nonfree, so that switch is not used:
   the image stays GPL-2.0-or-later from libx264 and libx265. sm_75 covers
   Turing and later, including the A4000 and L4. The filters dlopen `libcuda`;
   `libcudart` is not linked, so the process starts with no GPU. `libcuda`,
   `libnvidia-encode` and `libnvcuvid` come from the NVIDIA Container Toolkit
   when the container is started with a GPU and
   `NVIDIA_DRIVER_CAPABILITIES=compute,utility,video`. On ingest, an NVDEC
   frame stays on the device when the plan is a CUDA deinterlace or scale and
   those filters exist. A failed graph or a failed device open downloads that
   channel to the CPU graph. Egress adapts v210 on the CPU (that packing is
   not a `scale_cuda` input) and then tries NVENC.
6. **Rendezvous** is implemented (`SRTO_RENDEZVOUS`) but not required by the tests.
7. **Web UI (1.3.0).** Split into components like the sibling UIs (shared
   `style.css` tokens, `Pill`, `Segmented`, `OriginBadge`, `IdCode`). The audio
   matrix is a tab, not a dialog. Channel, matrix, route and settings edits are
   drafts in `web/src/store.js`. `audio_offset_ms` and `egress.profile` /
   `egress.level` are accepted by the API but not applied by the engine, so the
   UI has no control for them. The egress source picker uses
   `GET /api/v1/domains` and `POST /channels/:id/route`; that route does not
   change the receivers' IS-05 active parameters, and the next activation
   replaces it. Without `SRTGW_CONFIG_FILE` the Settings tab only edits
   `LOG_LEVEL` (a restart would forget the others). The export with
   passphrases is a download only.
8. **Audio alignment (1.3.1).** The specification asks for an asynchronous
   resampler with drift compensation; soxr in libswresample has no
   compensation (`swr_set_compensation` fails with it), so up to 1.3.0 the
   audio FIFO kept its start-up fill (0.6–1.1 s) and the audio was that much
   late against the video. The decoder now records each audio queue's source
   PTS and the video's PTS-to-TAI offset. Before each grain the writer takes
   the error of the queue's first sample against its mapped time +
   `sync_latency_ms` + half a grain and hands it to `AudioAligner`
   (`src/media/framesync.cpp`), which acts on the mean of 25 grains:
   - until a mean is within 20 ms (start-up: the video offset settles over
     2–3 s), it drops or inserts that many samples at once;
   - once aligned, it sets a speed of mean / 2 s, at most ±0.5 %; the IO
     thread passes it to `swr_set_compensation` with every decoded frame
     (over 2^20 output samples, so it never runs out between frames);
   - a mean beyond 100 ms is stepped again and the alignment starts over;
   - the error is taken against a hold (`holdNs`): per grain, error minus
     headroom (queued audio beyond the grain) is how much later the audio
     would have to be for the queue to just last; the hold rises at once to
     the window's worst of that + 10 ms and falls by at most 0.5 ms per
     window (2 ms let the speed swing between −0.5 and +0.4 % on bursts).
     FFmpeg's MPEG-TS muxer sends the audio in bursts later than the 120 ms
     sync latency: without the hold the queue ran dry before bursts (1–18 ms
     silences, several per second on the lab) and the aligner took the delay
     of each underrun back. Three first tries failed on the lab: capping the
     error at the window's lowest headroom made the speed swing between
     ±0.5 % every 0.5 s; adding the shortfall to the hold counted it twice
     when a source stall both drained the queue and moved the mapping (hold
     104 ms); taking the window's mean error against its lowest headroom
     kept the start-up backlog as a hold (270 ms, audio that much late).

   The resampler is libswresample's own engine, always on
   (`SWR_FLAG_RESAMPLE`, also 48 → 48 kHz, so the context is never
   re-initialised when the compensation starts), `filter_size` 64. Checked
   with FFmpeg 7.1's `aresample`: 48 → 48 kHz flat within 0.001 dB up to
   21 kHz (the default 32 taps lose 0.03 dB at 20 kHz and 0.36 dB at 21 kHz);
   a 1, 10 and 15 kHz tone resampled at a non-rational ratio leaves −95 to
   −108 dB. libsoxr stays in the FFmpeg build but is unused. An underrun does
   not restart the alignment: a first version did, and on the lab its
   re-alignment step 0.5 s later only added a second silence. `audio_drift_ppm`
   is the first track's speed, averaged over a minute (exponential); steps
   are re-alignments and not counted. Right after a start it still holds the
   start-up correction and settles over a few minutes.

   Measured on the lab (1080p50, CPU decode, ingest on 4 CPUs) in
   `~/mxl-lab/avs2/`: see the 1.3.1 entry of `CHANGELOG.md`. Open:
   - egress, found with the loop bench: it reads its audio at the output
     grain, not `read_offset_grains` back like its video, so its audio leaves
     2 grains (40 ms at 50p) early, and the read waits up to 20 ms for
     samples still being written. On a busy host the egress loop then falls
     behind and skips grains (~5 % of the frames on the lab); the stream's
     PTS count on without them (time runs slow, the audio jumps) and a timed
     out read sends silence. That is the −40…−60 ms of the loop. Reading the
     audio `read_offset_grains` back (lab-only build) moved the loop to
     −31…+2 ms and the egress drops from 2–3/s to under 1/s. Also,
     `MxlAudioReader::read` copies only the first fragment of a slice that
     wraps the ring: up to a grain of zeros once per ring (every 2.005 s on
     the test player's flow), the remaining silences of the loop;
   - the ingest frame queue (8 frames) caps the video's latency at ~150 ms:
     with `sync_latency_ms` 300 the channel stays on the slate (`no_signal`).
     A lab-only build with 32 frames ran at 300 ms, FFmpeg source +9…+19 ms;
   - `audio_offset_ms` is still not applied.

## MXL calls that matter

Writers use `mxlCreateFlowWriter`, `mxlFlowWriterOpenGrain` / `CommitGrain`
(`validSlices = totalSlices`, `MXL_GRAIN_FLAG_INVALID` is available for gaps)
and `mxlFlowWriterOpenSamples` / `CommitSamples`. Samples are addressed as
`count` samples ending at `index`. Readers use `mxlFlowReaderGetGrain` and
`GetSamples`. Egress waits with `mxlFlowSynchronizationGroupWaitForDataAt` on
the grain's TAI timestamp. Indexes use `mxlTimestampToIndex` / `mxlIndexToTimestamp`
when the library is linked, so the gateway and other readers share one clock.

Egress reads a grain in place (`MxlVideoReader::view`): it is done with it
long before the writer comes round to that slot again (history 1 s by
default, read offset 2 grains). It takes the grain at the output grain's
time minus the offset (`grainIndexAt`), not the newest one minus the offset,
which aliased with the writer's commit. The ingest's direct path fills the
grain in place (`MxlVideoWriter::writeWith`).

Interlaced flows: MXL doubles the declared `grain_rate` (25 for 1080i50) and
each grain is one field (height/2 rows). Ingest writes frame k as grains 2k
(first field: top when `tff`) and 2k + 1, egress weaves frame k from them
(`copyV210Field`, `interleaveV210Fields`, `yuv420ToV210(…, parity)`).

`domain_def.json` carries the domain id. `options.json` sets
`urn:x-mxl:option:history_duration/v1.0` only when this process creates the
directory (default 1 s). An existing file is left alone.

## NMOS

Ids are UUIDv5 under the URL namespace, from `NMOS_SEED` (default
`HOST_ID-srtgw`). An ingest target-format change changes the flow id and
therefore the IS-04 flow and the sender's active `mxl_flow_id`. A source-format
change does not. Egress receivers accept activation when the flow is not on
disk yet; the channel stays `waiting` and retries. The SRT side is not an
NMOS transport in v1.

## Tests

`./build/unit-tests` covers the decision table, synchroniser, cadence, matrix,
config precedence, access control, v210 stride and metrics text.

`tests/integration/ci.sh` pushes FFmpeg SRT into an ingest listener, checks
format and frame count, pulls the egress listener and checks H.264 720p, checks
caller mode, and checks that an `internet` listener logs a stream-id reject
without returning the passphrase. Set `SRTGW_TEST_SECONDS=60` for the long TAI
run. `tests/nmos/amwa.sh` is the AMWA suite runner, not part of default CI.

Hardware numbers for A4000 and L4 are recorded by the procedure in
`docs/hardware.md` rather than invented here. A lab run on an NVIDIA A16 is
recorded there; A4000 and L4 are still open.

## Platform guideline G1–G14

| Item | Status | Evidence |
| --- | --- | --- |
| G1 Configuration | met | Env, then file, then defaults in `src/config/config.cpp`. Unknown env names are not read (`environmentValues`). Invalid values throw `ConfigError` and `src/main.cpp` exits 78. Settings table in `README.md`. State is `STATE_DIR` (default `/config`). Passphrases are redacted in `src/util/logging.cpp` and omitted from export unless `secrets=1`. |
| G2 MXL domains | met | Scan path and output dir/id in `src/config/config.cpp`. `ensureOutputDomain` (`src/mxl/domain_files.cpp`) creates a missing domain and refuses to overwrite a different `domain_def.json`. `options.json` is written only when absent. `MXL_HISTORY_DURATION_MS` sets history. |
| G3 NMOS identity | met | `makeNmosIds` (`src/nmos/ids.cpp`) is UUIDv5 from `NMOS_SEED`, including the domain id. `NMOS_LABEL` and `NMOS_TAGS` are applied in `src/nmos/node.cpp`. Group hints stay. |
| G4 Registry, no DNS-SD | met | `NMOS_QUERY_ADDRESS` defaults to the registry address and `NMOS_QUERY_PORT` to registration port + 1. `NMOS_DNS_SD` defaults false and sets `pri`, `highest_pri` and `authorization_highest_pri` to max int (`src/nmos/node.cpp`). The Avahi client library stays linked; no daemon is required while DNS-SD is off. |
| G5 Announce IP addresses | met | `NMOS_HOST_ADDRESS` (alias `SRTGW_PUBLIC_IP`) must be a non-loopback IPv4 when set. It is the NMOS `host_address` / `host_addresses` and the UI request line (`announceAddress`). `HOST_ID` is only the default seed. |
| G6 Ports | met | `WEB_PORT`, `NMOS_PORT` (WebSocket + 1) and per-channel SRT ports. A listener UDP bind failure throws and `main` exits 75. Web bind failure returns 75. |
| G7 Health and metrics | met | `/livez` is process liveness. `/readyz` (`src/ops/api.cpp`) is 200 only when channels are up and, with a registry, `NmosNode::registered` sees the node on the Query API. `/metrics` uses the `mxl_srt_gateway_` prefix. |
| G8 Clean shutdown | met | SIGTERM stops channels, `httpDelete`s the node, optionally `removeOwnDomain`, and exits 143 (`src/main.cpp`). `SHUTDOWN_TIMEOUT_S` defaults to 10. `MXL_CLEANUP_ON_EXIT` defaults false. |
| G9 IS-05 | met | Senders publish active `mxl_domain_id` and `mxl_flow_id`. Activation with `master_enable` false clears the route. Active routes are stored in `STATE_DIR/routes.json`. |
| G10 Config export and import | met | `GET /api/v1/config/export` and `POST /api/v1/config/import`. Secrets are omitted unless `secrets=1`. An omitted passphrase on import keeps the stored one. |
| G11 Image and CI | met | `.github/workflows/container.yaml` pushes `git-<sha7>` and `nightly-dev` on `main`, and `X.Y.Z`, `X.Y`, `X` on a `vX.Y.Z` tag. `latest` is not published. OCI labels include source, revision, licenses and `io.dmf.mxl.revision`. The runtime user is uid 1000. |
| G12 Kubernetes example | met | `deploy/mxl-srt-gateway.yaml`: pod network, standard env, `/livez` and `/readyz`, grace period 30, MXL hostPath, writable `/config`, `supplementalGroups: [1000]`, no `hostIPC`. |
| G13 Documentation | met | `README.md`, `CHANGELOG.md` 1.0.0, `SPECIFICATION.md` settings table. |
| G14 Tests | met | `tests/unit/test_logic.cpp` covers the new settings and domain files. `tests/integration/shutdown.sh` covers ready, SIGTERM, exit 143, node DELETE and domain removal. |
