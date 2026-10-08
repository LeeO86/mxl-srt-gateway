<script setup>
// Status (SPECIFICATION.md §7, §9): health probes, versions, the process and GPU numbers of
// /metrics, and per channel its state, SRT statistics, frame sync, codec and alarms.
import { computed, onMounted, onUnmounted, ref } from "vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, api, fmtBitrate, fmtBytes, num, parseMetrics, probe, stateKind } from "../api.js";
import { live } from "../store.js";

const livez = ref(null);
const metrics = ref({ "": {} });
let timer = null;

async function load() {
  livez.value = await probe("/livez");
  try {
    metrics.value = parseMetrics(await api.text("/metrics"));
  } catch {
    /* keep the last values */
  }
}
onMounted(() => {
  load();
  timer = setInterval(load, 2000);
});
onUnmounted(() => clearInterval(timer));

const code = (p) => ({ text: p?.status ? String(p.status) : "no answer", kind: p?.status === 200 ? "ok" : p?.status ? "warn" : "bad" });
const ready = computed(() => live.ready?.body || {});
const values = computed(() => live.config || {});
const info = computed(() => live.info || {});
const proc = computed(() => metrics.value[""] || {});
const m = (c) => metrics.value[c.id] || {};
const latencyMs = (c) => {
  const x = m(c);
  return x.encode_latency_seconds_count > 0 ? ((x.encode_latency_seconds_sum / x.encode_latency_seconds_count) * 1000).toFixed(1) : "–";
};
const dir = (id) => live.channels.find((c) => c.id === id)?.direction || "";
</script>

<template>
  <div class="grid fit">
    <div class="panel">
      <h3>Health</h3>
      <dl class="kv">
        <dt>/livez</dt>
        <dd><Pill :text="code(livez).text" :kind="code(livez).kind" /></dd>
        <dt>/readyz</dt>
        <dd><Pill :text="code(live.ready).text" :kind="code(live.ready).kind" /></dd>
        <dt>Channels up</dt>
        <dd><Pill :text="ready.channels === false ? 'no' : 'yes'" :kind="ready.channels === false ? 'bad' : 'ok'" /></dd>
        <dt>NMOS registered</dt>
        <dd>
          <Pill v-if="!values.NMOS_REGISTRY_ADDRESS" text="no registry" kind="neutral" />
          <Pill v-else :text="ready.registered ? 'yes' : 'no'" :kind="ready.registered ? 'ok' : 'bad'" />
        </dd>
        <dt>Restart required</dt>
        <dd><Pill :text="values.restart_required ? 'yes' : 'no'" :kind="values.restart_required ? 'warn' : 'ok'" /></dd>
        <dt>Live updates</dt>
        <dd><Pill :text="live.connected ? 'connected' : 'reconnecting'" :kind="live.connected ? 'ok' : 'warn'" /></dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Versions</h3>
      <dl class="kv">
        <dt>Gateway</dt>
        <dd>{{ info.version || "–" }}</dd>
        <dt>MXL</dt>
        <dd><code :title="info.mxl">{{ (info.mxl || "–").slice(0, 12) }}</code></dd>
        <dt>nmos-cpp</dt>
        <dd><code :title="info.nmos_cpp">{{ (info.nmos_cpp || "–").slice(0, 12) }}</code></dd>
        <dt>libsrt</dt>
        <dd>{{ info.srt || "–" }}</dd>
        <dt>FFmpeg</dt>
        <dd>{{ info.ffmpeg || "–" }}</dd>
        <dt>Codecs</dt>
        <dd>DECODER={{ values.DECODER || "–" }} · ENCODER={{ values.ENCODER || "–" }}</dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Process and GPU</h3>
      <dl class="kv">
        <dt>Memory</dt>
        <dd>{{ fmtBytes(proc.process_resident_bytes) }}</dd>
        <dt>NVDEC sessions</dt>
        <dd>{{ num(proc.nvdec_sessions) }}</dd>
        <dt>NVENC sessions</dt>
        <dd>{{ num(proc.nvenc_sessions) }}</dd>
        <dt>GPU memory</dt>
        <dd>{{ fmtBytes(proc.gpu_memory_bytes) }}</dd>
        <dt>Config file</dt>
        <dd><code>{{ info.config_file || "none (SRTGW_CONFIG_FILE unset)" }}</code></dd>
      </dl>
    </div>
  </div>

  <div class="panel">
    <h3>Channels</h3>
    <div style="overflow-x: auto">
      <table>
        <thead>
          <tr>
            <th>Channel</th>
            <th>State</th>
            <th class="num" title="round-trip time">RTT</th>
            <th class="num">SRT rate</th>
            <th class="num" title="packets lost / retransmitted / dropped">Lost / retr. / drop</th>
            <th class="num" title="SRT buffer">Buffer</th>
            <th>Format</th>
            <th>Codec</th>
            <th class="num" title="decode or encode frames per second">fps</th>
            <th class="num" title="mean since start">Encode ms</th>
            <th class="num" title="MXL frames written (ingest) or encoded (egress)">Frames</th>
            <th class="num" title="frame sync repeats / drops">Rep. / drop</th>
            <th class="num" title="decode errors">Dec. err.</th>
            <th class="num" title="audio FIFO and drift">Audio</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="c in live.status" :key="c.id">
            <td>
              <strong>{{ c.label || c.id }}</strong> <span class="muted small">{{ dir(c.id) }}</span>
              <div v-for="a in c.alarms || []" :key="a" class="desc" style="color: var(--warn)">{{ a }}</div>
              <div v-if="c.error" class="desc" style="color: var(--bad)">{{ c.error }}</div>
            </td>
            <td class="nowrap">
              <Pill :text="STATE_TEXT[c.state] || c.state" :kind="stateKind(c.state)" />
              <span v-if="c.failover_active" class="pill warn">{{ dir(c.id) === "egress" ? "copy" : "backup" }}</span>
            </td>
            <td class="num">{{ c.srt?.connected ? num(c.srt.rtt_ms, 1) : "–" }}</td>
            <td class="num">{{ fmtBitrate(c.srt?.bitrate_bps) }}</td>
            <td class="num">{{ c.srt?.loss ?? 0 }} / {{ c.srt?.retrans ?? 0 }} / {{ c.srt?.drop ?? 0 }}</td>
            <td class="num">{{ c.srt?.connected ? `${num(c.srt.buffer_ms)} ms` : "–" }}</td>
            <td class="nowrap">{{ c.source_format || "–" }} → {{ c.target_format || "–" }}</td>
            <td class="nowrap">{{ c.decoder || c.encoder || "–" }}</td>
            <td class="num">{{ num(c.decode_fps || c.encode_fps, 1) }}</td>
            <td class="num">{{ latencyMs(c) }}</td>
            <td class="num">{{ c.frames ?? 0 }}</td>
            <td class="num">{{ c.repeats ?? 0 }} / {{ c.drops ?? 0 }}</td>
            <td class="num">{{ num(m(c).decode_errors_total) }}</td>
            <td class="num nowrap">{{ dir(c.id) === "egress" ? "–" : `${num(c.audio_fifo_ms)} ms · ${num(c.audio_drift_ppm, 1)} ppm` }}</td>
          </tr>
        </tbody>
      </table>
    </div>
    <p v-if="!live.status.length" class="empty">No channel.</p>
    <p class="note">
      Counters since the channel started. All of them, with the encode latency histogram: <a href="/metrics" target="_blank">/metrics</a> (Prometheus) ·
      <a href="/statusz" target="_blank">/statusz</a> (JSON).
    </p>
  </div>
</template>
