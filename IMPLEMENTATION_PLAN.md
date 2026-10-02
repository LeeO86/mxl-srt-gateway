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

1. **v210 packing.** The gateway asks the FFmpeg `v210` encoder (and decoder)
   to convert `yuv422p10le` to the packed 10-bit 4:2:2 grain. `AV_PIX_FMT_V210`
   is not a pixel format in the FFmpeg 6.1 headers this tree also builds
   against; the codec is. The bytes are still SMPTE v210 and the flow media
   type is `video/v210`.
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
5. **CUDA filters** are requested by name (`bwdif_cuda`, `scale_cuda`) when the
   build has them. If the graph fails, or NVDEC cannot open a 4:2:2 stream,
   that channel falls back to CPU and says so in the log and in `codec_info`.
   libcudart is not linked into this binary; FFmpeg loads it when a CUDA filter
   is used. The process starts with no GPU.
6. **Rendezvous** is implemented (`SRTO_RENDEZVOUS`) but not required by the tests.

## MXL calls that matter

Writers use `mxlCreateFlowWriter`, `mxlFlowWriterOpenGrain` / `CommitGrain`
(`validSlices = totalSlices`, `MXL_GRAIN_FLAG_INVALID` is available for gaps)
and `mxlFlowWriterOpenSamples` / `CommitSamples`. Samples are addressed as
`count` samples ending at `index`. Readers use `mxlFlowReaderGetGrain` and
`GetSamples`. Egress waits with `mxlFlowSynchronizationGroupWaitForDataAt` on
the grain's TAI timestamp. Indexes use `mxlTimestampToIndex` / `mxlIndexToTimestamp`
when the library is linked, so the gateway and other readers share one clock.

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
`docs/hardware.md` rather than invented here.
