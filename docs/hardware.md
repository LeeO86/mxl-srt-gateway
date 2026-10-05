# Hardware notes

These measurements are for a lab host with the NVIDIA driver and the container
started with `--gpus all` (or `runtimeClassName: nvidia`). The image sets
`NVIDIA_DRIVER_CAPABILITIES=compute,utility,video`; without `video` the toolkit
does not mount `libnvcuvid` or `libnvidia-encode`, and NVDEC/NVENC cannot open.
The same image starts on a host with no GPU: `DECODER=auto` and `ENCODER=auto`
stay on the CPU path when NVDEC, NVENC or CUDA device creation fails. CUDA
filters are used on ingest when NVDEC produced a device frame. Egress encode
uses NVENC after the CPU v210 adapter.

## What to record

On an A4000 and an L4:

1. Concurrent 1080p50 ingest channels (H.264, target 1080p50) until decode FPS
   falls or `mxl_srt_gateway_decode_fps` misses the target.
2. Concurrent 1080p50 egress channels at 15 Mbit/s, B-frames 0, until
   `mxl_srt_gateway_encode_latency_seconds` p95 exceeds one frame.
3. End-to-end latency with the mxl-test-player A/V sync pattern:
   SRT `latency_ms` + `sync_latency_ms` + codec delay. The dashboard RTT and
   the frame-sync repeat/drop counters should stay flat while the pattern runs.

`mxl_srt_gateway_nvdec_sessions`, `mxl_srt_gateway_nvenc_sessions` and
`mxl_srt_gateway_gpu_memory_bytes` come from `nvidia-smi` when it is on `PATH`.

4:2:2 or 10-bit sources that NVDEC refuses are decoded on the CPU and the
channel status reports the CPU decoder name. Interlaced HEVC is rejected at
configuration time. Interlaced H.264 is offered to the encoder and, if the
open fails, the channel enters `error` with that reason.

## Lab run 2026-10-05: CPU only (1.2.0)

Same host, containers without a GPU (`DECODER=cpu`, `ENCODER=cpu`). N egress
channels read the test player's 1080p50 flows and send H.264 at 15 Mbit/s
(`veryfast`, `zerolatency`, B-frames 0) to N ingest listeners of a second
gateway; 20 s warm-up, 40 s measured; cores are the containers' CPU time.

| Channels | Version | Encode fps | Decode fps (avg) | Ingest repeats / drops | Ingest / egress CPU |
| --- | --- | --- | --- | --- | --- |
| 1 | 1.1.2 | 46.6 | 36.0 | 98 / 3 | 0.9 / 2.5 cores |
| 4 | 1.1.2 | 41.7 | 36.3 | 1577 / 462 | 3.4 / 8.4 cores |
| 1 | 1.2.0 | 50.0 | 49.2 | 0 / 0 | 0.3 / 1.3 cores |
| 4 | 1.2.0 | 50.0 | 49.4 | 0 / 0 | 0.9 / 4.4 cores |
| 8 | 1.2.0 | 50.0 | 49.5 | 0 / 0 | 1.9 / 9.2 cores |
| 1 | 1.2.0, `threads: 4` | 50.0 | – | 0 / 0 | 0.3 / 0.9 cores |
| 4 | 1.2.0, `threads: 4` | 50.0 | 49.3 | 0 / 0 | 0.9 / 3.2 cores |
| 8 | 1.2.0, `threads: 4` | 50.0 | 48.5 | 0 / 0 | 1.9 / 6.9 cores |

Where 1.1.2 spent the time (perf, one channel): egress 66 % x264, 17 % kernel
page faults (two 5.5 MB buffers allocated and copied per frame), 8 % swscale;
ingest 47 % swscale (4:2:0 to 10-bit 4:2:2), 34 % decode, and the decode
thread could not keep 50 fps. In 1.2.0 the egress is x264 plus about 6 % for
the conversion. x264's default with `zerolatency` is 16 slice threads per
encoder; `threads: 4` saves 22 % at 8 channels (less waking, fewer slices),
keeps p95 encode latency at ≤ 10 ms and leaves about 0.86 cores per 1080p50
`veryfast` channel.

GPU path with 1.2.0 (`DECODER`/`ENCODER=auto`, 8 channels): 0 repeats or
drops, 1.5 / 1.1 cores (1.1.0: 2.5 / 2.2), NVENC 91 %.

1280×720p50 round trip (ffmpeg `testsrc2` → ingest → egress → ingest), CPU
and GPU: 1.2.0 passes the picture; 1.1.2's egress sent only its loss slate
(the v210 row size of a width that is not a multiple of 48 was wrong).

## Lab run 2026-10-03: NVIDIA A16

Not one of the target GPUs. The A16 is four GA107 GPUs (one NVENC, two NVDEC
engines each, PCIe Gen4 x4 per GPU); each gateway used one GPU. Host: 2× Xeon
Gold 6136, driver 595.84, image built from this repository with the fixes in
the CHANGELOG 1.0.1 section (1.0.0 itself left ingest channels on the
slate after a stall and fell back to CPU decode from about 13 channels).

Egress: N channels read 1080p50 v210 flows from mxl-test-player (colour bars
with burn-in, four flows shared round robin) and send H.264 at 15 Mbit/s,
B-frames 0, as SRT callers to N ingest listeners of a second gateway on the
same host. Each case ran 20 s warm-up and 40 s measured.

| Channels | Encoder | Encode fps (avg / min) | `encode_latency_seconds` p95 | NVENC load | Egress CPU |
| --- | --- | --- | --- | --- | --- |
| 4 | h264_nvenc | 49.6 / 49.3 | ≤ 5 ms | 39 % | 3.0 cores |
| 8 | h264_nvenc | 49.1 / 48.6 | ≤ 5 ms | 78 % | 6.8 cores |
| 10 | h264_nvenc | 47.2 / 41.7 | ≤ 5 ms | 94 % | 9.5 cores |
| 12 | h264_nvenc | 42.6 / 35.0 | ≤ 20 ms | 100 % | 10.5 cores |
| 8 | libx264 (`veryfast`) | 35.6 / 29.6 | ≤ 20 ms | – | 15.1 cores |

Egress result: one A16 GPU keeps 8 channels of 1080p50 at 15 Mbit/s; NVENC is
the limit at 10. The CPU encoder does not keep 8 channels on this host.

Ingest: N listeners decode 1080p50 H.264 at 15 Mbit/s (with AAC) into 1080p50
v210 flows. With the egress gateway as the source, frame-sync repeats and drops
stayed below 0.1 per second per channel at 4 channels and about 0.7 at 8. To
take NVENC out of the way, N `ffmpeg -re -stream_loop -1 -c copy` senders then
replayed one 20 s file:

| Channels | Decoder | NVDEC load | Ingest CPU |
| --- | --- | --- | --- |
| 1 | h264_cuvid | 3 % | 0.8 cores |
| 4 | h264_cuvid | 12 % | 3.2 cores |
| 16 | h264_cuvid | 57 % | 19.4 cores |
| 24 | h264_cuvid | 79 % | 30.6 cores |

Ingest result: NVDEC is not the limit at 24 channels; the host is, at about
1.3 cores per channel for the download, the 4:2:2 10-bit conversion and the
v210 pack on the CPU. (The replayed file restarts its timestamps every 20 s,
so its frame-sync counters are not a quality measure.)

`mxl_srt_gateway_decode_fps` restarts its window every second, so single
readings jump between about 35 and 55 fps while the output stays at 50 fps.

Not run here: end-to-end latency with the A/V sync pattern (item 3), 4:2:2 or
10-bit sources, interlaced H.264 encode.

## Lab run 2026-10-04: GPU v210 conversion (1.1.0)

Same host, GPUs and method as above. 1.0.1 against 1.1.0, built from this
repository, measured one after the other the same day.

Round trip, egress gateway as the source (the frame-sync counters are a quality
measure here), 40 s:

| Channels | Image | Encode fps (avg) | Decode fps (avg) | Frame sync repeats / drops | CPU ingest / egress | NVENC load |
| --- | --- | --- | --- | --- | --- | --- |
| 8 | 1.0.1 | 46.8 | 44.7 | 1191 / 517 | 7.4 / 7.8 cores | 77 % |
| 8 | 1.1.0 | 49.6 | 48.5 | 0 / 0 | 2.2 / 2.5 cores | 90 % |
| 12 | 1.0.1 | 32.3 | 26.5 | 1471 / 2883 | 10.4 / 11.4 cores | 77 % |
| 12 | 1.1.0 | 37.4 | 35.9 | 5869 / 1434 | 3.1 / 3.1 cores | 100 % |

From 10 channels NVENC is the limit (one engine per A16 GPU), with either image.

Ingest only (`ffmpeg -re -c copy` senders as above; CPU, not quality):

| Channels | 1.0.1 ingest CPU | 1.1.0 ingest CPU | NVDEC load (1.1.0) |
| --- | --- | --- | --- |
| 8 | 7.3 cores | 2.4 cores | 28 % |
| 16 | 21.9 cores | 6.7 cores | 62 % |
| 24 | 28.2 cores | 14.5 cores | 68 % |

1.1.0 downloads the packed v210 (5.5 MB per frame) instead of NV12 (3.1 MB).
Through pageable memory that did not fit the A16's x4 link at 16 channels (the
frame-sync counters went from about 1 to about 10 per second per channel); the
copies now go through page-locked memory and 16 channels are back at about 1.
24 channels need 6.6 GB/s of download, more than the x4 link carries; a target
GPU on x16 has about four times the bandwidth.

