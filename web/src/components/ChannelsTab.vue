<script setup>
// Channels (SPECIFICATION.md §5, §6, §7): every setting of the selected channel as a draft (Apply,
// Revert), a new ingest or egress channel, enable/disable and delete. Next to it the channel's live
// status, the address a remote party needs, and its MXL side (output flows, or the egress source).
import { computed, ref, watch } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import IdCode from "./IdCode.vue";
import Pill from "./Pill.vue";
import RoutePanel from "./RoutePanel.vue";
import Segmented from "./Segmented.vue";
import SrtEndpoint from "./SrtEndpoint.vue";
import { STATE_TEXT, copyText, fmtBitrate, num, stateKind } from "../api.js";
import {
  NEW,
  applyDraft,
  createChannel,
  creating,
  deleteChannel,
  discardNew,
  drafts,
  hasConfigFile,
  isDirty,
  live,
  revertDraft,
  routeEdits,
  selectedChannel,
  setEnabled,
  startNew,
  statusById,
  toForm,
  view,
} from "../store.js";

const isNew = computed(() => live.selected === NEW && Boolean(creating.draft));
const c = computed(() => (isNew.value ? null : selectedChannel.value));
const d = computed(() => (isNew.value ? creating.draft.value : c.value && drafts[c.value.id]?.value));
const orig = computed(() => (c.value ? toForm(c.value) : null));
const dirty = computed(() => isNew.value || (c.value && isDirty(c.value.id)));
const egress = computed(() => d.value?.direction === "egress");
const s = computed(() => (c.value ? statusById.value[c.value.id] : null));
const changed = (path) => {
  if (!orig.value || !d.value) return false;
  const get = (o) => path.split(".").reduce((x, k) => x?.[k], o);
  return get(d.value) !== get(orig.value);
};
const cls = (path) => ({ dirty: changed(path) });

// ---- choices -----------------------------------------------------------------
const RASTERS = [
  { value: "1920x1080", label: "1920×1080" },
  { value: "1280x720", label: "1280×720" },
  { value: "3840x2160", label: "3840×2160" },
];
const RATES = ["23.98", "24", "25", "29.97", "30", "50", "59.94", "60"];
const SCAN = [
  { value: "progressive", label: "Progressive" },
  { value: "interlaced", label: "Interlaced", title: "1920×1080 at 25 or 29.97 frames (i50, i59.94)" },
];
const FIELD = [
  { value: "tff", label: "Top first" },
  { value: "bff", label: "Bottom first" },
];
const SOURCE_SCAN = [
  { value: "auto", label: "As flagged" },
  { value: "progressive", label: "Progressive" },
  { value: "tff", label: "TFF" },
  { value: "bff", label: "BFF" },
];
const ASPECT = [
  { value: "letterbox", label: "Letter/pillarbox" },
  { value: "fill", label: "Fill" },
];
const SCALE = [
  { value: "bicubic", label: "Bicubic" },
  { value: "lanczos", label: "Lanczos" },
];
const DEINT = [
  { value: "bwdif", label: "bwdif" },
  { value: "yadif", label: "yadif" },
  { value: "weave", label: "Weave (none)" },
];
const LOSS = [
  { value: "slate", label: "Slate", title: "Channel label and NO SIGNAL" },
  { value: "black", label: "Black" },
];
const PROGRAM = [
  { value: "first", label: "First" },
  { value: "number", label: "By number" },
  { value: "name", label: "By service name" },
];
const CODEC = [
  { value: "h264", label: "H.264" },
  { value: "hevc", label: "HEVC" },
];
const MUX = [
  { value: "cbr", label: "CBR (null padding)" },
  { value: "vbr", label: "VBR" },
];
const FORCE = [
  { value: "auto", label: "Automatic" },
  { value: "main", label: "Main" },
  { value: "backup", label: "Backup" },
];
const X264_PRESETS = ["ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow"];
const X264_TUNES = ["zerolatency", "film", "animation", "grain", "stillimage", "fastdecode"];
const NVENC_PRESETS = ["p1", "p2", "p3", "p4", "p5", "p6", "p7"];
const NVENC_TUNES = [
  { value: "ull", label: "Ultra low latency" },
  { value: "ll", label: "Low latency" },
  { value: "hq", label: "High quality" },
];
const codecChoice = (key, global) => [
  { value: "", label: `Global (${global || "auto"})` },
  { value: "auto", label: "Auto" },
  { value: key === "decoder" ? "nvdec" : "nvenc", label: key === "decoder" ? "NVDEC" : "NVENC" },
  { value: "cpu", label: "CPU" },
];

const raster = computed({
  get: () => `${d.value.target.width}x${d.value.target.height}`,
  set: (v) => {
    const [w, h] = v.split("x").map(Number);
    d.value.target.width = w;
    d.value.target.height = h;
  },
});
const scan = computed({
  get: () => d.value.target.scan,
  set: (v) => {
    d.value.target.scan = v;
    if (v === "interlaced") {
      if (d.value.target.field_order !== "bff") d.value.target.field_order = "tff";
      if (!["25", "29.97"].includes(d.value.target.rate)) d.value.target.rate = "25";
    } else {
      d.value.target.field_order = "progressive";
    }
  },
});
const rates = computed(() => (d.value.target.scan === "interlaced" ? ["25", "29.97"] : RATES));
const rateLabel = (r) => (d.value.target.scan === "interlaced" ? `${r} frames (i${r === "25" ? "50" : "59.94"})` : r);
const bitrateKbps = computed({
  get: () => Math.round((d.value.egress.bitrate || 0) / 1000),
  set: (v) => (d.value.egress.bitrate = Math.round(Number(v) * 1000)),
});

// ---- checks before Apply (the API checks again) --------------------------------------
const portRange = computed(() => String(live.config?.SRT_PORT_RANGE || "9000-9099"));
function endpointProblems(ep, label) {
  const out = [];
  const [lo, hi] = portRange.value.split("-").map(Number);
  if (ep.mode !== "caller" && ep.local_port && (ep.local_port < lo || ep.local_port > hi)) out.push(`${label}: the local port must be 0 or in ${portRange.value}.`);
  if (ep.mode !== "listener" && (!ep.remote_host || !(ep.remote_port > 0))) out.push(`${label}: a caller needs the remote host and port.`);
  if (ep.passphrase && (ep.passphrase.length < 10 || ep.passphrase.length > 79)) out.push(`${label}: the passphrase must have 10 to 79 characters.`);
  if (!(ep.payload_size >= 100 && ep.payload_size <= 1456)) out.push(`${label}: the payload size must be 100–1456 bytes.`);
  return out;
}
const problems = computed(() => {
  const x = d.value;
  if (!x) return [];
  const out = [];
  if (isNew.value && x.id) {
    if (!/^[A-Za-z0-9._-]{1,64}$/.test(x.id)) out.push("The ID may only have letters, digits, dot, dash and underscore.");
    if (live.channels.some((ch) => ch.id === x.id)) out.push(`The ID ${x.id} is taken.`);
  }
  out.push(...endpointProblems(x.srt, "SRT"));
  const hasKey = x.srt.passphrase || (x.srt.passphrase_set && !x.srt.clear_passphrase);
  if (x.srt.exposure === "internet" && !hasKey) out.push("Internet exposure needs a passphrase.");
  if (x.srt.exposure === "internet" && x.srt.mode === "listener" && !String(x.srt.accepted_streamids).trim()) out.push("An internet listener needs accepted stream IDs.");
  if (x.backup.enabled) out.push(...endpointProblems(x.backup.endpoint, egress.value ? "Second destination" : "Backup input"));
  if (x.target.scan === "interlaced" && (x.target.width !== 1920 || !["25", "29.97"].includes(x.target.rate))) out.push("Interlaced is 1920×1080 at 25 or 29.97 frames only.");
  if (egress.value && x.egress.codec === "hevc" && x.target.scan === "interlaced") out.push("HEVC cannot be interlaced.");
  if (!(x.sync_latency_ms >= 0 && x.sync_latency_ms <= 2000)) out.push("The sync latency must be 0–2000 ms.");
  if (!(x.audio_offset_ms >= -1000 && x.audio_offset_ms <= 1000)) out.push("The audio offset must be −1000 to 1000 ms.");
  return out;
});

// ---- actions ----------------------------------------------------------------
const confirmDelete = ref(false);
watch(c, () => (confirmDelete.value = false));
async function apply() {
  if (isNew.value) await createChannel();
  else await applyDraft(c.value.id);
}
function revert() {
  if (isNew.value) discardNew();
  else revertDraft(c.value.id);
}
async function remove() {
  if (!confirmDelete.value) {
    confirmDelete.value = true;
    return;
  }
  confirmDelete.value = false;
  await deleteChannel(c.value.id);
}
async function copy(text) {
  live.notice = (await copyText(text)) ? "Copied." : "Copy failed; select the text and copy it by hand.";
}
const shown = ref(false);
const decoderGlobal = computed(() => live.config?.DECODER);
const encoderGlobal = computed(() => live.config?.ENCODER);
const anyDirty = (id) => isDirty(id) || Boolean(routeEdits[id]);
</script>

<template>
  <div class="toolbar">
    <ChannelPicker with-new :dirty="anyDirty" />
    <span class="spacer"></span>
    <button class="btn secondary" @click="startNew('ingest')">New ingest</button>
    <button class="btn secondary" @click="startNew('egress')">New egress</button>
  </div>
  <div v-if="!d" class="panel empty">No channel yet. Create an ingest (SRT → MXL) or an egress (MXL → SRT) channel.</div>
  <div v-else class="editor">
    <div class="main">
      <div class="panel">
        <h3>
          {{ isNew ? `New ${d.direction} channel` : `${d.direction} · ${c.id}` }}
          <span class="spacer"></span>
          <span v-if="dirty" class="pill warn">{{ isNew ? "not created" : "not applied" }}</span>
        </h3>
        <div class="fields grid">
          <div>
            <label for="ch-id">ID (in URLs and NMOS ids){{ isNew ? "; empty: next free chN" : "" }}</label>
            <input id="ch-id" v-model.trim="d.id" :disabled="!isNew" placeholder="ch1" />
          </div>
          <div>
            <label for="ch-label">Label (NMOS, slate, this page)</label>
            <input id="ch-label" v-model="d.label" :class="cls('label')" />
          </div>
          <div style="align-self: end">
            <label class="check"><input v-model="d.enabled" type="checkbox" /> Enabled</label>
          </div>
        </div>
      </div>

      <div class="panel">
        <h3>SRT {{ egress ? "output" : "input" }}</h3>
        <SrtEndpoint :ep="d.srt" :orig="orig?.srt" :direction="d.direction" idp="srt" :port-range="portRange" />
      </div>

      <div class="panel">
        <h3>
          {{ egress ? "Second destination" : "Backup input" }}
          <span class="spacer"></span>
          <label class="check"><input v-model="d.backup.enabled" type="checkbox" /> On</label>
        </h3>
        <p class="note" style="margin-top: 0">
          {{
            egress
              ? "The same stream goes to a second SRT destination as well (backup receiver)."
              : "A second SRT source. The channel switches to it when the main input is lost for the failover time, and back after the main input was stable for the failback time."
          }}
        </p>
        <template v-if="d.backup.enabled">
          <div v-if="!egress" class="fields grid" style="margin-bottom: 0.6rem">
            <div>
              <label for="bk-fo">Failover after ms</label>
              <input id="bk-fo" v-model.number="d.backup.failover_ms" type="number" min="100" step="100" :class="cls('backup.failover_ms')" />
            </div>
            <div>
              <label for="bk-fb">Failback after ms</label>
              <input id="bk-fb" v-model.number="d.backup.failback_ms" type="number" min="100" step="100" :class="cls('backup.failback_ms')" />
            </div>
            <div style="grid-column: span 2">
              <label>On air</label>
              <Segmented v-model="d.backup.force" :options="FORCE" label="Input on air" />
            </div>
          </div>
          <SrtEndpoint :ep="d.backup.endpoint" :orig="orig?.backup?.endpoint" :direction="d.direction" idp="bk" :port-range="portRange" />
        </template>
      </div>

      <div class="panel">
        <h3>{{ egress ? "Video encoding" : "Demux and decode" }}</h3>
        <template v-if="egress">
          <div class="choices">
            <div>
              <label>Encoder</label>
              <Segmented v-model="d.encoder" :options="codecChoice('encoder', encoderGlobal)" label="Encoder" />
            </div>
            <div>
              <label>Codec</label>
              <Segmented v-model="d.egress.codec" :options="CODEC" label="Codec" />
            </div>
          </div>
          <div class="fields grid">
            <div>
              <label for="eg-br">Bitrate in kbit/s (CBR)</label>
              <input id="eg-br" v-model.number="bitrateKbps" type="number" min="500" step="500" :class="cls('egress.bitrate')" />
            </div>
            <div>
              <label for="eg-gop">GOP in seconds</label>
              <input id="eg-gop" v-model.number="d.egress.gop_seconds" type="number" min="0.1" step="0.1" :class="cls('egress.gop_seconds')" />
            </div>
            <div>
              <label for="eg-bf">B-frames</label>
              <input id="eg-bf" v-model.number="d.egress.bframes" type="number" min="0" max="4" :class="cls('egress.bframes')" />
            </div>
          </div>
          <div class="group-caption">CPU (libx264)</div>
          <div class="fields grid">
            <div>
              <label for="eg-preset">Preset</label>
              <select id="eg-preset" v-model="d.egress.preset" :class="cls('egress.preset')">
                <option v-for="p in X264_PRESETS" :key="p">{{ p }}</option>
              </select>
            </div>
            <div>
              <label for="eg-tune">Tune</label>
              <select id="eg-tune" v-model="d.egress.tune" :class="cls('egress.tune')">
                <option v-for="t in X264_TUNES" :key="t">{{ t }}</option>
              </select>
            </div>
            <div>
              <label for="eg-thr">Threads (0: x264's choice)</label>
              <input id="eg-thr" v-model.number="d.egress.threads" type="number" min="0" max="64" :class="cls('egress.threads')" />
            </div>
          </div>
          <div class="group-caption">GPU (NVENC)</div>
          <div class="fields grid">
            <div>
              <label for="eg-npreset">Preset (p1 fastest, p7 best)</label>
              <select id="eg-npreset" v-model="d.egress.nvenc_preset" :class="cls('egress.nvenc_preset')">
                <option v-for="p in NVENC_PRESETS" :key="p">{{ p }}</option>
              </select>
            </div>
            <div style="grid-column: span 2">
              <label>Tuning</label>
              <Segmented v-model="d.egress.nvenc_tune" :options="NVENC_TUNES" label="NVENC tuning" />
            </div>
          </div>
          <p class="note">HEVC on the CPU (libx265) always runs ultrafast with zerolatency.</p>
        </template>
        <template v-else>
          <div class="choices">
            <div>
              <label>Decoder</label>
              <Segmented v-model="d.decoder" :options="codecChoice('decoder', decoderGlobal)" label="Decoder" />
            </div>
            <div>
              <label>Program</label>
              <Segmented v-model="d.program.select" :options="PROGRAM" label="Program" />
            </div>
            <div v-if="d.program.select === 'number'">
              <label for="pg-num">Program number</label>
              <input id="pg-num" v-model.number="d.program.number" type="number" min="1" style="width: 8rem" :class="cls('program.number')" />
            </div>
            <div v-if="d.program.select === 'name'">
              <label for="pg-name">Service name</label>
              <input id="pg-name" v-model="d.program.service_name" style="width: 14rem" :class="cls('program.service_name')" />
            </div>
          </div>
          <div class="fields grid">
            <div>
              <label for="pg-vpid">Video PID (0: the first)</label>
              <input id="pg-vpid" v-model.number="d.program.video_pid" type="number" min="0" max="8190" :class="cls('program.video_pid')" />
            </div>
            <div style="grid-column: span 2">
              <label for="pg-apids">Audio PIDs (empty: all of the program)</label>
              <input id="pg-apids" v-model="d.program.audio_pids" placeholder="257, 258" :class="cls('program.audio_pids')" />
            </div>
          </div>
          <p class="note">Decoded audio is routed to the MXL channels on the Audio tab.</p>
        </template>
      </div>

      <div class="panel">
        <h3>{{ egress ? "Output format" : "MXL output format" }}</h3>
        <div class="choices">
          <div>
            <label>Raster</label>
            <Segmented v-model="raster" :options="RASTERS" label="Raster" />
          </div>
          <div>
            <label>Scan</label>
            <Segmented v-model="scan" :options="SCAN" label="Scan" />
          </div>
          <div>
            <label for="tg-rate">Rate</label>
            <select id="tg-rate" v-model="d.target.rate" :class="cls('target.rate')">
              <option v-for="r in rates" :key="r" :value="r">{{ rateLabel(r) }}</option>
            </select>
          </div>
          <div v-if="d.target.scan === 'interlaced'">
            <label>Field order</label>
            <Segmented v-model="d.target.field_order" :options="FIELD" label="Field order" />
          </div>
        </div>
        <div class="group-caption">Adaptation (when the {{ egress ? "MXL flow" : "stream" }} differs)</div>
        <div class="choices">
          <div>
            <label>Aspect</label>
            <Segmented v-model="d.aspect" :options="ASPECT" label="Aspect" />
          </div>
          <div>
            <label>Scaler</label>
            <Segmented v-model="d.scale" :options="SCALE" label="Scaler" />
          </div>
          <div>
            <label>Deinterlacer</label>
            <Segmented v-model="d.deinterlacer" :options="DEINT" label="Deinterlacer" />
          </div>
          <div v-if="!egress">
            <label>Source scan</label>
            <Segmented v-model="d.source_scan" :options="SOURCE_SCAN" label="Source scan" />
          </div>
        </div>
        <p class="note">
          {{
            egress
              ? "The MXL picture is converted to this format before encoding."
              : "The MXL flow always has this format, whatever arrives. A new format makes a new MXL flow id (IS-04 updates the sender). Source scan corrects a mis-flagged stream."
          }}
        </p>
      </div>

      <div class="panel">
        <h3>{{ egress ? "MPEG-TS and MXL input" : "Frame sync and signal loss" }}</h3>
        <div v-if="egress" class="fields grid">
          <div>
            <label for="ts-svc">Service name</label>
            <input id="ts-svc" v-model="d.egress.service_name" :class="cls('egress.service_name')" />
          </div>
          <div>
            <label for="ts-prov">Provider</label>
            <input id="ts-prov" v-model="d.egress.provider" :class="cls('egress.provider')" />
          </div>
          <div>
            <label for="ts-prog">Program number</label>
            <input id="ts-prog" v-model.number="d.egress.program_number" type="number" min="1" max="65535" :class="cls('egress.program_number')" />
          </div>
          <div>
            <label for="ts-pmt">PMT PID</label>
            <input id="ts-pmt" v-model.number="d.egress.pmt_pid" type="number" min="16" max="8190" :class="cls('egress.pmt_pid')" />
          </div>
          <div>
            <label for="ts-vpid">Video PID (audio: the next ones)</label>
            <input id="ts-vpid" v-model.number="d.egress.video_pid" type="number" min="16" max="8190" :class="cls('egress.video_pid')" />
          </div>
          <div>
            <label for="ts-pcr">PCR interval in ms</label>
            <input id="ts-pcr" v-model.number="d.egress.pcr_ms" type="number" min="10" max="100" :class="cls('egress.pcr_ms')" />
          </div>
          <div style="grid-column: span 2">
            <label>Mux</label>
            <Segmented v-model="d.egress.mux" :options="MUX" label="Mux" />
          </div>
          <div>
            <label for="ts-off">MXL read offset in grains</label>
            <input id="ts-off" v-model.number="d.egress.read_offset_grains" type="number" min="0" max="50" :class="cls('egress.read_offset_grains')" />
          </div>
        </div>
        <div v-else class="fields grid">
          <div>
            <label for="fs-lat">Sync latency in ms</label>
            <input id="fs-lat" v-model.number="d.sync_latency_ms" type="number" min="0" max="2000" step="10" :class="cls('sync_latency_ms')" />
          </div>
          <div>
            <label for="fs-hold">Hold the last frame for ms</label>
            <input id="fs-hold" v-model.number="d.hold_ms" type="number" min="0" max="10000" step="50" :class="cls('hold_ms')" />
          </div>
          <div>
            <label>Then show</label>
            <Segmented v-model="d.loss_mode" :options="LOSS" label="Signal loss" />
          </div>
          <div>
            <label for="fs-aoff">Audio offset in ms (+: later)</label>
            <input id="fs-aoff" v-model.number="d.audio_offset_ms" type="number" min="-1000" max="1000" step="1" :class="cls('audio_offset_ms')" />
          </div>
        </div>
        <p class="note">
          {{
            egress
              ? "The egress reads the grain this many grains before the output time (more on a fabrics mirror). Without a flow it sends a slate and silence, so the far end stays connected."
              : "The MXL flows keep running at TAI: the synchroniser repeats or drops frames for a source that is slow or fast, and holds, then shows the slate or black, when the source is lost."
          }}
        </p>
      </div>

      <div v-if="problems.length" class="errbox">
        <div v-for="p in problems" :key="p">{{ p }}</div>
      </div>
      <div class="actions">
        <span class="muted small">
          {{ hasConfigFile ? "Saved to the configuration file." : "SRTGW_CONFIG_FILE is not set: changes last until the gateway restarts." }}
          Only this channel restarts.
        </span>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!dirty" @click="revert">{{ isNew ? "Discard" : "Revert" }}</button>
        <button class="btn" :disabled="!dirty || problems.length > 0" @click="apply">{{ isNew ? "Create channel" : `Apply to ${c.id}` }}</button>
      </div>
    </div>

    <div v-if="c" class="side">
      <div class="panel">
        <h3>
          Live
          <span class="spacer"></span>
          <Pill v-if="!c.enabled" text="disabled" kind="neutral" />
          <Pill v-else :text="STATE_TEXT[s?.state] || s?.state || 'idle'" :kind="stateKind(s?.state)" />
        </h3>
        <div class="picture">
          <img :src="`/api/v1/channels/${encodeURIComponent(c.id)}/thumbnail?t=${view.tick}`" alt="" :style="{ opacity: shown ? 1 : 0 }" @load="shown = true" @error="shown = false" />
          <div v-if="!shown" class="none">no picture yet</div>
          <span v-if="s?.timecode" class="ov br mono">{{ s.timecode }}</span>
        </div>
        <p v-if="s?.error" class="alarm bad">{{ s.error }}</p>
        <p v-for="a in s?.alarms || []" :key="a" class="alarm">{{ a }}</p>
        <dl class="kv" style="margin-top: 0.6rem">
          <dt>SRT</dt>
          <dd>
            <Pill :text="s?.srt?.connected ? 'connected' : 'not connected'" :kind="s?.srt?.connected ? 'ok' : 'neutral'" />
            <span v-if="s?.failover_active" class="pill warn">{{ egress ? "copy connected" : "backup on air" }}</span>
          </dd>
          <dt>Peer</dt>
          <dd>{{ s?.peer || "–" }}<span v-if="s?.srt?.streamid" class="muted"> · stream ID {{ s.srt.streamid }}</span></dd>
          <dt>RTT</dt>
          <dd>{{ s?.srt?.connected ? `${num(s.srt.rtt_ms, 1)} ms` : "–" }}</dd>
          <dt>{{ egress ? "Send rate" : "Receive rate" }}</dt>
          <dd>{{ fmtBitrate(s?.srt?.bitrate_bps) }}</dd>
          <dt>Packets</dt>
          <dd>{{ s?.srt?.loss ?? 0 }} lost · {{ s?.srt?.retrans ?? 0 }} retransmitted · {{ s?.srt?.drop ?? 0 }} dropped</dd>
          <dt>SRT buffer</dt>
          <dd>{{ s?.srt?.connected ? `${num(s.srt.buffer_ms)} ms` : "–" }}</dd>
          <dt>Format</dt>
          <dd>{{ s?.source_format || "–" }} → {{ s?.target_format || "–" }}</dd>
          <dt>{{ egress ? "Encoder" : "Decoder" }}</dt>
          <dd>{{ (egress ? s?.encoder : s?.decoder) || "–" }}</dd>
          <template v-if="egress">
            <dt>Encode</dt>
            <dd>{{ num(s?.encode_fps, 1) }} fps · {{ s?.frames ?? 0 }} frames</dd>
          </template>
          <template v-else>
            <dt>Decode</dt>
            <dd>{{ num(s?.decode_fps, 1) }} fps · {{ s?.frames ?? 0 }} MXL frames</dd>
            <dt>Audio</dt>
            <dd>FIFO {{ num(s?.audio_fifo_ms) }} ms · drift {{ num(s?.audio_drift_ppm, 1) }} ppm</dd>
          </template>
          <dt>Repeat / drop</dt>
          <dd>{{ s?.repeats ?? 0 }} / {{ s?.drops ?? 0 }}</dd>
        </dl>
        <div v-if="s?.request" class="urlrow">
          <code :title="s.request">{{ s.request }}</code>
          <button class="btn small secondary" title="What the network team or the far end needs" @click="copy(s.request)">Copy</button>
        </div>
      </div>

      <RoutePanel v-if="egress" :id="c.id" />
      <div v-else class="panel">
        <h3>MXL output</h3>
        <dl class="kv">
          <dt>Domain</dt>
          <dd><IdCode :id="live.config?.MXL_OUTPUT_DOMAIN_ID" /> <code>{{ live.config?.MXL_OUTPUT_DOMAIN_DIR }}</code></dd>
          <dt>Video flow</dt>
          <dd><IdCode :id="s?.flows?.video" /></dd>
          <template v-for="(f, i) in s?.flows?.audio || []" :key="f">
            <dt>Audio flow {{ (s?.flows?.audio || []).length > 1 ? i + 1 : "" }}</dt>
            <dd><IdCode :id="f" /> <span class="muted small">{{ c.audio_outputs?.[i]?.channels }} ch</span></dd>
          </template>
        </dl>
        <p class="note">IS-04 senders for these flows are on the NMOS &amp; MXL tab.</p>
      </div>

      <div class="panel">
        <h3>Channel</h3>
        <div class="actions" style="margin-top: 0">
          <button class="btn secondary" @click="setEnabled(c.id, !c.enabled)">{{ c.enabled ? "Disable" : "Enable" }}</button>
          <span class="spacer"></span>
          <button class="btn danger" @click="remove">{{ confirmDelete ? `Really delete ${c.id}?` : "Delete" }}</button>
          <button v-if="confirmDelete" class="btn secondary" @click="confirmDelete = false">Keep</button>
        </div>
        <p class="note">A disabled channel keeps its settings and stops its SRT and MXL sides.</p>
      </div>
    </div>
  </div>
</template>
