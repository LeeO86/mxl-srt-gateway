# Hardware notes

These measurements are for a lab host with the NVIDIA driver and the container
started with `--gpus all` (or `runtimeClassName: nvidia`). The same image starts
on a host with no GPU: `DECODER=auto` and `ENCODER=auto` stay on the CPU path
when NVDEC, NVENC or CUDA device creation fails.

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
