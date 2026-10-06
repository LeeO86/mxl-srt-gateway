# Changelog

## 1.2.0

Measured on the lab host without a GPU ([docs/hardware.md](docs/hardware.md), 1080p50, H.264 15 Mbit/s `veryfast`, egress into ingest on the same host): one channel now keeps real time on the CPU (1.1.2: 46.6 fps encode, 36 fps decode), and 8 channels run with no frame-sync repeat or drop at 1.9 / 6.8 cores with `threads: 4`. 1.1.2 did not keep 4 channels (41.7 fps, 3.4 / 8.4 cores). On the GPU, 8 channels take 1.6 / 1.3 cores instead of 2.5 / 2.2. Interlaced MXL flows (one field per grain) work now.

- v210 rows are padded to 128 bytes (48 pixels), as MXL and FFmpeg lay them out. Before, widths that are not a multiple of 48 got the wrong row size: a 1280×720 egress only ever sent its loss slate, and the 720p ingest slate and black picture were sheared (GPU path included).
- CPU ingest: when nothing needs adapting, the writer packs the decoded 4:2:0 picture to v210 straight into the MXL grain (SSSE3). Before, swscale converted it to 10-bit 4:2:2, FFmpeg's v210 encoder packed it into a new buffer per frame, and the writer copied that into the grain.
- CPU egress: the grain is read in place and converted straight to the 4:2:0 frame x264 takes (SSSE3, one frame reused). Before, every frame copied the grain and the slate, then went through FFmpeg's v210 decoder and swscale.
- The new conversions use the GPU kernels' arithmetic (4:2:0 to 4:2:2: chroma 3:1 from the two nearest rows; 4:2:2 to 4:2:0: mean of two rows; within the field when interlaced), so the CPU and GPU paths give the same picture. Unit tests compare both directions byte for byte with a transcription of the kernels, with and without SSSE3.
- `egress.threads` sets libx264's thread count (default 0: x264's own choice, 16 slice threads at 1080p with `zerolatency`). With 4, 8 × 1080p50 `veryfast` took 22 % less egress CPU on the lab host and p95 encode latency stayed ≤ 10 ms.
- The ingest's packed pictures (GPU path and adapted CPU path) come from a pool instead of a new allocation per frame; egress reads grains in place on the GPU path too.
- Interlaced MXL flows: MXL doubles an interlaced flow's declared `grain_rate` and each grain holds one field. Ingest now writes frame k as grains 2k (first field: the top one when `tff`) and 2k + 1; egress weaves frame k from those two grains. Before, ingest wrote half a frame into each field grain and a 1080i egress only ever sent its slate. Checked on the lab with black top and white bottom fields through ingest → egress → ingest, CPU and GPU.
- Interlaced egress tells libx264 the field order on every frame; the stream said bottom field first before.
- Egress takes the source grain `read_offset_grains` before the output grain's time (MXL grains are indexed by time) instead of the newest grain minus the offset; it still follows a source that runs later than that. The newest grain moved between n−1 and n depending on whether the writer's commit came just before or after the egress tick: with 1.1.2, 4–10 repeat/drop pairs per channel in 45 s on the lab, now 0.

## 1.1.2

- A busy `NMOS_PORT` or `NMOS_PORT`+1 exits 75 again within a second. 1.1.1 noticed the failed listener only after nmos-cpp had started, and stopping that server hung: on the lab host the process neither exited nor served (killed after 9 minutes). The ports are now bound and released once before the node starts. The CI test passed with 1.1.1; the lab host showed the hang.

## 1.1.1

- A busy `NMOS_PORT` exits 75, like a busy web port. nmos-cpp swallows listener errors, so the gateway now checks that it really listens on the port after the node started (`/proc/net/tcp{,6}` and its own sockets), as the ST 2110 gateway does. Before, it ran on without IS-04/IS-05; a start error it did see was only logged (`nmos_start_failed`).

## 1.1.0

Measured on the lab A16 ([docs/hardware.md](docs/hardware.md)): 8 channels through egress and ingest now keep 50 fps with no frame-sync repeat or drop, at 2.2 / 2.5 cores instead of 7.4 / 7.8; 16 ingest channels take 6.7 cores instead of 21.9.

- NVDEC ingest: when every adaptation step runs on CUDA, or none is needed, the decoded frame stays on the GPU and is packed to v210 there; only the packed picture is downloaded. Before, each frame was downloaded as 4:2:0, converted to 10-bit 4:2:2 and packed to v210 on the CPU.
- NVENC egress: v210 becomes an NV12 CUDA frame on the GPU when the raster needs no adaptation; otherwise the CPU-adapted frame is uploaded as NV12. Before, NVENC got CPU frames in yuv420p.
- All channels share the GPU's primary CUDA context instead of one context per connection.
- The GPU copies go through a per-thread page-locked buffer; from or into pageable memory they ran at about half the link rate.
- The v210 encoder and decoder contexts are kept per thread instead of being opened for every frame, and the ingest writer shares each decoded picture instead of copying it per output grain.
- The kernels are compiled to PTX by Clang (like FFmpeg's `--enable-cuda-llvm` filters) and loaded through the driver at runtime. Without clang or the ffnvcodec headers at build time, or without a driver at runtime, the CPU path is used; a failed GPU conversion logs `gpu_v210_failed` or `gpu_nv12_failed` and the channel continues on the CPU.

## 1.0.1

- An ingest channel that fell more than the frame queue (8 frames) behind
  the decoder wrote the loss slate forever while it reported `running`, and
  counted a frame-sync drop on every grain. It now jumps to the newest frame.
- The device is re-registered only when its sender or receiver list changes,
  with a new version. Before, it was sent every 500 ms with the old version and
  the registry answered 400 each time.
- The process raises its open-file soft limit to the hard limit at start. Each
  MXL flow keeps a descriptor per grain, and with Docker's default of 1024 the
  CUDA device of about the 13th ingest channel failed to open (silent CPU
  fallback).
- The SIGTERM DELETE of the node used `/resource/node/<id>`; the Registration
  API path is `/resource/nodes/<id>` (nmos-cpp answered 404).

## 1.0.0

Stable settings and API contract for the MXL platform. A later breaking change
needs 2.0.0.

- `NMOS_HOST_ADDRESS` is the address announced to NMOS, IS-05 and the UI.
  `SRTGW_PUBLIC_IP` is the same setting.
- `NMOS_LABEL` and `NMOS_TAGS` set the node and device label and tags.
  `NMOS_SEED` still derives every id.
- `NMOS_QUERY_ADDRESS` defaults to the registry address.
  `NMOS_QUERY_PORT` defaults to the registration port plus one.
  `NMOS_DNS_SD=false` (the default) does not browse or advertise with mDNS.
- `/readyz` stays 503 until the Query API lists the node when a registry is set.
- SIGTERM deletes the node from the Registration API, and with
  `MXL_CLEANUP_ON_EXIT=true` removes only this function's output domain.
  The exit code is 143. `SHUTDOWN_TIMEOUT_S` defaults to 10.
- `MXL_HISTORY_DURATION_MS` sets the domain history. `SRTGW_HISTORY_DURATION_NS`
  still works.
- IS-05 activations are stored in `STATE_DIR/routes.json` (default `/config`).
- `GET /api/v1/config/export` omits passphrases unless `secrets=1`.
  An import that omits a passphrase keeps the one already stored.
  Passphrases live in the config file and are not logged.
- An existing `domain_def.json` with a different id is not overwritten.
- The image is published as `git-<sha7>` and `nightly-dev` from `main`, and as
  `X.Y.Z`, `X.Y` and `X` from a `vX.Y.Z` tag. Version tags are not moved.
