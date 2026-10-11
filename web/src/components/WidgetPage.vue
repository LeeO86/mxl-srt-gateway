<script setup>
// Operator-screen widget (SPECIFICATION.md §7.1): /widget/channel-status?channel=<id>[&theme=dark|light|transparent]
// is one channel's SRT state (connection, peer, rate, RTT, loss, latency, meters, errors) with start/stop
// (the channel's enabled flag), without the app around it, on this gateway's own API (same origin).
// It posts {type: "widget-ready"} and {type: "widget-size", w, h} to the page that frames it.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import Meters from "./Meters.vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, fmtBitrate, num, stateKind } from "../api.js";
import { live, setEnabled, startLive, statusById, stopLive } from "../store.js";

const params = new URLSearchParams(location.search);
const id = params.get("channel") || "";
const theme = params.get("theme");
if (theme) document.documentElement.dataset.theme = theme;
// A frame is see-through only when its color scheme matches its parent's; without the meta it is the default.
if (theme === "transparent") document.querySelector('meta[name="color-scheme"]')?.remove();

const channel = computed(() => live.channels.find((c) => c.id === id));
const s = computed(() => statusById.value[id] || { state: "idle", srt: {}, meters: [], alarms: [] });
const egress = computed(() => channel.value?.direction === "egress");
const state = computed(() => (channel.value?.enabled ? s.value.state : "idle"));

// Listening or calling until a peer is there, then who is connected.
const connection = computed(() => {
  const c = channel.value;
  const srt = c.srt || {};
  if (!c.enabled) return "stopped";
  if (s.value.srt?.connected) return `${srt.mode === "listener" ? "connected from" : "connected to"} ${s.value.peer || "peer"}`;
  if (srt.mode === "listener") return `listening on port ${srt.local_port || "–"}`;
  return `${srt.mode === "rendezvous" ? "rendezvous with" : "calling"} ${srt.remote_host || "–"}:${srt.remote_port || "–"}`;
});

const stats = computed(() => {
  const srt = s.value.srt || {};
  const up = Boolean(srt.connected);
  return [
    { k: egress.value ? "Send rate" : "Receive rate", v: fmtBitrate(srt.bitrate_bps) },
    { k: "RTT", v: up ? `${num(srt.rtt_ms, 1)} ms` : "–" },
    { k: "Lost / retrans", v: `${srt.loss ?? 0} / ${srt.retrans ?? 0}`, title: "packets lost / retransmitted since the connection" },
    { k: "Latency", v: `${channel.value.srt?.latency_ms ?? "–"} ms`, title: "configured SRT latency" },
    { k: "SRT buffer", v: up ? `${num(srt.buffer_ms)} ms` : "–" },
    { k: "Dropped", v: srt.drop ?? 0, title: "packets dropped (too late)" },
  ];
});

// Stop asks once more: a click by mistake would take a live feed off.
const confirmStop = ref(false);
let confirmTimer = 0;
function toggle() {
  if (channel.value.enabled && !confirmStop.value) {
    confirmStop.value = true;
    confirmTimer = setTimeout(() => (confirmStop.value = false), 3000);
    return;
  }
  clearTimeout(confirmTimer);
  confirmStop.value = false;
  setEnabled(id, !channel.value.enabled);
}

const root = ref(null);
let ready = false;
let observer = null;

function post(message) {
  if (window.parent !== window) window.parent.postMessage(message, "*");
}
function postSize() {
  const rect = root.value.getBoundingClientRect();
  post({ type: "widget-size", w: Math.round(rect.width), h: Math.round(rect.height) });
}
watch(
  channel,
  (c) => {
    if (!c || ready) return;
    ready = true;
    post({ type: "widget-ready" });
    postSize();
  },
  { flush: "post" },
);
onMounted(() => {
  startLive();
  observer = new ResizeObserver(() => ready && postSize());
  observer.observe(root.value);
});
onUnmounted(() => {
  observer?.disconnect();
  clearTimeout(confirmTimer);
  stopLive();
});
</script>

<template>
  <div ref="root" class="widget-root">
    <template v-if="channel">
      <div class="widget-head">
        <span class="name" :title="channel.id">{{ channel.label || channel.id }}</span>
        <span class="badge">{{ channel.direction }}</span>
        <span class="spacer"></span>
        <Pill v-if="!channel.enabled" text="stopped" kind="neutral" />
        <Pill v-else :text="STATE_TEXT[state] || state" :kind="stateKind(state)" />
        <button class="btn small" :class="channel.enabled ? (confirmStop ? 'danger' : 'secondary') : ''" @click="toggle">
          {{ channel.enabled ? (confirmStop ? "Really stop?" : "Stop") : "Start" }}
        </button>
      </div>
      <div class="widget-line">
        <span class="conn" :title="connection">{{ connection }}</span>
        <span v-if="s.failover_active" class="badge">{{ egress ? "copy connected" : "backup on air" }}</span>
        <span class="spacer"></span>
        <span class="muted">{{ s.source_format || "–" }} → {{ s.target_format || "–" }}</span>
      </div>
      <div class="stats">
        <div v-for="x in stats" :key="x.k" :title="x.title"><b>{{ x.k }}</b>{{ x.v }}</div>
      </div>
      <Meters v-if="!egress" :peaks="s.meters || []" :label="`${channel.label || channel.id} MXL audio levels`" />
      <p v-if="s.error" class="alarm bad">{{ s.error }}</p>
      <p v-for="a in s.alarms || []" :key="a" class="alarm">{{ a }}</p>
    </template>
    <div v-else class="empty">{{ live.error || `Waiting for channel ${id}…` }}</div>
    <div v-if="live.actionError" class="widget-error" title="Dismiss" @click="live.actionError = ''">{{ live.actionError }}</div>
  </div>
</template>
