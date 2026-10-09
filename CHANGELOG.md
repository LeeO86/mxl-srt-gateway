# Changelog

## 1.3.1

### Fixes

- Ingest audio was written to MXL up to about a second later than the matching video. The audio FIFO kept whatever piled up while the stream started (stream probing, slate time), often 0.6–1.1 s, and nothing brought it back to the 120 ms target: the soxr resampler ignores `swr_set_compensation`, and the hard trim only started 1 s above the target. Measured on the lab with the mxl-test-player A/V sync pattern (egress → SRT → ingest, 1080p50, against the player's own flows): 1.2.1 and 1.3.0 wrote the beep 900 ms (FIFO 1090 ms) or 460 ms (FIFO 650 ms) after the flash, and 100 ms early after an audio gap had drained the FIFO. The audio queue now keeps the source timestamps, and before each grain the writer lines it up with the video's PTS-to-TAI mapping (drop the older samples or insert silence when it is more than 20 ms off). Without video, the queue is held at `sync_latency_ms`.
- `audio_drift_ppm` was the FIFO level error × 10⁶ (8,000,000 at a 1 s FIFO). It is now the net of samples dropped (+) and inserted (−) per sample written after the first alignment, so the source audio clock against TAI.
- The Compose and Kubernetes examples use the `1.3.1` image.

## 1.3.0

- New web UI in the look of the other LeeO86 media functions (mxl-webrtc-monitor, mxl-test-player, mxl-replay, mxl-multiviewer, mxl-st2110-gateway, mxl-browser-source): header with the node label, channel counts, running, waiting, failed, alarm, registration and connection pills and the versions; banners for a lost API, lost live updates, a needed restart and an action's result or error; tabs in the URL hash; light and dark theme. Every API function has a control:
  - **Dashboard**: every channel with its state, picture (about once a second), source and target format, SRT peer, RTT, rate, lost/retransmitted/dropped packets, buffer, decoder or encoder, frame sync repeats and drops, audio FIFO, MXL audio meters, alarms and errors, backup or second destination on air.
  - **Channels**: every channel setting as a draft with Apply and Revert; only that channel restarts. Label, enabled; SRT mode (caller, listener, rendezvous), remote host and port, local address and port, latency, connect timeout, stream ID or accepted stream IDs, exposure, key length, passphrase (write-only, *Remove the passphrase*), allowed peers, payload size, overhead, max bandwidth; the ingest backup input (failover, failback, which input is on air) or the egress second destination; decoder or encoder override, program and PID selection, codec, bitrate, GOP, B-frames, x264 preset, tune and threads, NVENC preset and tuning, MPEG-TS service, provider, program number, PIDs, PCR and mux, MXL read offset; output raster, scan, rate, field order, aspect, scaler, deinterlacer, source scan; frame sync latency, hold and loss picture. New ingest and egress channels, enable/disable, delete (asks once more). Checks before Apply (port range, caller target, passphrase length, internet rules, interlaced rasters, HEVC interlaced, taken IDs). Next to it the live status, the address line for the far end or the network team (copy), and the MXL side: the ingest's output flows, or the egress source with a picker over the flows on the host.
  - **Audio**: the ingest matrix (decoded track channels to MXL channels, presets, gain and mute per MXL channel, channel count, meters, decoded tracks with PID, codec, layout, language, missing) and the egress tracks (presets, MXL channel per slot, codec, layout, language, bitrate, gain, mute, PID). It was a dialog.
  - **NMOS & MXL**: node, device, address, registry and Query API, registration, DNS-SD, seed, links to the raw IS-04 and IS-05 resources; every sender, flow and receiver with its channel state and, for receivers, the routed flow, domain and `master_enable`; every MXL domain on the host with its flows.
  - **Status**: `/livez`, `/readyz` with its parts, restart required, versions (gateway, MXL, nmos-cpp, libsrt, FFmpeg), memory, NVDEC/NVENC sessions and GPU memory from `/metrics`, and per channel the SRT, codec, frame sync and audio numbers.
  - **Settings**: every setting with its value and origin (ENV, FILE, DEFAULT) and whether it needs a restart; a save sends only the changed keys. Export as JSON or `KEY=value` (copy, download), the JSON with passphrases as a download only; import a JSON document or `KEY=value` lines from a file or pasted text.
  - Edits are drafts in the page: tab switches, status pushes and WebSocket reconnects keep them. Passphrases are never shown; the API still only says `passphrase_set`.
- API additions (backwards compatible): `GET /api/v1/info` (version, MXL, nmos-cpp, libsrt and FFmpeg refs, node label, announced address, config file). `GET /api/v1/domains` (every MXL domain under `MXL_DOMAIN_SCAN_PATH` with label, path, mirror, whether it is this gateway's, and its flows with label, format, media type, size, rate or channel count). `egress.audio_preset` selects the egress audio preset.

### Fixes

- An egress channel's audio tracks were replaced by one stereo AAC track whenever its document carried `egress.preset` (the x264 preset), and every saved channel does: at each start from `SRTGW_CONFIG_FILE`, on every channel `PUT` with the whole document (the 1.2 UI sent that), and on import. `egress.preset` was read as the audio preset as well. It is the x264 preset only now; an audio preset name there still selects the tracks, and `egress.audio_preset` (or `egress_preset` on `PUT /channels/:id/matrix`) is the audio preset.
- `PUT /api/v1/config` and a `KEY=value` import dropped the environment: the saved values replaced environment ones in the running configuration and every origin became FILE. The environment keeps precedence now; a JSON import keeps it as well.
- `PUT /api/v1/config` set `restart_required` for keys whose value did not change (the 1.2 UI sent ten keys on every save) and for a change it then rejected. It is set only for a changed value that is accepted; LOG_LEVEL never needs it.
- A decoded track's `layout` stayed `0ch` when the stream did not tell the channel count before the first frame (AAC in MPEG-TS). The audio matrix then offered two rows per track, so channels 3–6 of a 5.1 track could not be routed. The layout now comes from the decoded frames.
- The Compose and Kubernetes examples use the `1.3.0` image.

## 1.2.1

- A new output `domain_def.json` carries `description` and `tags`, as BCP-007-03 requires (`id`, `label`, `description`, `tags`). The gateway wrote only `id` and `label`, and mxl-st2110-gateway 1.0.2 skipped such domains. An existing file is still not rewritten.

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
