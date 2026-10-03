<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";

const tab = ref("dashboard");
const channels = ref([]);
const live = ref({});
const nmos = ref({});
const config = ref({});
const error = ref("");
const editing = ref(null);
const matrixChannel = ref(null);
const matrixRoutes = ref([]);
const matrixOutCount = ref(16);
const matrixGains = ref([]);
const matrixMutes = ref([]);
const matrixTracks = ref([]);
const matrixMxlCount = ref(16);
const matrixSourceTracks = ref(8);
const matrixSourceChannels = ref(2);
const matrixError = ref("");
let socket;

const ingestPresets = [
  ["sequential", "Sequential"],
  ["16ch-stereo", "16 ch from 8 stereo"],
  ["5.1+stereo", "5.1 + stereo"],
  ["302m-16", "302M 16 ch"],
  ["5.1-stereo-downmix", "5.1 to stereo"],
  ["mono-dual", "Mono to dual mono"],
];
const egressPresets = [
  ["8x-stereo-aac", "8 × stereo AAC"],
  ["16ch-302m", "16 ch 302M"],
  ["5.1+stereo", "5.1 + stereo"],
  ["stereo", "Stereo"],
];

function layoutChannels(layout) {
  if (layout === "mono") return 1;
  if (layout === "5.1" || layout === "5.1(side)") return 6;
  if (layout === "7.1") return 8;
  return 2;
}

function defaultAudioBitrate(codec, layout) {
  const channels = layoutChannels(layout);
  if (codec === "mp2") return channels <= 1 ? 128000 : 256000;
  if (codec === "s302m" || codec === "302m") return 0;
  if (codec === "opus") return channels <= 2 ? 128000 : 256000;
  if (channels <= 1) return 96000;
  if (channels <= 2) return 192000;
  if (channels <= 6) return 384000;
  return 512000;
}

function emptyRoutes(count) {
  return Array.from({ length: count }, () => []);
}

function tap(track, channel, gain = 0) {
  return { track, channel, gain_db: gain, mute: false };
}

function presetIngestRoutes(name, channels) {
  const routes = emptyRoutes(channels);
  if (name === "5.1+stereo") {
    for (let i = 0; i < Math.min(6, channels); i++) routes[i].push(tap(0, i));
    if (channels > 6) routes[6].push(tap(1, 0));
    if (channels > 7) routes[7].push(tap(1, 1));
  } else if (name === "302m-16") {
    for (let i = 0; i < channels; i++) routes[i].push(tap(Math.floor(i / 8), i % 8));
  } else if (name === "5.1-stereo-downmix") {
    if (channels >= 1) routes[0] = [tap(0, 0), tap(0, 2, -3), tap(0, 4, -3)];
    if (channels >= 2) routes[1] = [tap(0, 1), tap(0, 2, -3), tap(0, 5, -3)];
  } else if (name === "mono-dual") {
    if (channels >= 1) routes[0].push(tap(0, 0));
    if (channels >= 2) routes[1].push(tap(0, 0));
  } else if (name === "16ch-stereo") {
    for (let i = 0; i < channels; i++) routes[i].push(tap(Math.floor(i / 2), i % 2));
  } else {
    for (let i = 0; i < channels; i++) routes[i].push(tap(0, i));
  }
  return routes;
}

function presetEgressTracks(name) {
  if (name === "16ch-302m") {
    return [0, 1].map((part) => ({
      codec: "s302m",
      layout: "7.1",
      channels: Array.from({ length: 8 }, (_, ch) => part * 8 + ch),
      bitrate: 0,
      language: "und",
      gain_db: 0,
      mute: false,
      pid: 0,
    }));
  }
  if (name === "5.1+stereo") {
    return [
      { codec: "aac", layout: "5.1", channels: [0, 1, 2, 3, 4, 5], bitrate: 384000, language: "und", gain_db: 0, mute: false, pid: 0 },
      { codec: "aac", layout: "stereo", channels: [6, 7], bitrate: 192000, language: "und", gain_db: 0, mute: false, pid: 0 },
    ];
  }
  const pairs = name === "8x-stereo-aac" ? 8 : 1;
  return Array.from({ length: pairs }, (_, i) => ({
    codec: "aac",
    layout: "stereo",
    channels: [i * 2, i * 2 + 1],
    bitrate: 192000,
    language: "und",
    gain_db: 0,
    mute: false,
    pid: 0,
  }));
}

function syncColumnMeta() {
  const gains = [];
  const mutes = [];
  for (let i = 0; i < matrixOutCount.value; i++) {
    const column = matrixRoutes.value[i] || [];
    gains.push(column.length ? Number(column[0].gain_db) || 0 : matrixGains.value[i] || 0);
    mutes.push(column.length ? column.every((item) => item.mute) : Boolean(matrixMutes.value[i]));
  }
  matrixGains.value = gains;
  matrixMutes.value = mutes;
}

function applyColumnMeta() {
  matrixRoutes.value.forEach((column, index) => {
    for (const item of column) {
      item.gain_db = Number(matrixGains.value[index]) || 0;
      item.mute = Boolean(matrixMutes.value[index]);
    }
  });
}

const matrixSourceRows = computed(() => {
  if (!matrixChannel.value) return [];
  const live = liveById.value[matrixChannel.value.id];
  const decoded = (live && live.tracks) || [];
  const rows = [];
  const tracksN = Math.max(1, Math.min(32, Number(matrixSourceTracks.value) || 1));
  const channelsN = Math.max(1, Math.min(16, Number(matrixSourceChannels.value) || 1));
  if (decoded.length) {
    decoded.forEach((track, index) => {
      const count = Number.parseInt(track.layout, 10) || 2;
      for (let channel = 0; channel < count; channel++) {
        rows.push({
          track: index,
          channel,
          label: `T${index + 1}.${channel + 1}`,
          detail: `${track.codec || "audio"} ${track.layout || ""} ${track.language || ""} pid ${track.pid || "—"}${track.missing ? " · missing" : ""}`,
        });
      }
    });
  } else {
    for (let track = 0; track < tracksN; track++) {
      for (let channel = 0; channel < channelsN; channel++) {
        rows.push({ track, channel, label: `T${track + 1}.${channel + 1}`, detail: "waiting for the stream" });
      }
    }
  }
  const seen = new Set(rows.map((row) => `${row.track}:${row.channel}`));
  matrixRoutes.value.forEach((column) => {
    for (const item of column) {
      const key = `${item.track}:${item.channel}`;
      if (!seen.has(key)) {
        seen.add(key);
        rows.push({ track: item.track, channel: item.channel, label: `T${item.track + 1}.${item.channel + 1}`, detail: "routed, not in the stream" });
      }
    }
  });
  return rows;
});

const matrixMxlRows = computed(() => {
  const count = Math.max(1, Math.min(64, Number(matrixMxlCount.value) || 1));
  return Array.from({ length: count }, (_, index) => index);
});

function routed(row, output) {
  return (matrixRoutes.value[output] || []).some((item) => item.track === row.track && item.channel === row.channel);
}

function toggleRoute(row, output) {
  const column = matrixRoutes.value[output] || (matrixRoutes.value[output] = []);
  const at = column.findIndex((item) => item.track === row.track && item.channel === row.channel);
  if (at >= 0) column.splice(at, 1);
  else column.push({ track: row.track, channel: row.channel, gain_db: Number(matrixGains.value[output]) || 0, mute: Boolean(matrixMutes.value[output]) });
}

function setOutCount(value) {
  const count = Math.max(2, Math.min(64, Number(value) || 2));
  matrixOutCount.value = count;
  while (matrixRoutes.value.length < count) matrixRoutes.value.push([]);
  matrixRoutes.value.splice(count);
  syncColumnMeta();
}

function applyIngestPreset(name) {
  matrixRoutes.value = presetIngestRoutes(name, matrixOutCount.value);
  matrixGains.value = [];
  matrixMutes.value = [];
  syncColumnMeta();
}

function egressSource(trackIndex, slot) {
  const track = matrixTracks.value[trackIndex];
  if (!track || !track.channels) return -1;
  const value = track.channels[slot];
  return value === undefined ? -1 : value;
}

function setEgressSource(trackIndex, slot, source) {
  const track = matrixTracks.value[trackIndex];
  if (!track) return;
  if (!Array.isArray(track.channels)) track.channels = [];
  while (track.channels.length <= slot) track.channels.push(-1);
  track.channels[slot] = track.channels[slot] === source ? -1 : source;
}

function setTrackLayout(track, layout) {
  const count = layoutChannels(layout);
  const next = (track.channels || []).slice(0, count);
  while (next.length < count) next.push(next.length ? next[next.length - 1] + 1 : 0);
  track.layout = layout;
  track.channels = next;
  track.bitrate = defaultAudioBitrate(track.codec, layout);
}

function setTrackCodec(track, codec) {
  track.codec = codec;
  track.bitrate = defaultAudioBitrate(codec, track.layout);
}

function addEgressTrack() {
  if (matrixTracks.value.length >= 16) return;
  const start = matrixTracks.value.reduce((max, track) => Math.max(max, ...((track.channels || []).map((channel) => channel + 1))), 0);
  matrixTracks.value.push({
    codec: "aac",
    layout: "stereo",
    channels: [start, start + 1],
    bitrate: 192000,
    language: "und",
    gain_db: 0,
    mute: false,
    pid: 0,
  });
  matrixMxlCount.value = Math.max(matrixMxlCount.value, start + 2);
}

function applyEgressPreset(name) {
  matrixTracks.value = presetEgressTracks(name);
  const highest = matrixTracks.value.reduce((max, track) => Math.max(max, ...((track.channels || []).map((channel) => channel + 1))), 0);
  matrixMxlCount.value = Math.max(16, highest);
}

function openMatrix(channel) {
  matrixError.value = "";
  matrixChannel.value = channel;
  const output = (channel.audio_outputs && channel.audio_outputs[0]) || { channels: 16, routes: [] };
  matrixOutCount.value = Math.max(2, Math.min(64, Number(output.channels) || 16));
  matrixRoutes.value = JSON.parse(JSON.stringify(output.routes || []));
  while (matrixRoutes.value.length < matrixOutCount.value) matrixRoutes.value.push([]);
  matrixRoutes.value.splice(matrixOutCount.value);
  matrixGains.value = [];
  matrixMutes.value = [];
  syncColumnMeta();
  const tracks = (channel.egress && channel.egress.audio_tracks) || [];
  matrixTracks.value = JSON.parse(JSON.stringify(tracks));
  let highest = 0;
  let sourceTracks = 1;
  let sourceChannels = 2;
  matrixRoutes.value.forEach((column) => {
    for (const item of column) {
      sourceTracks = Math.max(sourceTracks, item.track + 1);
      sourceChannels = Math.max(sourceChannels, item.channel + 1);
    }
  });
  matrixTracks.value.forEach((track) => {
    for (const channelIndex of track.channels || []) highest = Math.max(highest, channelIndex + 1);
  });
  matrixSourceTracks.value = Math.max(8, sourceTracks);
  matrixSourceChannels.value = Math.max(2, sourceChannels);
  matrixMxlCount.value = Math.max(16, highest);
}

const liveById = computed(() => {
  const map = {};
  for (const channel of live.value.channels || []) map[channel.id] = channel;
  return map;
});

async function loadChannels() {
  const response = await fetch("/api/v1/channels");
  const body = await response.json();
  channels.value = body.channels || [];
}

async function loadConfig() {
  const response = await fetch("/api/v1/config");
  config.value = await response.json();
}

async function loadNmos() {
  const response = await fetch("/api/v1/nmos");
  nmos.value = await response.json();
}

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${proto}://${location.host}/api/v1/events`);
  socket.onmessage = (event) => {
    try {
      live.value = JSON.parse(event.data);
    } catch {
      /* ignore malformed event */
    }
  };
  socket.onclose = () => setTimeout(connect, 1000);
}

function blank(direction) {
  return {
    id: "",
    label: direction === "egress" ? "Egress" : "Ingest",
    direction,
    enabled: true,
    srt: {
      mode: direction === "egress" ? "caller" : "listener",
      local_address: "",
      local_port: direction === "egress" ? 0 : 9000,
      remote_host: "",
      remote_port: 9000,
      latency_ms: 200,
      passphrase: "",
      pbkeylen: 0,
      streamid: "",
      accepted_streamids: [],
      exposure: "internal",
      payload_size: 1316,
      overhead: 25,
      peer_allow: [],
    },
    target: { width: 1920, height: 1080, scan: "progressive", rate: "50", field_order: "tff" },
    source_scan: "auto",
    deinterlacer: "bwdif",
    aspect: "letterbox",
    scale: "bicubic",
    sync_latency_ms: 120,
    hold_ms: 500,
    loss_mode: "slate",
    audio_offset_ms: 0,
    egress: { codec: "h264", bitrate: 15000000, gop_seconds: 1, bframes: 0, preset: "veryfast", tune: "zerolatency", mux: "cbr", pcr_ms: 40, service_name: "SRTGW", provider: "mxl-srt-gateway" },
  };
}

function edit(channel) {
  editing.value = JSON.parse(JSON.stringify(channel));
  if (!editing.value.srt) editing.value.srt = blank(editing.value.direction).srt;
  editing.value.srt.passphrase = "";
  editing.value.srt.accepted_streamids_text = (editing.value.srt.accepted_streamids || []).join(",");
  editing.value.srt.peer_allow_text = (editing.value.srt.peer_allow || []).join(",");
}

async function save() {
  error.value = "";
  const body = JSON.parse(JSON.stringify(editing.value));
  body.srt.accepted_streamids = (body.srt.accepted_streamids_text || "").split(",").map((s) => s.trim()).filter(Boolean);
  body.srt.peer_allow = (body.srt.peer_allow_text || "").split(",").map((s) => s.trim()).filter(Boolean);
  delete body.srt.accepted_streamids_text;
  delete body.srt.peer_allow_text;
  if (!body.srt.passphrase) delete body.srt.passphrase;
  const method = channels.value.some((channel) => channel.id === body.id) ? "PUT" : "POST";
  const path = method === "PUT" ? `/api/v1/channels/${body.id}` : "/api/v1/channels";
  const response = await fetch(path, { method, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  if (!response.ok) {
    const payload = await response.json().catch(() => ({}));
    error.value = payload.error || response.statusText;
    return;
  }
  editing.value = null;
  await loadChannels();
}

async function remove(id) {
  await fetch(`/api/v1/channels/${id}`, { method: "DELETE" });
  await loadChannels();
}

function commitColumns() {
  matrixRoutes.value.forEach((column, index) => {
    if (!column.length) return;
    const gain = Number(matrixGains.value[index]) || 0;
    if (gain !== (Number(column[0].gain_db) || 0)) {
      for (const item of column) item.gain_db = gain;
    }
    const mute = Boolean(matrixMutes.value[index]);
    if (mute !== Boolean(column[0].mute)) {
      for (const item of column) item.mute = mute;
    }
  });
}

async function saveMatrix() {
  matrixError.value = "";
  commitColumns();
  const body = {
    audio_outputs: [
      {
        channels: matrixOutCount.value,
        preset: "custom",
        routes: matrixRoutes.value.slice(0, matrixOutCount.value),
      },
    ],
    egress: { audio_tracks: matrixTracks.value },
  };
  const response = await fetch(`/api/v1/channels/${matrixChannel.value.id}/matrix`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!response.ok) {
    const payload = await response.json().catch(() => ({}));
    matrixError.value = payload.error || response.statusText;
    return;
  }
  matrixChannel.value = null;
  await loadChannels();
}

async function saveGlobals() {
  const body = {};
  for (const key of ["LOG_LEVEL", "DECODER", "ENCODER", "NMOS_HOST_ADDRESS", "SRTGW_PUBLIC_IP", "NMOS_REGISTRY_ADDRESS", "NMOS_LABEL", "WEB_PORT", "NMOS_PORT", "SRT_PORT_RANGE"]) {
    if (config.value[key] !== undefined) body[key] = String(config.value[key]);
  }
  const response = await fetch("/api/v1/config", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  config.value = await response.json();
}

async function download(kind) {
  const response = await fetch(kind === "env" ? "/api/v1/config/export?format=env" : "/api/v1/config/export");
  const text = await response.text();
  const blob = new Blob([text], { type: "text/plain" });
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = kind === "env" ? "srtgw.env" : "srtgw.json";
  link.click();
}

function meterHeight(value) {
  const db = value > 0 ? 20 * Math.log10(value) : -80;
  return `${Math.max(2, Math.min(48, ((db + 60) / 60) * 48))}px`;
}

onMounted(async () => {
  await Promise.all([loadChannels(), loadConfig(), loadNmos()]);
  connect();
});
onUnmounted(() => socket && socket.close());
</script>

<template>
  <div class="app">
    <header>
      <h1><span>MXL</span> SRT Gateway</h1>
      <nav>
        <button :class="{ active: tab === 'dashboard' }" @click="tab = 'dashboard'">Dashboard</button>
        <button :class="{ active: tab === 'channels' }" @click="tab = 'channels'">Channels</button>
        <button :class="{ active: tab === 'nmos' }" @click="tab = 'nmos'; loadNmos()">NMOS</button>
        <button :class="{ active: tab === 'settings' }" @click="tab = 'settings'">Settings</button>
      </nav>
    </header>
    <main>
      <section v-if="tab === 'dashboard'" class="grid">
        <article v-for="channel in channels" :key="channel.id" class="card">
          <div class="row">
            <h2>{{ channel.label || channel.id }}</h2>
            <span class="pill">{{ channel.direction }}</span>
            <span class="pill" :class="(liveById[channel.id] || {}).state || 'idle'">{{ (liveById[channel.id] || {}).state || "idle" }}</span>
          </div>
          <img class="thumb" :src="`/api/v1/channels/${channel.id}/thumbnail`" alt="" />
          <p class="request">{{ (liveById[channel.id] || {}).request }}</p>
          <div class="stats">
            <div><b>Peer</b>{{ (liveById[channel.id] || {}).peer || "—" }}</div>
            <div><b>Source</b>{{ (liveById[channel.id] || {}).source_format || "—" }}</div>
            <div><b>Target</b>{{ (liveById[channel.id] || {}).target_format || "—" }}</div>
            <div><b>RTT</b>{{ ((liveById[channel.id] || {}).srt || {}).rtt_ms || 0 }} ms</div>
            <div><b>Bitrate</b>{{ Math.round((((liveById[channel.id] || {}).srt || {}).bitrate_bps || 0) / 1000) }} kbps</div>
            <div><b>Loss</b>{{ ((liveById[channel.id] || {}).srt || {}).loss || 0 }}</div>
            <div><b>Codec</b>{{ (liveById[channel.id] || {}).decoder || (liveById[channel.id] || {}).encoder || "—" }}</div>
            <div><b>Repeat / drop</b>{{ (liveById[channel.id] || {}).repeats || 0 }} / {{ (liveById[channel.id] || {}).drops || 0 }}</div>
            <div><b>FIFO</b>{{ Math.round((liveById[channel.id] || {}).audio_fifo_ms || 0) }} ms</div>
          </div>
          <div class="meters">
            <i v-for="(meter, index) in ((liveById[channel.id] || {}).meters || []).slice(0, 16)" :key="index" :style="{ height: meterHeight(meter) }" :title="index"></i>
          </div>
          <p v-if="((liveById[channel.id] || {}).alarms || []).length" class="err">{{ (liveById[channel.id] || {}).alarms.join(", ") }}</p>
          <button @click="openMatrix(channel)">Audio matrix</button>
        </article>
        <p v-if="!channels.length">No channels yet. Create one on the Channels tab.</p>
      </section>

      <section v-else-if="tab === 'channels'">
        <div class="row" style="margin-bottom: 0.8rem">
          <button class="primary" @click="edit(blank('ingest'))">New ingest</button>
          <button class="primary" @click="edit(blank('egress'))">New egress</button>
        </div>
        <div class="card">
          <table>
            <thead><tr><th>ID</th><th>Direction</th><th>Mode</th><th>Port / remote</th><th>Exposure</th><th></th></tr></thead>
            <tbody>
              <tr v-for="channel in channels" :key="channel.id">
                <td>{{ channel.label }} <small>({{ channel.id }})</small></td>
                <td>{{ channel.direction }}</td>
                <td>{{ channel.srt.mode }}</td>
                <td>{{ channel.srt.mode === "caller" ? channel.srt.remote_host + ":" + channel.srt.remote_port : channel.srt.local_port }}</td>
                <td>{{ channel.srt.exposure }}{{ channel.srt.passphrase_set ? " · key set" : "" }}</td>
                <td class="row">
                  <button @click="edit(channel)">Edit</button>
                  <button @click="openMatrix(channel)">Audio</button>
                  <button class="warn" @click="remove(channel.id)">Delete</button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section v-else-if="tab === 'nmos'" class="card">
        <h2>MXL SRT Gateway</h2>
        <p>Node {{ nmos.node_id }} · device {{ nmos.device_id }}</p>
        <p>Registry {{ nmos.registry || "unconfigured" }}:{{ nmos.registry_port }} · node port {{ nmos.port }} · linked {{ nmos.built }}</p>
        <table>
          <thead><tr><th>Kind</th><th>Channel</th><th>Label</th><th>ID</th></tr></thead>
          <tbody>
            <tr v-for="item in nmos.resources || []" :key="item.id + item.kind">
              <td>{{ item.kind }}</td><td>{{ item.channel }}</td><td>{{ item.label }}</td><td>{{ item.id }}</td>
            </tr>
          </tbody>
        </table>
      </section>

      <section v-else class="card">
        <h2>Effective configuration</h2>
        <p v-if="config.restart_required" class="err">A global change is staged. Restart the process to apply it.</p>
        <div class="form">
          <label>Log level<input v-model="config.LOG_LEVEL" /></label>
          <label>Decoder<input v-model="config.DECODER" /></label>
          <label>Encoder<input v-model="config.ENCODER" /></label>
          <label>Public IP<input v-model="config.SRTGW_PUBLIC_IP" /></label>
          <label>Registry<input v-model="config.NMOS_REGISTRY_ADDRESS" /></label>
          <label>SRT ports<input v-model="config.SRT_PORT_RANGE" /></label>
          <label>Web port<input v-model="config.WEB_PORT" /></label>
          <label>NMOS port<input v-model="config.NMOS_PORT" /></label>
        </div>
        <div class="row" style="margin-top: 0.8rem">
          <button class="primary" @click="saveGlobals">Save globals</button>
          <button @click="download('json')">Export JSON</button>
          <button @click="download('env')">Export KEY=value</button>
        </div>
        <pre>{{ JSON.stringify(config.origin || {}, null, 2) }}</pre>
      </section>
    </main>

    <div v-if="editing" class="modal" @click.self="editing = null">
      <div class="card">
        <h2>{{ editing.id ? "Edit" : "New" }} {{ editing.direction }}</h2>
        <p v-if="error" class="err">{{ error }}</p>
        <div class="form">
          <label>ID<input v-model="editing.id" :disabled="channels.some((c) => c.id === editing.id)" /></label>
          <label>Label<input v-model="editing.label" /></label>
          <label>SRT mode
            <select v-model="editing.srt.mode"><option>listener</option><option>caller</option><option>rendezvous</option></select>
          </label>
          <label>Exposure
            <select v-model="editing.srt.exposure"><option>internal</option><option>internet</option></select>
          </label>
          <label>Local port<input v-model.number="editing.srt.local_port" type="number" /></label>
          <label>Remote host<input v-model="editing.srt.remote_host" /></label>
          <label>Remote port<input v-model.number="editing.srt.remote_port" type="number" /></label>
          <label>Latency ms<input v-model.number="editing.srt.latency_ms" type="number" /></label>
          <label>Passphrase (write-only)<input v-model="editing.srt.passphrase" type="password" placeholder="unchanged if empty" /></label>
          <label>Key length
            <select v-model.number="editing.srt.pbkeylen"><option :value="0">auto</option><option :value="16">AES-128</option><option :value="24">AES-192</option><option :value="32">AES-256</option></select>
          </label>
          <label>Stream ID<input v-model="editing.srt.streamid" /></label>
          <label>Accepted stream IDs<input v-model="editing.srt.accepted_streamids_text" placeholder="comma separated" /></label>
          <label>Peer allow<input v-model="editing.srt.peer_allow_text" placeholder="10.0.0.0/8" /></label>
          <label>Width<input v-model.number="editing.target.width" type="number" /></label>
          <label>Height<input v-model.number="editing.target.height" type="number" /></label>
          <label>Scan
            <select v-model="editing.target.scan"><option>progressive</option><option>interlaced</option></select>
          </label>
          <label>Rate
            <select v-model="editing.target.rate"><option>23.98</option><option>24</option><option>25</option><option>29.97</option><option>30</option><option>50</option><option>59.94</option><option>60</option></select>
          </label>
          <label>Deinterlacer
            <select v-model="editing.deinterlacer"><option>bwdif</option><option>yadif</option><option>weave</option></select>
          </label>
          <label>Aspect
            <select v-model="editing.aspect"><option>letterbox</option><option>fill</option></select>
          </label>
          <label>Loss
            <select v-model="editing.loss_mode"><option>slate</option><option>black</option></select>
          </label>
        </div>
        <div class="row" style="margin-top: 0.8rem">
          <button class="primary" @click="save">Apply</button>
          <button @click="editing = null">Cancel</button>
        </div>
      </div>
    </div>

    <div v-if="matrixChannel" class="modal" @click.self="matrixChannel = null">
      <div class="card matrix-card">
        <h2>Audio matrix · {{ matrixChannel.label || matrixChannel.id }}</h2>
        <div class="matrix-body">
        <p v-if="matrixError" class="err">{{ matrixError }}</p>
        <template v-if="matrixChannel.direction !== 'egress'">
          <p class="hint">Rows are decoded channels. Columns are MXL outputs. A column can sum several sources. Gain and mute apply to that output.</p>
          <div class="row">
            <button v-for="preset in ingestPresets" :key="preset[0]" @click="applyIngestPreset(preset[0])">{{ preset[1] }}</button>
          </div>
          <div class="form" style="margin-top: 0.6rem">
            <label>MXL channels<input :value="matrixOutCount" type="number" min="2" max="64" @change="setOutCount($event.target.value)" /></label>
            <label v-if="!((liveById[matrixChannel.id] || {}).tracks || []).length">Source tracks<input v-model.number="matrixSourceTracks" type="number" min="1" max="32" /></label>
            <label v-if="!((liveById[matrixChannel.id] || {}).tracks || []).length">Channels / track<input v-model.number="matrixSourceChannels" type="number" min="1" max="16" /></label>
          </div>
          <div class="matrix-scroll">
            <table class="matrix">
              <thead>
                <tr>
                  <th class="src">Source</th>
                  <th v-for="output in matrixOutCount" :key="output">Out {{ output }}</th>
                </tr>
                <tr>
                  <th class="src">Gain dB</th>
                  <th v-for="output in matrixOutCount" :key="'g' + output"><input class="gain" v-model.number="matrixGains[output - 1]" type="number" step="0.5" @change="applyColumnMeta" /></th>
                </tr>
                <tr>
                  <th class="src">Mute</th>
                  <th v-for="output in matrixOutCount" :key="'m' + output"><input v-model="matrixMutes[output - 1]" type="checkbox" @change="applyColumnMeta" /></th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="row in matrixSourceRows" :key="row.track + ':' + row.channel">
                  <th class="src" :title="row.detail">{{ row.label }}</th>
                  <td v-for="output in matrixOutCount" :key="output">
                    <button class="xp" :class="{ on: routed(row, output - 1) }" @click="toggleRoute(row, output - 1)" :title="row.detail"></button>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </template>
        <template v-else>
          <p class="hint">Rows are MXL channels. Each column is one channel of an egress PID. A slot takes a single source; click it again to clear.</p>
          <div class="row">
            <button v-for="preset in egressPresets" :key="preset[0]" @click="applyEgressPreset(preset[0])">{{ preset[1] }}</button>
            <button @click="addEgressTrack">Add track</button>
          </div>
          <div class="form" style="margin: 0.6rem 0">
            <label>MXL channels<input v-model.number="matrixMxlCount" type="number" min="1" max="64" /></label>
          </div>
          <div class="matrix-scroll">
            <table class="matrix">
              <thead>
                <tr>
                  <th class="src">MXL</th>
                  <template v-for="(track, index) in matrixTracks" :key="'h' + index">
                    <th v-for="slot in track.channels.length" :key="index + '-' + slot">A{{ index + 1 }}.{{ slot }}</th>
                  </template>
                </tr>
              </thead>
              <tbody>
                <tr v-for="source in matrixMxlRows" :key="source">
                  <th class="src">Ch {{ source + 1 }}</th>
                  <template v-for="(track, index) in matrixTracks" :key="'r' + index">
                    <td v-for="slot in track.channels.length" :key="index + '-' + slot">
                      <button class="xp" :class="{ on: egressSource(index, slot - 1) === source }" @click="setEgressSource(index, slot - 1, source)"></button>
                    </td>
                  </template>
                </tr>
              </tbody>
            </table>
          </div>
          <div v-for="(track, index) in matrixTracks" :key="index" class="track-card">
            <div class="row">
              <b>Track {{ index + 1 }}</b>
              <button class="warn" @click="matrixTracks.splice(index, 1)">Remove</button>
            </div>
            <div class="form">
              <label>Codec
                <select :value="track.codec" @change="setTrackCodec(track, $event.target.value)">
                  <option>aac</option><option>mp2</option><option>s302m</option><option>opus</option><option>ac3</option>
                </select>
              </label>
              <label>Layout
                <select :value="track.layout" @change="setTrackLayout(track, $event.target.value)">
                  <option>mono</option><option>stereo</option><option>5.1</option><option>7.1</option>
                </select>
              </label>
              <label>Language<input v-model="track.language" /></label>
              <label>Bitrate<input v-model.number="track.bitrate" type="number" /></label>
              <label>Gain dB<input v-model.number="track.gain_db" type="number" step="0.5" /></label>
              <label class="check">Mute<input v-model="track.mute" type="checkbox" /></label>
            </div>
          </div>
        </template>
        </div>
        <div class="row save-row">
          <button class="primary" @click="saveMatrix">Save matrix</button>
          <button @click="matrixChannel = null">Close</button>
        </div>
      </div>
    </div>
  </div>
</template>
