<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";

const tab = ref("dashboard");
const channels = ref([]);
const live = ref({});
const nmos = ref({});
const config = ref({});
const error = ref("");
const editing = ref(null);
const matrixFor = ref(null);
const matrixPreset = ref("16ch-stereo");
const egressPreset = ref("8x-stereo-aac");
let socket;

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

async function applyMatrix() {
  await fetch(`/api/v1/channels/${matrixFor.value}/matrix`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ preset: matrixPreset.value, egress_preset: egressPreset.value }),
  });
  matrixFor.value = null;
  await loadChannels();
}

async function saveGlobals() {
  const body = {};
  for (const key of ["LOG_LEVEL", "DECODER", "ENCODER", "SRTGW_PUBLIC_IP", "NMOS_REGISTRY_ADDRESS", "WEB_PORT", "NMOS_PORT", "SRT_PORT_RANGE"]) {
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
                  <button @click="matrixFor = channel.id">Audio</button>
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

    <div v-if="matrixFor" class="modal" @click.self="matrixFor = null">
      <div class="card">
        <h2>Audio matrix · {{ matrixFor }}</h2>
        <div class="form">
          <label>Ingest preset
            <select v-model="matrixPreset">
              <option value="sequential">sequential</option>
              <option value="16ch-stereo">16 ch from 8 stereo PIDs</option>
              <option value="5.1+stereo">5.1 + stereo</option>
              <option value="302m-16">302M 16 ch</option>
              <option value="5.1-stereo-downmix">5.1 to stereo</option>
              <option value="mono-dual">mono to dual stereo</option>
            </select>
          </label>
          <label>Egress preset
            <select v-model="egressPreset">
              <option value="8x-stereo-aac">8 × stereo AAC</option>
              <option value="16ch-302m">16 ch 302M</option>
              <option value="5.1+stereo">5.1 + stereo</option>
              <option value="stereo">stereo</option>
            </select>
          </label>
        </div>
        <div class="row" style="margin-top: 0.8rem">
          <button class="primary" @click="applyMatrix">Apply presets</button>
          <button @click="matrixFor = null">Close</button>
        </div>
      </div>
    </div>
  </div>
</template>
