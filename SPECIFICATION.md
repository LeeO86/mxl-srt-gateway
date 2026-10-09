# mxl-srt-gateway — Specification

Status: Draft v0.3 (for implementation by Claude Code or Cursor in a new, empty repository)

Changelog: v0.3 — caller and listener mode required for both directions, with a mode table and tests (§5.0). v0.2 — multichannel audio in both directions made a v1 requirement (§5.5, §6.2); SRT sources from internal WAN **and** the internet through the corporate firewall, with the security rules that follow (§5.7, §10); candidate list updated (§13).
Repository: `LeeO86/mxl-srt-gateway` (name can still change)
Aligns with: `LeeO86/mxl-decklink`, `LeeO86/mxl-st2110-gateway`, `LeeO86/mxl-fabrics-agent`,
`LeeO86/mxl-multiviewer`, `LeeO86/mxl-webrtc-monitor`, platform repo `mmz-srf/mxl-poc-platform`

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

---

## 1. Purpose and scope

`mxl-srt-gateway` bridges compressed SRT contribution streams and uncompressed MXL
flows, in both directions, on one host:

- **Ingest (SRT → MXL):** receive an MPEG-TS over SRT, decode (NVDEC or CPU),
  **adapt the video to the configured target format** (deinterlace, interlace,
  scale, frame-rate convert, colour convert), frame-synchronise to TAI, and write
  MXL `video/v210` and `audio/float32` flows.
- **Egress (MXL → SRT):** read MXL video and audio flows, optionally convert the
  format, encode (NVENC or CPU), multiplex into MPEG-TS and send over SRT.

One container serves several **channels**; each channel is either ingest or egress.

Design principles:

1. **MXL side is standard NMOS.** Ingest channels are BCP-007-03 MXL senders,
   egress channels are BCP-007-03 MXL receivers; both are routed by the platform's
   crosspoint like every other media function.
2. **The MXL timeline is TAI.** SRT sources are not locked to the facility clock.
   Ingest therefore always runs a frame synchroniser: video repeats/drops frames,
   audio follows the video timeline by dropping or inserting samples. The MXL
   output never stalls and never runs ahead of TAI.
3. **Compressed edge, own timing core.** Demux, decode, encode and mux use the
   FFmpeg libraries (libavformat, libavcodec, libavfilter, libswresample), SRT uses
   libsrt directly. Timing, frame sync and MXL I/O are own C++ code, not driven by
   a library clock. GStreamer is not used.
4. **GPU when available, CPU always possible.** NVDEC/NVENC and CUDA filters when
   an NVIDIA GPU is present; complete CPU fallback in the same image.
5. **Conventions of the sibling repos**: config model, admin UI, `/metrics`, exit
   codes, CI, Compose and Kubernetes, uid 1000, MXL root `/Volumes/mxl`.

Out of scope for v1 (see §13 for the candidates): SRT socket groups/bonding,
SCTE-35, subtitles/teletext, HDR, ST 2110, RTMP/RIST/WebRTC, recording.

---

## 2. Architecture

```
 INGEST channel
 libsrt (caller/listener) ─► TS demux (libavformat, custom AVIO)
   ─► video decode (NVDEC | CPU) ─► format adapter (CUDA | CPU filters)
   ─► audio decode ─► resampler ─► audio queue (aligned to video PTS) ─► channel map
   ─► frame synchroniser (TAI clock) ─► MXL writers (v210, float32)

 EGRESS channel
 MXL readers (v210, float32, TAI-aligned)
   ─► optional format adapter ─► video encode (NVENC | CPU)
   ─► audio map ─► audio encode
   ─► TS mux (libavformat, custom AVIO) ─► libsrt (caller/listener)

 per process: nmos-cpp node, web server (UI, REST, WebSocket), /metrics
```

- One thread group per channel; a failing channel never affects another.
- The libsrt socket is owned by the channel; libavformat reads/writes through a
  custom AVIO context, so SRT statistics and options are fully under our control.

---

## 3. Technology and build

- C++20, CMake ≥ 3.24, Ninja; GCC ≥ 12 or Clang ≥ 16.
- MXL `dmf-mxl/mxl` `release/v1.1` at `218ddaa` (shared platform pin, one build
  variable), `-DMXL_ENABLE_FABRICS_OFI=OFF`.
- nmos-cpp at the commit used by mxl-decklink / mxl-fabrics-agent / mxl-multiviewer.
- libsrt 1.5.x (exact version pinned), built with encryption (OpenSSL).
- FFmpeg 7.x libraries (exact version pinned), built in the image with: libsrt not
  required (we use libsrt directly), nvcodec headers (NVDEC/NVENC via
  `ffnvcodec`), CUDA filters if available (`scale_cuda`, `bwdif_cuda` or
  `yadif_cuda`), libx264, libx265 (optional), libsoxr for resampling. Record the
  configure line in `IMPLEMENTATION_PLAN.md`. Licensing: an image containing
  libx264/libx265 is GPL; document that in the README and `THIRD_PARTY_NOTICES.md`.
- CUDA runtime linked statically, as in mxl-multiviewer; the image MUST start on a
  host without a GPU and fall back to CPU.
- Web UI: Vue 3 SPA embedded in the binary, no CDN.
- Tests: doctest (vendored), shell integration tests.
- Follow the real APIs of the pinned libraries; record every deviation.

Repository layout as the siblings (`.github/workflows`, `cmake`, `deploy`,
`docker`, `src`, `tests`, `third_party`, `web`, `AGENTS.md`,
`IMPLEMENTATION_PLAN.md`, `README.md`, `SPECIFICATION.md`, `LICENSE`).

---

## 4. NMOS

- One nmos-cpp Node, one Device ("MXL SRT Gateway"); deterministic UUIDv5 IDs from
  `NMOS_SEED` and the channel/essence index.
- **Ingest channel** → per essence an MXL sender with IS-04 Source and Flow:
  one video sender, one audio sender per audio output flow (see §5.5). Group hint
  `urn:x-nmos:tag:grouphint/v1.0` = `<channel label>:Video` / `:Audio <n>`.
  Active params point at this process's own output domain and flow IDs.
  A change of the **target format** mints a new flow ID and updates the IS-04 Flow
  and the sender's active `mxl_flow_id` (like mxl-decklink). A change of the
  **source** format does **not** change the flow, because the adapter keeps the
  output at the target format.
- **Egress channel** → one video receiver and one audio receiver (grouped).
  Activation is accepted even if the domain or flow is not on disk yet (`waiting`,
  retry with backoff); the IS-04 `subscription` is updated on every activation.
- The SRT side is configured through the UI/REST API, not NMOS, in v1. (NMOS
  control of the SRT side is a candidate feature, §13.)
- Static registry (`NMOS_REGISTRY_ADDRESS`/`_PORT`), DNS-SD off by default.
- The node advertises an address reachable from the registry and crosspoint (pod
  IP on the pod network; host IP with host networking).

---

## 5. Ingest (SRT → MXL)

### 5.0 SRT connection modes (both directions)

Every channel, ingest and egress, MUST support both **caller** and **listener**
mode, selectable per channel in the UI, REST API and config file, and changeable at
runtime (only that channel reconnects). Rendezvous mode is optional.

| Direction | Caller | Listener | Default |
| --- | --- | --- | --- |
| Ingest (SRT → MXL) | gateway connects to a remote SRT sender/listener and pulls the stream | gateway waits on a local UDP port for a remote caller to push | listener |
| Egress (MXL → SRT) | gateway connects to a remote SRT listener and pushes the stream | gateway waits on a local UDP port for a remote caller to pull | caller |

- Caller settings: remote host/IP and port, optional local bind address/port,
  `streamid` to send, connect timeout, reconnect backoff (1 s → 10 s).
- Listener settings: local bind address (default: the node IP), port from
  `SRT_PORT_RANGE`, accepted `streamid` values, maximum one connected peer per
  channel (a second peer is rejected and logged).
- Both modes share latency, encryption, bandwidth and payload options, the
  statistics, and the access rules of §5.7; the UI shows the connected peer's
  address in both modes.
- Integration tests cover all four combinations (ingest caller/listener, egress
  caller/listener).

### 5.1 SRT input

- Modes: **listener** (default) and **caller** (§5.0); rendezvous optional.
- Options per channel: port / remote host:port, `latency_ms` (default 200),
  passphrase and key length (AES-128/192/256), `streamid` (caller: sent; listener:
  accepted values configurable), max bandwidth / overhead, payload size 1316.
- Listener accepts one connection per channel; a second caller is rejected and
  logged.
- Reconnect: caller retries with backoff (1 s → 10 s); listener keeps listening.
- Optional backup input: a second SRT source (listener port or caller URL);
  automatic switch on loss of the main input after `failover_ms`, switch back
  after `failback_ms` of stable main signal (or manual). SRT socket groups are not
  used in v1.

### 5.2 Demux and decode

- MPEG-TS; program selection: first program (default), by program number, or by
  service name; video PID and audio PIDs selectable (default: first video, all
  audio).
- Video codecs: H.264, HEVC, MPEG-2 (NVDEC or CPU); AV1 if the GPU supports it.
  4:2:0 and 4:2:2, 8- and 10-bit (NVDEC may not support 4:2:2 for all codecs —
  fall back to CPU decode for that stream and report it).
- Audio codecs: AAC (LC, HE), MP2, AC-3/E-AC-3 (decoder availability as built),
  SMPTE 302M (PCM), Opus.
- Decoder selection `DECODER=auto|nvdec|cpu`; per channel the chosen decoder is
  shown in status and metrics.

### 5.3 Target format and adaptation

Each ingest channel has a **target format** that defines the MXL output flow:
raster (1920×1080, 1280×720, 3840×2160), scan (progressive/interlaced), rate
(23.98, 24, 25, 29.97, 30, 50, 59.94, 60 frames or 1080i at 25 and 29.97 frames),
colour BT.709 (v1; SDR only). The output is always the target, whatever arrives.

| Source → Target | Processing |
| --- | --- |
| interlaced → progressive, same field rate (e.g. 1080i50 → 1080p50) | field-rate deinterlace (bwdif, one frame per field) |
| interlaced → progressive, frame rate (e.g. 1080i50 → 1080p25) | frame-rate deinterlace (bwdif, one frame per frame) |
| progressive → interlaced (e.g. 1080p50 → 1080i50) | interlace (two frames → one frame, top field first; field order configurable) |
| progressive 25 → interlaced 25 (1080p25 → 1080i50) | PsF-style: both fields from the same frame |
| raster change | scale (bicubic/lanczos on CPU, `scale_cuda` on GPU), aspect preserved with pillar-/letterbox or fill (configurable) |
| SD (BT.601) → HD | colour matrix conversion to BT.709 |
| frame-rate change (e.g. 59.94 → 50) | handled by the frame synchroniser (repeat/drop); no motion interpolation in v1 |
| interlaced → interlaced, different field order | field shift, configurable |
| anamorphic SD | honour the source sample aspect ratio |

- Detection: field order, scan and aspect come from the stream; per channel an
  override (`source_scan=auto|progressive|tff|bff`) handles mis-flagged streams.
- Deinterlacer selection: `bwdif` (default), `yadif`, `weave` (none); on GPU the
  CUDA variant when available, else CPU.
- Conversion to v210 (10-bit 4:2:2) at the end of the adapter; 8-bit sources are
  expanded.

### 5.4 Frame synchroniser and timing

- The MXL writer runs on the TAI grain clock of the target rate: for each output
  grain index, it takes the newest adapted frame whose (source-time-mapped)
  presentation time is ≤ output time − `sync_latency_ms` (default 120 ms).
- Source timestamps are mapped to the local timeline with a smoothed offset; the
  synchroniser **repeats** a frame when the source is slow and **drops** one when
  it is fast. Repeat/drop events are counted (metrics) and spaced, not bursty.
- Audio is resampled to 48 kHz float32 (libswresample with soxr) and queued with
  its source timestamps. Before each grain, the writer lines the queue up with
  the video: the first queued sample plays at its mapped presentation time +
  `sync_latency_ms` (half a grain later, like the frame the synchroniser shows).
  When the mean error over 25 grains is more than 20 ms, it drops the older
  samples or inserts silence; without a video mapping it holds the queue at
  `sync_latency_ms`. Output is always at
  exactly the grain cadence (e.g. 960 samples per grain at 50, 1601/1602
  cadence at 59.94). `audio_drift_ppm` is the net of dropped (+) and inserted
  (−) samples per sample written after the first alignment.
- Lip sync: video and audio share the same mapped timeline; a per-channel
  `audio_offset_ms` (±) corrects source offsets.
- Loss of signal: hold the last frame for `hold_ms` (default 500), then black or
  slate (configurable, slate shows the channel label and "NO SIGNAL"); audio goes
  to silence. **The MXL flows keep running** at TAI; NMOS state does not change.
- Source format change during operation: the adapter reconfigures; the MXL flow
  stays the same (target format unchanged).

### 5.5 Multichannel audio (v1 requirement)

- Input: **all** audio PIDs of the selected program are decoded, each with its full
  channel layout: stereo, 5.1, 7.1 and multichannel AAC, AC-3/E-AC-3, SMPTE 302M
  (2/4/6/8 channels per PID; several 302M PIDs together for 16 channels), Opus.
  The UI lists every decoded track with codec, layout, language and live meters.
- Output: one or more `audio/float32` flows per channel (default one flow, 16
  channels, configurable 2–64).
- Routing matrix from decoded tracks/channels to output channels (UI matrix like
  mxl-decklink), with per-output gain (dB) and mute; unused outputs are silent.
- Downmix/upmix presets (5.1 → stereo, mono → stereo dual).
- Presets for typical layouts: "16 ch from 8 stereo PIDs", "5.1 + stereo",
  "302M 16 ch" (two 8-channel PIDs).
- A track that disappears from the stream leaves its mapped outputs silent and
  raises an alarm; it is picked up again automatically when it returns.

### 5.7 Access control for internet-facing listeners

SRT sources come from internal WAN sites **and** from the internet through the
corporate firewall. Therefore:

- Each channel has a `exposure` setting: `internal` (default) or `internet`.
- `internet` channels MUST use encryption (passphrase, AES-256 by default) and,
  in listener mode, a `streamid` allow-list; connections without matching
  passphrase/streamid are rejected and logged with peer IP and reason.
- Optional allow-list of peer IP ranges per channel.
- Connection attempts are rate-limited per peer; repeated failures are counted
  (metric) and logged.
- Passphrases are stored only in the config file (or a Kubernetes Secret), never
  returned by the API, and masked in logs.
- Recommendation documented in the README: prefer **caller** mode towards a
  remote listener when the far end allows it (only outbound UDP needed); use
  listener mode on the internet only with the firewall/NAT rule described in §10.

### 5.6 Ancillary

- Timecode: optional; if the stream carries SEI/GOP timecode it is shown in status
  only (no ANC output in v1).

---

## 6. Egress (MXL → SRT)

### 6.1 MXL input

- Video and audio receivers per §4; readers read the grain `read_offset_grains`
  (default 2) before the output grain's time, or the newest one when the source
  runs later (more on mirror domains), aligned via the MXL synchronisation group
  for video and audio. An interlaced flow holds one field per grain at twice the
  declared rate; the offset counts frames.
- Missing flow → `waiting` with slate/silence encoded (the SRT output keeps
  running so the far end does not reconnect); no grains → `no_signal`, same
  behaviour.

### 6.2 Format and encode

- Output format per channel: raster, scan, rate. Same adaptation matrix as §5.3,
  applied before encoding (e.g. MXL 1080i50 → 1080p50 for an H.264 progressive
  stream, or 2160p50 → 1080p50 for contribution).
- Video codecs: H.264 (default), HEVC. `ENCODER=auto|nvenc|cpu`. Settings: CBR
  bitrate (default 15 Mbit/s 1080p50), GOP length (default 1 s), B-frames
  (default 0 for low latency), profile/level, NVENC preset and tuning
  (`ll`/`ull`), x264 preset, `zerolatency` and thread count. Interlaced encoding only where the
  encoder supports it (detect at start; otherwise refuse that configuration with
  a clear error).
- Audio (multichannel, v1 requirement): up to **16 tracks**, each its own PID with
  a channel selection from the MXL audio flow and a layout: mono, stereo, 5.1, 7.1.
  Codecs: AAC-LC (default; 192 kbit/s per stereo pair, scaled for 5.1/7.1),
  MP2 (mono/stereo), AC-3 only if the encoder is available in the build,
  SMPTE 302M (PCM, transparent, 2/4/6/8 channels per PID — e.g. 16 channels as
  two 8-channel PIDs), Opus (only if the far end supports it in TS).
- Per track: language code (ISO 639) and descriptor in the PMT, gain, mute.
- Presets mirror §5.5 ("8 × stereo AAC", "16 ch 302M", "5.1 + stereo").

### 6.3 TS mux and SRT output

- MPEG-TS with configurable service name/provider, program number, PIDs, PCR
  interval (default 40 ms), mux mode CBR (with null padding, default) or VBR.
- SRT modes caller (default) and listener (§5.0); same options as §5.1. Optional second
  destination (duplicate output to a backup receiver).
- Reconnect handling: caller retries with backoff; the encoder keeps running so a
  reconnect starts at the next IDR within one GOP.

---

## 7. Web UI and API

- **Dashboard:** per channel: direction, state (`idle`, `connecting`, `waiting`,
  `no_signal`, `running`, `error`), SRT peer, SRT statistics (RTT, packet loss,
  retransmits, dropped, buffer, bitrate), detected source format, target format,
  decoder/encoder in use, frame-sync repeats/drops, audio drift, audio meters
  (WebSocket), JPEG thumbnail of the current frame (low rate).
- **Channels:** create/edit/delete ingest and egress channels with all settings
  above; changes apply per channel at runtime (only that channel restarts).
- **Audio:** the matrix (ingest) or the tracks (egress) per channel.
- **NMOS & MXL:** node info, registration, senders/receivers with active
  parameters, the MXL domains and flows on the host (egress source picker).
- **Status:** health, versions, process and GPU, per-channel counters.
- **Settings:** effective configuration with origins, import/export, `KEY=value` export.
- REST under `/api/v1/…` (channels CRUD, status, statistics, config, info,
  domains), WebSocket `/api/v1/events`; `/livez`, `/readyz`, `/statusz`,
  `/metrics` on `WEB_PORT`.
- Unauthenticated by design (lab network), like the siblings; SRT passphrases are
  write-only in the UI and never returned by the API.

---

## 8. Configuration

Environment over JSON config file (`SRTGW_CONFIG_FILE`) over defaults; invalid
configuration exits 78; global changes flagged `restart_required`.

| Key | Default | Meaning |
| --- | --- | --- |
| `HOST_ID` | hostname | default `NMOS_SEED` prefix. Not used as an announced address |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | parent of domain directories, mirrors included |
| `MXL_OUTPUT_DOMAIN_DIR` | `/Volumes/mxl/srtgw-<seed-short>` | own output domain, created if missing |
| `MXL_OUTPUT_DOMAIN_ID` | UUIDv5 from the seed | stable domain id. A different id already in `domain_def.json` is not overwritten |
| `MXL_HISTORY_DURATION_MS` | `1000` | history duration when this process creates `options.json`. `SRTGW_HISTORY_DURATION_NS` is the previous name |
| `MXL_CLEANUP_ON_EXIT` | `false` | SIGTERM deletes only this output domain directory |
| `STATE_DIR` | `/config` | config persistence and IS-05 `routes.json` |
| `SHUTDOWN_TIMEOUT_S` | `10` | SIGTERM budget. Exit code is 143 |
| `DECODER` / `ENCODER` | `auto` | `auto`, `nvdec`/`nvenc`, `cpu` |
| `SRT_PORT_RANGE` | `9000-9099` | allowed listener ports |
| `NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT` | empty / 3210 | Registration API |
| `NMOS_QUERY_ADDRESS` / `NMOS_QUERY_PORT` | registry address / registry port + 1 | Query API. `/readyz` requires the node to be listed there when a registry is set |
| `NMOS_DNS_SD` | false | off disables browse and mDNS advertisement |
| `NMOS_PORT` | 3272 | Node API. WebSocket is this port + 1 |
| `NMOS_SEED` | `HOST_ID-srtgw` | UUIDv5 for node, device, flows, senders, receivers, domain id |
| `NMOS_LABEL` | unset | node and device label. Unset keeps the previous labels |
| `NMOS_TAGS` | empty | JSON object of tag to string array, on the node and device |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | announced address. `SRTGW_PUBLIC_IP` is the same setting |
| `WEB_PORT` | 8120 | UI, REST, health, metrics |
| `LOG_LEVEL` | `info` | JSON logs |

Defaults do not collide with the sibling defaults under host networking.

---

## 9. Metrics (prefix `mxl_srt_gateway_`)

Per channel: `channel_state`, `srt_rtt_ms`, `srt_pkt_loss_total`,
`srt_pkt_retrans_total`, `srt_pkt_drop_total`, `srt_bitrate_bps`,
`srt_buffer_ms`, `srt_connected`, `video_frames_out_total`, `framesync_repeats_total`,
`framesync_drops_total`, `audio_drift_ppm`, `audio_fifo_ms`, `decode_errors_total`,
`decode_fps`, `encode_fps`, `encode_latency_seconds` (histogram), `codec_info`
(info gauge with decoder/encoder, source and target format), `failover_active`,
plus process and GPU (`nvenc_sessions`, `nvdec_sessions`, GPU memory).
Grafana dashboard in `deploy/grafana/`.

---

## 10. Deployment

- **Pod network by default** (platform rule): MXL root `hostPath`, uid 1000,
  optional `nvidia.com/gpu` with `runtimeClassName: nvidia`.
- SRT listener ports: exposed per channel as UDP `hostPort` (or a NodePort range)
  on the node's management/WAN-facing IP; the UI shows the address and port a
  remote caller must use (`SRTGW_PUBLIC_IP` from `status.hostIP` via downward API).
  Caller mode needs only outbound UDP. Document the firewall requirement: SRT is
  UDP and does not pass the corporate HTTP proxy.
- Internet exposure: the README and `docs/network.md` describe what the network
  team must provide per `internet` listener channel — a NAT/firewall rule from a
  public address and UDP port to the node IP and listener port, limited to known
  peer ranges where possible — and for `internet` caller channels the outbound UDP
  rule to the remote listener. The gateway exposes, per channel, the exact
  public/internal address and port as a ready-to-copy request in the UI.
- Internet and internal channels can run in the same container; for stricter
  separation, run a second instance on a dedicated node or interface (documented).
- Compose demo: nmos-cpp registry; the gateway with one egress channel reading a
  test flow (mxl-test-player or mxl-decklink mock) and sending SRT to a second
  instance's ingest listener on the same host (loopback), whose MXL output is
  routed to mxl-webrtc-monitor. Host-network Compose file for real hosts.
- Kubernetes Deployment (and GPU variant) that `mxl-poc-platform` can vendor;
  ServiceMonitor; ConfigMap; PVC or hostPath for `/config`.
- CI as siblings: build, unit and integration tests (CPU paths), GHCR image
  `ghcr.io/leeo86/mxl-srt-gateway`, tags `vX.Y.Z`/`X.Y`/`X`/`latest`,
  `nightly-dev`, `git-<sha>`; label `io.dmf.mxl.revision`.
- Exit codes: 0, 75, 78, 143 as siblings.

---

## 11. Testing

- Unit: adaptation decision table (§5.3) for every source/target pair; frame
  synchroniser repeat/drop decisions with synthetic clocks (slow, fast, jittery
  sources); audio cadence at 59.94; channel mapping and gain; config precedence;
  ID derivation.
- Integration (CPU, CI):
  1. FFmpeg CLI generates a 1080i50 H.264 + AAC TS with test pattern and sends it
     over SRT to an ingest channel with target 1080p50; check the MXL flow is
     1080p50, frame count matches TAI over 60 s, field-order test pattern
     deinterlaces correctly (pixel checks), audio cadence exact.
  2. Source at 59.94 into a 50p target: repeats/drops within expected counts, no
     MXL gaps.
  3. Kill the SRT source: MXL output continues with hold → slate; reconnect resumes.
  4. Egress from an MXL test flow to SRT, received by FFmpeg/`srt-live-transmit`;
     verify codec, resolution, bitrate, PCR interval.
  5. Multichannel: a TS with 8 stereo AAC PIDs and one with two 8-channel 302M
     PIDs map to a 16-channel MXL flow with correct channel order (per-channel
     tone frequencies); egress back to 8 × stereo AAC and 16 ch 302M, verified
     with FFmpeg.
  6. Internet exposure: an `internet` listener rejects a caller without passphrase
     or with a wrong `streamid` and logs the reason.
  7. NMOS: IS-05 activation of an egress receiver to a flow that does not exist
     yet → `waiting` → running when the flow appears.
- NMOS conformance: AMWA IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01 (script or CI).
- Hardware (documented): NVDEC/NVENC on A4000 and L4; number of concurrent
  1080p50 ingest and egress channels per GPU; end-to-end latency
  (SRT latency + `sync_latency_ms` + codec) measured with the A/V sync pattern of
  mxl-test-player.

---

## 12. Implementation order

1. Skeleton, config, ops endpoints, metrics, CI, Dockerfile with FFmpeg/libsrt.
2. Ingest with CPU decode, progressive only, frame synchroniser, MXL writer.
3. Adaptation matrix on CPU (deinterlace, interlace, scale, colour).
4. NMOS senders; output domain.
5. Egress with CPU encode; NMOS receivers.
6. NVDEC/NVENC and CUDA filters; fallback logic.
7. Backup input/output, audio matrix UI, dashboard, Compose/Kubernetes, docs.

---

## 13. Candidate features (decide before or during implementation)

Not in v1 unless confirmed:

Decided for v1: format conversion (§5.3, §6.2) and multichannel audio (§5.5,
§6.2). Not selected for v1, kept for later:

- SRT socket groups (main/backup bonding in "broadcast" mode).
- SCTE-35 pass-through between TS and MXL data flows.
- Subtitles/teletext (DVB subtitles, teletext pass-through).
- NMOS control of the SRT side (IS-05 with the SRT transport, if defined in the
  AMWA parameter registers for the pinned nmos-cpp).
- HDR (HLG/PQ, BT.2020) passing through to MXL.
- Timecode as ANC (`video/smpte291`) flow from stream timecode.
- Motion-compensated frame-rate conversion.

## 14. Open points

- Which public addresses/ports the network team can provide for internet-facing
  listeners, and whether a dedicated node/interface is required for them.
- Typical channel counts per host for sizing.
