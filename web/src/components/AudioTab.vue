<script setup>
// Audio (SPECIFICATION.md §5.5, §6.2). Ingest: a matrix from the decoded tracks' channels to the
// MXL audio channels, with gain and mute per output and presets. Egress: the audio tracks (PIDs)
// with codec, layout, MXL channels, language, bitrate, gain and mute. Edits are drafts.
import { computed } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import Meters from "./Meters.vue";
import Segmented from "./Segmented.vue";
import { applyMatrix, isMatrixDirty, matrixDrafts, revertMatrix, selectedChannel, statusById } from "../store.js";

const c = selectedChannel;
const md = computed(() => (c.value ? matrixDrafts[c.value.id] : null));
const m = computed(() => md.value?.value);
const ui = computed(() => md.value?.ui);
const dirty = computed(() => c.value && isMatrixDirty(c.value.id));
const egress = computed(() => c.value?.direction === "egress");
const s = computed(() => (c.value ? statusById.value[c.value.id] : null));

// ---- shared ----
function layoutChannels(layout) {
  if (layout === "mono") return 1;
  if (layout === "5.1" || layout === "5.1(side)") return 6;
  if (layout === "7.1") return 8;
  return 2;
}
function defaultAudioBitrate(codec, layout) {
  const n = layoutChannels(layout);
  if (codec === "mp2") return n <= 1 ? 128000 : 256000;
  if (codec === "s302m") return 0;
  if (codec === "opus") return n <= 2 ? 128000 : 256000;
  if (n <= 1) return 96000;
  if (n <= 2) return 192000;
  if (n <= 6) return 384000;
  return 512000;
}
const tap = (track, channel, gain = 0) => ({ track, channel, gain_db: gain, mute: false });

// ---- ingest matrix ----
const INGEST_PRESETS = [
  { value: "sequential", label: "Sequential" },
  { value: "16ch-stereo", label: "16 ch from 8 stereo" },
  { value: "5.1+stereo", label: "5.1 + stereo" },
  { value: "302m-16", label: "302M 16 ch" },
  { value: "5.1-stereo-downmix", label: "5.1 to stereo" },
  { value: "mono-dual", label: "Mono to dual mono" },
];
function presetRoutes(name, n) {
  const routes = Array.from({ length: n }, () => []);
  if (name === "5.1+stereo") {
    for (let i = 0; i < Math.min(6, n); i++) routes[i].push(tap(0, i));
    if (n > 6) routes[6].push(tap(1, 0));
    if (n > 7) routes[7].push(tap(1, 1));
  } else if (name === "302m-16") {
    for (let i = 0; i < n; i++) routes[i].push(tap(Math.floor(i / 8), i % 8));
  } else if (name === "5.1-stereo-downmix") {
    routes[0] = [tap(0, 0), tap(0, 2, -3), tap(0, 4, -3)];
    routes[1] = [tap(0, 1), tap(0, 2, -3), tap(0, 5, -3)];
  } else if (name === "mono-dual") {
    routes[0].push(tap(0, 0));
    routes[1].push(tap(0, 0));
  } else if (name === "16ch-stereo") {
    for (let i = 0; i < n; i++) routes[i].push(tap(Math.floor(i / 2), i % 2));
  } else {
    for (let i = 0; i < n; i++) routes[i].push(tap(0, i));
  }
  return routes;
}
function applyPreset(name) {
  m.value.routes = presetRoutes(name, m.value.channels);
  ui.value.gains = {};
  ui.value.mutes = {};
}
function setOutCount(value) {
  const n = Math.max(2, Math.min(64, Number(value) || 2));
  m.value.channels = n;
  while (m.value.routes.length < n) m.value.routes.push([]);
  m.value.routes.splice(n);
}

// Rows: the decoded tracks' channels (or, before the stream is there, tracks × channels to plan),
// plus any routed source the stream does not have now.
const decoded = computed(() => s.value?.tracks || []);
const rows = computed(() => {
  if (!m.value) return [];
  const out = [];
  if (decoded.value.length) {
    decoded.value.forEach((t, index) => {
      const count = Number.parseInt(t.layout, 10) || 2;
      for (let ch = 0; ch < count; ch++) {
        out.push({ track: index, channel: ch, label: `T${index + 1}.${ch + 1}`, detail: `PID ${t.pid} · ${t.codec || "audio"} · ${t.layout || ""} · ${t.language || "und"}${t.missing ? " · missing" : ""}` });
      }
    });
  } else {
    const tracks = Math.max(1, Math.min(32, Number(ui.value.sourceTracks) || 8));
    const chans = Math.max(1, Math.min(16, Number(ui.value.sourceChannels) || 2));
    for (let t = 0; t < tracks; t++) for (let ch = 0; ch < chans; ch++) out.push({ track: t, channel: ch, label: `T${t + 1}.${ch + 1}`, detail: "waiting for the stream" });
  }
  const seen = new Set(out.map((r) => `${r.track}:${r.channel}`));
  m.value.routes.forEach((col) =>
    col.forEach((x) => {
      const key = `${x.track}:${x.channel}`;
      if (!seen.has(key)) {
        seen.add(key);
        out.push({ track: x.track, channel: x.channel, label: `T${x.track + 1}.${x.channel + 1}`, detail: "routed, not in the stream now" });
      }
    }),
  );
  return out;
});
const routed = (row, out) => (m.value.routes[out] || []).some((x) => x.track === row.track && x.channel === row.channel);
function toggle(row, out) {
  const col = m.value.routes[out];
  const at = col.findIndex((x) => x.track === row.track && x.channel === row.channel);
  if (at >= 0) col.splice(at, 1);
  else col.push({ track: row.track, channel: row.channel, gain_db: gainOf(out), mute: muteOf(out) });
}
// Gain and mute belong to an output: every tap of the column gets them; an empty column keeps them for its first tap.
const gainOf = (out) => (m.value.routes[out]?.length ? Number(m.value.routes[out][0].gain_db) || 0 : Number(ui.value.gains[out]) || 0);
const muteOf = (out) => (m.value.routes[out]?.length ? m.value.routes[out].every((x) => x.mute) : Boolean(ui.value.mutes[out]));
function setGain(out, value) {
  const g = Number(value) || 0;
  ui.value.gains[out] = g;
  m.value.routes[out].forEach((x) => (x.gain_db = g));
}
function setMute(out, value) {
  ui.value.mutes[out] = value;
  m.value.routes[out].forEach((x) => (x.mute = value));
}

// ---- egress tracks ----
const EGRESS_PRESETS = [
  { value: "8x-stereo-aac", label: "8 × stereo AAC" },
  { value: "16ch-302m", label: "16 ch 302M" },
  { value: "5.1+stereo", label: "5.1 + stereo" },
  { value: "stereo", label: "Stereo" },
];
const CODECS = [
  { value: "aac", label: "AAC" },
  { value: "mp2", label: "MP2" },
  { value: "s302m", label: "302M" },
  { value: "opus", label: "Opus" },
  { value: "ac3", label: "AC-3" },
];
const LAYOUTS = [
  { value: "mono", label: "Mono" },
  { value: "stereo", label: "Stereo" },
  { value: "5.1", label: "5.1" },
  { value: "7.1", label: "7.1" },
];
const track = (codec, layout, channels, bitrate) => ({ codec, layout, channels, bitrate, language: "und", gain_db: 0, mute: false, pid: 0 });
function presetTracks(name) {
  if (name === "16ch-302m") return [0, 1].map((p) => track("s302m", "7.1", Array.from({ length: 8 }, (_, ch) => p * 8 + ch), 0));
  if (name === "5.1+stereo") return [track("aac", "5.1", [0, 1, 2, 3, 4, 5], 384000), track("aac", "stereo", [6, 7], 192000)];
  return Array.from({ length: name === "8x-stereo-aac" ? 8 : 1 }, (_, i) => track("aac", "stereo", [i * 2, i * 2 + 1], 192000));
}
const highest = computed(() => (m.value?.tracks || []).reduce((max, t) => Math.max(max, ...(t.channels || []).map((ch) => ch + 1)), 0));
const mxlRows = computed(() => Array.from({ length: Math.max(1, Math.min(64, Number(ui.value?.mxlCount) || Math.max(16, highest.value))) }, (_, i) => i));
function applyTrackPreset(name) {
  m.value.tracks = presetTracks(name);
}
function addTrack() {
  if (m.value.tracks.length >= 16) return;
  m.value.tracks.push(track("aac", "stereo", [highest.value, highest.value + 1], 192000));
}
function setSlot(t, slot, source) {
  const list = m.value.tracks[t].channels;
  list[slot] = list[slot] === source ? -1 : source;
}
function setLayout(t, layout) {
  const n = layoutChannels(layout);
  const next = (t.channels || []).slice(0, n);
  while (next.length < n) next.push(next.length ? next[next.length - 1] + 1 : 0);
  t.layout = layout;
  t.channels = next;
  t.bitrate = defaultAudioBitrate(t.codec, layout);
}
function setCodec(t, codec) {
  t.codec = codec;
  t.bitrate = defaultAudioBitrate(codec, t.layout);
}
const unassigned = computed(() => (m.value?.tracks || []).some((t) => (t.channels || []).some((ch) => ch < 0)));
</script>

<template>
  <div class="toolbar"><ChannelPicker :dirty="isMatrixDirty" /></div>
  <div v-if="!c || !m" class="panel empty">No channel yet. Create one on the Channels tab.</div>
  <template v-else-if="!egress">
    <div class="grid two">
      <div class="panel">
        <h3>Decoded tracks</h3>
        <table v-if="decoded.length">
          <thead>
            <tr><th>Track</th><th>PID</th><th>Codec</th><th>Layout</th><th>Language</th><th></th></tr>
          </thead>
          <tbody>
            <tr v-for="(t, i) in decoded" :key="i" :class="{ dim: t.missing }">
              <td>T{{ i + 1 }}</td>
              <td class="num">{{ t.pid }}</td>
              <td>{{ t.codec }}</td>
              <td>{{ t.layout }}</td>
              <td>{{ t.language || "und" }}</td>
              <td><span v-if="t.missing" class="pill warn">missing</span></td>
            </tr>
          </tbody>
        </table>
        <p v-else class="note" style="margin-top: 0">
          No stream yet. Plan the matrix with the expected tracks; the real ones replace these rows when the stream arrives.
        </p>
        <div v-if="!decoded.length" class="fields grid">
          <div>
            <label for="mx-st">Expected tracks</label>
            <input id="mx-st" v-model.number="ui.sourceTracks" type="number" min="1" max="32" placeholder="8" />
          </div>
          <div>
            <label for="mx-sc">Channels per track</label>
            <input id="mx-sc" v-model.number="ui.sourceChannels" type="number" min="1" max="16" placeholder="2" />
          </div>
        </div>
      </div>
      <div class="panel">
        <h3>MXL audio flow</h3>
        <div class="fields grid">
          <div>
            <label for="mx-out">MXL channels (2–64)</label>
            <input id="mx-out" :value="m.channels" type="number" min="2" max="64" :class="{ dirty }" @change="setOutCount($event.target.value)" />
          </div>
        </div>
        <label>Levels on the MXL channels</label>
        <Meters :peaks="s?.meters || []" :label="`${c.label} MXL audio levels`" />
        <p class="note">A different channel count makes a new MXL audio flow. Unrouted channels are silent.</p>
      </div>
    </div>
    <div class="panel">
      <h3>
        Routing
        <span class="spacer"></span>
        <span v-if="dirty" class="pill warn">not saved</span>
      </h3>
      <div class="row tight">
        <span class="muted small">Presets</span>
        <Segmented :options="INGEST_PRESETS" :model-value="null" label="Presets" @update:model-value="applyPreset" />
      </div>
      <p class="note">Rows are the decoded channels (T track . channel), columns the MXL channels. A column can sum several sources; gain and mute apply to the column.</p>
      <div class="matrix-scroll">
        <table class="matrix">
          <thead>
            <tr>
              <th class="src">Source</th>
              <th v-for="o in m.channels" :key="o">{{ o }}</th>
            </tr>
            <tr>
              <th class="src">Gain dB</th>
              <th v-for="o in m.channels" :key="'g' + o"><input class="gain" type="number" step="0.5" :value="gainOf(o - 1)" :aria-label="`gain of MXL channel ${o}`" @change="setGain(o - 1, $event.target.value)" /></th>
            </tr>
            <tr>
              <th class="src">Mute</th>
              <th v-for="o in m.channels" :key="'m' + o"><input type="checkbox" :checked="muteOf(o - 1)" :aria-label="`mute MXL channel ${o}`" @change="setMute(o - 1, $event.target.checked)" /></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="row in rows" :key="row.track + ':' + row.channel">
              <th class="src" :title="row.detail">{{ row.label }}</th>
              <td v-for="o in m.channels" :key="o">
                <button class="xp" :class="{ on: routed(row, o - 1) }" :aria-pressed="routed(row, o - 1)" :title="`${row.label} → MXL ${o} (${row.detail})`" @click="toggle(row, o - 1)"></button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <div class="actions">
        <span class="muted small">Saving restarts only this channel.</span>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!dirty" @click="revertMatrix(c.id)">Revert</button>
        <button class="btn" :disabled="!dirty" @click="applyMatrix(c.id)">Save audio of {{ c.id }}</button>
      </div>
    </div>
  </template>
  <template v-else>
    <div class="panel">
      <h3>
        Audio tracks (PIDs)
        <span class="spacer"></span>
        <span v-if="dirty" class="pill warn">not saved</span>
      </h3>
      <div class="row tight">
        <span class="muted small">Presets</span>
        <Segmented :options="EGRESS_PRESETS" :model-value="null" label="Presets" @update:model-value="applyTrackPreset" />
        <button class="btn secondary small" :disabled="m.tracks.length >= 16" @click="addTrack">Add track</button>
        <span class="spacer"></span>
        <label for="mx-mxl" style="margin: 0">MXL channels shown</label>
        <input id="mx-mxl" v-model.number="ui.mxlCount" type="number" min="1" max="64" :placeholder="String(Math.max(16, highest))" style="width: 5rem" />
      </div>
      <p class="note">Rows are the channels of the routed MXL audio flow, columns the channels of each track. A slot takes one source; click it again to clear it.</p>
      <div v-if="m.tracks.length" class="matrix-scroll">
        <table class="matrix">
          <thead>
            <tr>
              <th class="src">MXL</th>
              <template v-for="(t, i) in m.tracks" :key="'h' + i">
                <th v-for="slot in t.channels.length" :key="i + '-' + slot" :title="`track ${i + 1} (${t.codec} ${t.layout}), channel ${slot}`">A{{ i + 1 }}.{{ slot }}</th>
              </template>
            </tr>
          </thead>
          <tbody>
            <tr v-for="src in mxlRows" :key="src">
              <th class="src">Ch {{ src + 1 }}</th>
              <template v-for="(t, i) in m.tracks" :key="'r' + i">
                <td v-for="slot in t.channels.length" :key="i + '-' + slot">
                  <button class="xp" :class="{ on: t.channels[slot - 1] === src }" :aria-pressed="t.channels[slot - 1] === src" @click="setSlot(i, slot - 1, src)"></button>
                </td>
              </template>
            </tr>
          </tbody>
        </table>
      </div>
      <p v-else class="empty">No audio track: the stream has video only.</p>
      <div v-if="unassigned" class="warnbox">A track channel has no source and is silent.</div>
    </div>
    <div class="tracks">
      <div v-for="(t, i) in m.tracks" :key="i" class="track">
        <h4>
          Track {{ i + 1 }}
          <span class="muted small">PID {{ t.pid || `${(c.egress?.video_pid || 256) + 1 + i} (auto)` }}</span>
          <span class="spacer"></span>
          <button class="btn small danger" @click="m.tracks.splice(i, 1)">Remove</button>
        </h4>
        <label>Codec</label>
        <Segmented :options="CODECS" :model-value="t.codec" class="small" label="Codec" @update:model-value="setCodec(t, $event)" />
        <label>Layout</label>
        <Segmented :options="LAYOUTS" :model-value="t.layout" class="small" label="Layout" @update:model-value="setLayout(t, $event)" />
        <div class="fields grid">
          <div>
            <label :for="`tr-lang-${i}`">Language (ISO 639)</label>
            <input :id="`tr-lang-${i}`" v-model.trim="t.language" maxlength="3" />
          </div>
          <div>
            <label :for="`tr-br-${i}`">Bitrate in bit/s{{ t.codec === "s302m" ? " (PCM)" : "" }}</label>
            <input :id="`tr-br-${i}`" v-model.number="t.bitrate" type="number" min="0" step="16000" :disabled="t.codec === 's302m'" />
          </div>
          <div>
            <label :for="`tr-gain-${i}`">Gain dB</label>
            <input :id="`tr-gain-${i}`" v-model.number="t.gain_db" type="number" step="0.5" />
          </div>
          <div>
            <label :for="`tr-pid-${i}`">PID (0: auto)</label>
            <input :id="`tr-pid-${i}`" v-model.number="t.pid" type="number" min="0" max="8190" />
          </div>
          <div style="align-self: end">
            <label class="check"><input v-model="t.mute" type="checkbox" /> Mute</label>
          </div>
        </div>
      </div>
    </div>
    <div class="actions">
      <span class="muted small">Saving restarts only this channel. AC-3 and Opus need the encoder in the build and a receiver that takes them.</span>
      <span class="spacer"></span>
      <button class="btn secondary" :disabled="!dirty" @click="revertMatrix(c.id)">Revert</button>
      <button class="btn" :disabled="!dirty" @click="applyMatrix(c.id)">Save audio of {{ c.id }}</button>
    </div>
  </template>
</template>
