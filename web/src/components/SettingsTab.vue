<script setup>
// Settings (SPECIFICATION.md §7, §8): every setting with its value and origin, edits saved with
// PUT /api/v1/config, export (JSON document, KEY=value, with passphrases as a file only) and import.
import { computed, onMounted, ref } from "vue";
import OriginBadge from "./OriginBadge.vue";
import { api, copyText, download } from "../api.js";
import { hasConfigFile, importConfig, importDraft, live, refreshConfig, saveSettings, settingsEdits } from "../store.js";

const exportDoc = ref("");
const envText = ref("");
const exportMsg = ref("");
const importMsg = ref({ kind: "", text: "" });

const GROUPS = [
  { title: "Codecs and SRT", test: /^(DECODER|ENCODER|SRT_PORT_RANGE)$/ },
  { title: "NMOS", test: /^(NMOS_|HOST_ID|SRTGW_PUBLIC_IP)/ },
  { title: "MXL", test: /^(MXL_|SRTGW_HISTORY)/ },
  { title: "Process", test: /./ },
];
// One line per key, from the README settings table.
const DESCRIPTIONS = {
  WEB_PORT: "This UI, the REST API, /livez, /readyz and /metrics.",
  NMOS_PORT: "IS-04/IS-05 node API; the WebSocket is the next port.",
  SRT_PORT_RANGE: "Listener ports this process may bind (e.g. 9000-9099).",
  MXL_DOMAIN_SCAN_PATH: "Parent of every domain directory, mirrors included.",
  MXL_OUTPUT_DOMAIN_DIR: "This gateway's output domain, created if missing.",
  MXL_OUTPUT_DOMAIN_ID: "Stable domain id (UUID). An existing domain_def.json with another id is not overwritten.",
  MXL_HISTORY_DURATION_MS: "history_duration written when this process creates options.json.",
  SRTGW_HISTORY_DURATION_NS: "Old name of MXL_HISTORY_DURATION_MS (in ns).",
  MXL_CLEANUP_ON_EXIT: "On SIGTERM, delete only this gateway's output domain directory.",
  STATE_DIR: "Writable state: routes.json of the IS-05 activations.",
  SHUTDOWN_TIMEOUT_S: "Bound on the work after SIGTERM.",
  DECODER: "auto (NVDEC when it works, else CPU), nvdec or cpu. Channels can override it.",
  ENCODER: "auto (NVENC when it works, else CPU), nvenc or cpu. Channels can override it.",
  NMOS_SEED: "UUIDv5 seed of the node, device, flows, senders, receivers and domain id.",
  NMOS_LABEL: "Node and device label (empty: HOST_ID).",
  NMOS_TAGS: "Tags on the node and device: JSON object of string arrays.",
  NMOS_REGISTRY_ADDRESS: "Registration API host (empty: no registration).",
  NMOS_REGISTRY_PORT: "Registration API port.",
  NMOS_QUERY_ADDRESS: "Query API host, used by /readyz (default: the registry).",
  NMOS_QUERY_PORT: "Query API port (default: registration port + 1).",
  NMOS_DNS_SD: "DNS-SD browsing and mDNS advertisement.",
  NMOS_HOST_ADDRESS: "IPv4 announced to NMOS, in IS-05 and in the SRT address lines.",
  SRTGW_PUBLIC_IP: "Old name of NMOS_HOST_ADDRESS.",
  HOST_ID: "Identity in the default seed and label. Not an announced address.",
  LOG_LEVEL: "error, warn, info, debug or trace. Applies at once.",
};
const ALIASES = ["SRTGW_PUBLIC_IP", "SRTGW_HISTORY_DURATION_NS"];
const META = ["origin", "restart_required", "channels"];

const settings = computed(() => {
  const config = live.config || {};
  return Object.keys(config)
    .filter((k) => !META.includes(k))
    .sort()
    .map((key) => {
      const origin = config.origin?.[key] || "default";
      return {
        key,
        value: String(config[key] ?? ""),
        origin,
        // Without a configuration file a restart forgets a saved value, so only LOG_LEVEL (at once) is worth saving.
        editable: origin !== "env" && !ALIASES.includes(key) && (hasConfigFile.value || key === "LOG_LEVEL"),
        restart: key !== "LOG_LEVEL",
      };
    });
});
const groups = computed(() => {
  const left = [...settings.value];
  return GROUPS.map((g) => {
    const items = left.filter((s) => g.test.test(s.key));
    items.forEach((s) => left.splice(left.indexOf(s), 1));
    return { title: g.title, items };
  }).filter((g) => g.items.length);
});
const changed = computed(() => Object.keys(settingsEdits));

const value = (s) => (s.key in settingsEdits ? settingsEdits[s.key] : s.value);
function edit(s, v) {
  if (v === s.value) delete settingsEdits[s.key];
  else settingsEdits[s.key] = v;
}
const discard = () => changed.value.forEach((k) => delete settingsEdits[k]);

async function loadExport() {
  try {
    exportDoc.value = JSON.stringify(await api.get("/api/v1/config/export"), null, 2);
    envText.value = await api.text("/api/v1/config/export?format=env");
  } catch (e) {
    exportMsg.value = e.message;
  }
}
onMounted(() => {
  refreshConfig();
  loadExport();
});

async function save() {
  if (await saveSettings()) loadExport();
}
async function copy(text, what) {
  exportMsg.value = (await copyText(text)) ? `${what} copied.` : "Copy failed; select the text and copy it by hand.";
}
// The passphrases go into the file only; the page never shows them.
async function downloadWithSecrets() {
  try {
    const text = JSON.stringify(await api.get("/api/v1/config/export?secrets=1"), null, 2);
    download(`mxl-srt-gateway-${seed.value}-with-passphrases.json`, text + "\n");
    exportMsg.value = "Downloaded with passphrases. Keep that file like a password.";
  } catch (e) {
    exportMsg.value = e.message;
  }
}
function onFile(ev) {
  ev.target.files?.[0]?.text().then((t) => (importDraft.text = t));
}
const importKind = computed(() => (importDraft.text.trim().startsWith("{") ? "json" : "env"));
async function doImport() {
  if (importKind.value === "json") {
    try {
      JSON.parse(importDraft.text);
    } catch (e) {
      importMsg.value = { kind: "err", text: `Not JSON: ${e.message}` };
      return;
    }
  }
  try {
    await importConfig(importDraft.text);
    importMsg.value = {
      kind: "ok",
      text:
        importKind.value === "json"
          ? "Imported. The channels apply now; the other settings after a restart. Passphrases the document does not carry were kept."
          : "Imported. The settings apply after a restart (LOG_LEVEL at once).",
    };
    importDraft.text = "";
    loadExport();
  } catch (e) {
    importMsg.value = { kind: "err", text: e.message };
  }
}
const seed = computed(() => live.config?.NMOS_SEED || "srtgw");
</script>

<template>
  <div class="panel">
    <h3>Configuration</h3>
    <p class="note" style="margin-top: 0">
      Precedence: environment (ENV), then the configuration file (FILE), then the default. ENV settings are read-only here. Saved values go into
      <code>{{ live.info?.config_file || "SRTGW_CONFIG_FILE" }}</code> and apply when the gateway starts again, except LOG_LEVEL, which applies at
      once. Channels are set on the Channels and Audio tabs and apply at once.
    </p>
    <div v-if="live.info && !hasConfigFile" class="warnbox">
      SRTGW_CONFIG_FILE is not set: nothing is written to a file. Only LOG_LEVEL can be changed here; channel changes last until the gateway restarts.
    </div>
    <div class="actions" style="margin-top: 0">
      <span class="spacer"></span>
      <button class="btn secondary" :disabled="!changed.length" @click="discard">Discard</button>
      <button class="btn" :disabled="!changed.length" @click="save">Save{{ changed.length ? ` (${changed.length})` : "" }}</button>
    </div>
  </div>

  <div v-for="g in groups" :key="g.title" class="panel">
    <h3>{{ g.title }}</h3>
    <table>
      <thead>
        <tr><th style="width: 34%">Key</th><th>Value</th><th style="width: 9rem">Origin</th></tr>
      </thead>
      <tbody>
        <tr v-for="s in g.items" :key="s.key">
          <td>
            <code>{{ s.key }}</code>
            <div class="desc">{{ DESCRIPTIONS[s.key] || "" }}</div>
          </td>
          <td>
            <input :value="value(s)" :disabled="!s.editable" :class="{ dirty: s.key in settingsEdits }" :aria-label="s.key" @input="edit(s, $event.target.value)" />
          </td>
          <td class="nowrap">
            <OriginBadge :origin="s.origin" all />
            <span v-if="s.restart" class="badge default" title="applies when the gateway starts">RESTART</span>
          </td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>Export</h3>
      <p class="note">
        Every setting and every channel as one JSON document, without passphrases (<code>passphrase_set</code> tells which channels have one).
        The copy with passphrases is a download only.
      </p>
      <div class="actions" style="margin-top: 0">
        <span class="msg ok" style="margin: 0">{{ exportMsg }}</span>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!exportDoc" @click="copy(exportDoc, 'JSON')">Copy JSON</button>
        <button class="btn" :disabled="!exportDoc" @click="download(`mxl-srt-gateway-${seed}.json`, exportDoc + '\n')">Download JSON</button>
      </div>
      <pre style="max-height: 320px">{{ exportDoc }}</pre>
      <div class="actions">
        <button class="btn secondary" title="GET /api/v1/config/export?secrets=1, saved as a file" @click="downloadWithSecrets">Download with passphrases</button>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!envText" @click="copy(envText, 'KEY=value list')">Copy KEY=value</button>
        <button class="btn secondary" :disabled="!envText" @click="download(`mxl-srt-gateway-${seed}.env`, envText, 'text/plain')">Download KEY=value</button>
      </div>
    </div>
    <div class="panel">
      <h3>Import</h3>
      <p class="note">
        An exported JSON document replaces the settings and the channel list; a channel keeps its passphrase when the document has none for it.
        <code>KEY=value</code> lines change only those settings. Keys set by the environment keep their value.
      </p>
      <input type="file" accept="application/json,.json,.env,text/plain" aria-label="Open an exported file" @change="onFile" />
      <textarea v-model="importDraft.text" aria-label="Exported JSON or KEY=value lines" placeholder='{"WEB_PORT": 8120, "channels": [...]} or LOG_LEVEL=debug' style="margin-top: 0.5rem"></textarea>
      <div class="actions">
        <span class="msg" :class="importMsg.kind" style="margin: 0">{{ importMsg.text }}</span>
        <span class="spacer"></span>
        <span v-if="importDraft.text.trim()" class="muted small">{{ importKind === "json" ? "JSON document" : "KEY=value lines" }}</span>
        <button class="btn" :disabled="!importDraft.text.trim()" @click="doImport">Import</button>
      </div>
    </div>
  </div>
</template>
