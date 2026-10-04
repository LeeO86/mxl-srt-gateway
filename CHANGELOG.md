# Changelog

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
