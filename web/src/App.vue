<script setup>
import { computed, onMounted, onUnmounted, provide, ref } from "vue";
import Pill from "./components/Pill.vue";
import DashboardTab from "./components/DashboardTab.vue";
import ChannelsTab from "./components/ChannelsTab.vue";
import AudioTab from "./components/AudioTab.vue";
import NmosTab from "./components/NmosTab.vue";
import StatusTab from "./components/StatusTab.vue";
import SettingsTab from "./components/SettingsTab.vue";
import { creating, drafts, isDirty, isMatrixDirty, live, matrixDrafts, routeEdits, settingsEdits, startLive, stopLive } from "./store.js";

const tabs = [
  { id: "dashboard", label: "Dashboard", component: DashboardTab, full: true },
  { id: "channels", label: "Channels", component: ChannelsTab },
  { id: "audio", label: "Audio", component: AudioTab, full: true },
  { id: "nmos", label: "NMOS & MXL", component: NmosTab },
  { id: "status", label: "Status", component: StatusTab },
  { id: "settings", label: "Settings", component: SettingsTab },
];

const current = ref("dashboard");
const tab = computed(() => tabs.find((t) => t.id === current.value));

function onHash() {
  const id = location.hash.slice(1);
  if (tabs.some((t) => t.id === id)) current.value = id;
}
function go(id) {
  current.value = id;
  location.hash = id;
}
provide("go", go);

const count = (pred) => live.status.filter(pred).length;
const running = computed(() => count((c) => c.state === "running"));
const waiting = computed(() => count((c) => ["connecting", "waiting", "no_signal"].includes(c.state)));
const failed = computed(() => count((c) => c.state === "error"));
const alarms = computed(() => live.status.reduce((n, c) => n + (c.alarms || []).length, 0));
const ingest = computed(() => live.channels.filter((c) => c.direction === "ingest").length);
const egress = computed(() => live.channels.length - ingest.value);
const registration = computed(() => {
  if (!live.config) return null;
  if (!live.config.NMOS_REGISTRY_ADDRESS) return { text: "no registry", kind: "neutral" };
  return live.ready?.body?.registered ? { text: "registered", kind: "ok" } : { text: "not registered", kind: "warn" };
});
const unsavedChannels = computed(() => Object.keys(drafts).filter(isDirty).length + Object.keys(routeEdits).length + (creating.draft ? 1 : 0));
const unsavedAudio = computed(() => Object.keys(matrixDrafts).filter(isMatrixDirty).length);
const unsavedSettings = computed(() => Object.keys(settingsEdits).length);

onMounted(() => {
  onHash();
  window.addEventListener("hashchange", onHash);
  startLive();
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHash);
  stopLive();
});
</script>

<template>
  <header>
    <h1>mxl-srt-gateway</h1>
    <span v-if="live.info" class="node">
      {{ live.info.label }} · {{ ingest }} ingest · {{ egress }} egress · {{ live.info.host_address }}
    </span>
    <span class="spacer"></span>
    <Pill v-if="live.status.length" :text="`${running} of ${live.status.length} running`" :kind="running ? 'ok' : 'neutral'" title="channels whose SRT and MXL sides run" />
    <Pill v-if="waiting" :text="`${waiting} waiting`" kind="warn" title="connecting to the SRT peer, waiting for an MXL flow, or no signal" />
    <Pill v-if="failed" :text="`${failed} failed`" kind="bad" title="channels that stopped with an error" />
    <Pill v-if="alarms" :text="`${alarms} alarm${alarms === 1 ? '' : 's'}`" kind="warn" title="e.g. an audio PID that left the stream" />
    <Pill v-if="registration" :text="registration.text" :kind="registration.kind" title="NMOS registration (the Query API lists this node)" />
    <Pill :text="live.connected ? 'live' : 'offline'" :kind="live.connected ? 'ok' : 'bad'" title="/api/v1/events" />
    <span v-if="live.info" class="muted small">v{{ live.info.version }} · MXL {{ live.info.mxl.slice(0, 7) }} · libsrt {{ live.info.srt }} · FFmpeg {{ live.info.ffmpeg }}</span>
  </header>
  <div v-if="live.error" class="banner bad">{{ live.error }}</div>
  <div v-else-if="live.everConnected && !live.connected" class="banner warn">Live updates lost. Reconnecting; values refresh every 2 s meanwhile.</div>
  <div v-if="live.config?.restart_required" class="banner warn">
    A setting that applies at start changed. Restart the gateway to apply it.
    <button class="btn small secondary" @click="go('settings')">Settings</button>
  </div>
  <div v-if="live.actionError" class="banner bad">
    {{ live.actionError }}
    <span class="spacer"></span>
    <button class="btn small secondary" @click="live.actionError = ''">Dismiss</button>
  </div>
  <div v-if="live.notice" class="banner info">
    {{ live.notice }}
    <span class="spacer"></span>
    <button class="btn small secondary" @click="live.notice = ''">Dismiss</button>
  </div>
  <nav>
    <button v-for="t in tabs" :key="t.id" :class="{ active: current === t.id }" :aria-current="current === t.id ? 'page' : undefined" @click="go(t.id)">
      {{ t.label
      }}<span v-if="t.id === 'channels' && unsavedChannels" class="count warn" title="channel changes not applied">{{ unsavedChannels }}</span
      ><span v-if="t.id === 'audio' && unsavedAudio" class="count warn" title="audio changes not saved">{{ unsavedAudio }}</span
      ><span v-if="t.id === 'status' && failed" class="count" title="channels with an error">{{ failed }}</span
      ><span v-if="t.id === 'settings' && unsavedSettings" class="count warn" title="settings not saved">{{ unsavedSettings }}</span>
    </button>
  </nav>
  <main :class="{ full: tab.full }">
    <component :is="tab.component" />
  </main>
</template>
