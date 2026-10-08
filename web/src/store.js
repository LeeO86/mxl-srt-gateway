// Shared UI state: info, channel status (WebSocket /api/v1/events, four times a second), channel
// configuration, NMOS, MXL domains, the configuration and readiness (polled), the drafts that
// survive tab switches and reconnects, and the actions every page uses.
import { computed, reactive } from "vue";
import { api, canon, clone, probe } from "./api.js";

function stored(key, fallback) {
  try {
    return localStorage.getItem(`mxl-srt-gateway.${key}`) ?? fallback;
  } catch {
    return fallback;
  }
}
function store(key, value) {
  try {
    localStorage.setItem(`mxl-srt-gateway.${key}`, String(value));
  } catch {
    /* kept for this tab only */
  }
}

export const live = reactive({
  info: null, // GET /api/v1/info
  status: [], // channel status, newest from the WebSocket
  channels: [], // channel configuration, GET /api/v1/channels (passphrases masked: passphrase_set)
  nmos: null, // GET /api/v1/nmos
  config: null, // GET /api/v1/config: values, origin, restart_required, channels
  ready: null, // GET /readyz: { status, body: { ready, channels, registered } }
  domains: [], // GET /api/v1/domains: MXL domains and their flows
  connected: false,
  everConnected: false,
  error: "", // API unreachable
  actionError: "", // the last failed action (banner)
  notice: "", // the last action's result worth showing (banner)
  selected: stored("channel", ""), // the channel the Channels and Audio tabs edit; NEW for the new channel
});

// Thumbnails reload once a second.
export const view = reactive({ tick: 0 });

export const NEW = "__new";

export const statusById = computed(() => Object.fromEntries(live.status.map((c) => [c.id, c])));
/** The selected channel, or the first one (also while the new channel is selected). */
export const selectedChannel = computed(() => live.channels.find((c) => c.id === live.selected) || live.channels[0] || null);
export function selectChannel(id) {
  live.selected = id;
  if (id !== NEW) store("channel", id);
}

/** The configuration file is set: settings and channel changes are written there. */
export const hasConfigFile = computed(() => Boolean(live.info?.config_file));
/** Origin of a setting: "env", "file" or "default". */
export const originOf = (key) => live.config?.origin?.[key] || "default";

/** Runs an action; a failure goes to the error banner. Returns the result (true for an empty answer), or undefined. */
export async function act(fn, notice = "") {
  try {
    const result = await fn();
    live.actionError = "";
    if (notice) live.notice = typeof notice === "function" ? notice(result) : notice;
    return result ?? true;
  } catch (e) {
    live.actionError = e.message;
    return undefined;
  }
}

// ---- channel settings drafts -------------------------------------------------
// A draft is taken from the configuration while it is unchanged and kept while it is edited, so
// tab switches, WebSocket reconnects and refreshes never overwrite an edit. The audio matrix is
// not part of it (the Audio tab owns it), and the passphrase is write-only: the form starts empty
// and an empty field leaves the stored one as it is.

const LISTS = ["accepted_streamids", "peer_allow"];
const splitList = (text) =>
  String(text || "")
    .split(/[\s,]+/)
    .map((s) => s.trim())
    .filter(Boolean);

function endpointForm(endpoint) {
  const e = clone(endpoint || {});
  for (const k of LISTS) e[k] = (e[k] || []).join(", ");
  e.passphrase = "";
  e.clear_passphrase = false;
  return e;
}
function endpointBody(form) {
  const e = clone(form);
  for (const k of LISTS) e[k] = splitList(e[k]);
  if (e.clear_passphrase) e.passphrase = "";
  else if (!e.passphrase) delete e.passphrase;
  delete e.clear_passphrase;
  delete e.passphrase_set;
  return e;
}

/** The editable form of a channel document. */
export function toForm(channel) {
  const form = clone(channel);
  form.srt = endpointForm(channel.srt);
  form.backup = { ...clone(channel.backup || {}), endpoint: endpointForm(channel.backup?.endpoint) };
  form.program = { ...clone(channel.program || {}), audio_pids: (channel.program?.audio_pids || []).join(", ") };
  delete form.audio_outputs;
  if (form.egress) delete form.egress.audio_tracks;
  return form;
}

/** The PUT/POST body of a form. */
export function toBody(form) {
  const body = clone(form);
  body.srt = endpointBody(form.srt);
  body.backup.endpoint = endpointBody(form.backup.endpoint);
  body.program.audio_pids = splitList(form.program.audio_pids)
    .map(Number)
    .filter((n) => Number.isInteger(n) && n > 0);
  return body;
}

export const drafts = reactive({}); // channel id -> { value, base }

function syncDraft(id, fresh) {
  const d = drafts[id];
  const c = canon(fresh);
  if (!d || canon(d.value) === d.base) {
    if (!d || d.base !== c) drafts[id] = { value: fresh, base: c };
  } else {
    d.base = c; // edited: the draft stays, compared with the newest configuration
  }
}
export const isDirty = (id) => {
  const d = drafts[id];
  return !!d && canon(d.value) !== d.base;
};
export function revertDraft(id) {
  const channel = live.channels.find((c) => c.id === id);
  if (channel) drafts[id] = { value: toForm(channel), base: canon(toForm(channel)) };
}

function storeChannel(result) {
  const at = live.channels.findIndex((c) => c.id === result.id);
  if (at >= 0) live.channels[at] = result;
  else live.channels.push(result);
}

/** PUT /api/v1/channels/{id}: only that channel restarts. */
export async function applyDraft(id) {
  const d = drafts[id];
  if (!d) return undefined;
  const result = await act(() => api.put(`/api/v1/channels/${encodeURIComponent(id)}`, toBody(d.value)), `${id}: applied. Only this channel restarted.`);
  if (result) {
    storeChannel(result);
    revertDraft(id);
    syncMatrix(id, matrixOf(result));
  }
  return result;
}

export async function setEnabled(id, enabled) {
  const result = await act(
    () => api.put(`/api/v1/channels/${encodeURIComponent(id)}`, { enabled }),
    `${id}: ${enabled ? "enabled" : "disabled"}.`,
  );
  if (result) {
    storeChannel(result);
    const d = drafts[id];
    if (d && isDirty(id)) d.value.enabled = enabled;
    syncDraft(id, toForm(result));
  }
}

export async function deleteChannel(id) {
  const result = await act(() => api.del(`/api/v1/channels/${encodeURIComponent(id)}`), `${id}: deleted.`);
  if (result) {
    live.channels = live.channels.filter((c) => c.id !== id);
    delete drafts[id];
    delete matrixDrafts[id];
    delete routeEdits[id];
    selectChannel(live.channels[0]?.id || "");
  }
}

// ---- new channel ----------------------------------------------------------------

function blankEndpoint(mode) {
  return {
    mode,
    local_address: "",
    local_port: 0,
    remote_host: "",
    remote_port: 0,
    latency_ms: 200,
    passphrase_set: false,
    pbkeylen: 0,
    streamid: "",
    accepted_streamids: [],
    max_bandwidth: 0,
    overhead: 25,
    payload_size: 1316,
    connect_timeout_ms: 3000,
    peer_allow: [],
    exposure: "internal",
  };
}

/** A new channel with the gateway's defaults (config.cpp defaultIngest / defaultEgress). */
function blankChannel(direction) {
  const egress = direction === "egress";
  return {
    id: "",
    label: "",
    direction,
    enabled: true,
    srt: blankEndpoint(egress ? "caller" : "listener"),
    backup: { enabled: false, endpoint: blankEndpoint(egress ? "caller" : "listener"), failover_ms: 1000, failback_ms: 5000, force: "auto" },
    program: { select: "first", number: 0, service_name: "", video_pid: 0, audio_pids: [] },
    target: { width: 1920, height: 1080, scan: "progressive", field_order: "progressive", rate: "50", color: "bt709" },
    source_scan: "auto",
    deinterlacer: "bwdif",
    scale: "bicubic",
    aspect: "letterbox",
    sync_latency_ms: 120,
    hold_ms: 500,
    loss_mode: "slate",
    audio_offset_ms: 0,
    decoder: "",
    encoder: "",
    egress: {
      codec: "h264",
      bitrate: 15000000,
      gop_seconds: 1,
      bframes: 0,
      profile: "high",
      level: "4.2",
      preset: "veryfast",
      tune: "zerolatency",
      threads: 0,
      nvenc_preset: "p4",
      nvenc_tune: "ull",
      service_name: "SRTGW",
      provider: "mxl-srt-gateway",
      program_number: 1,
      pcr_ms: 40,
      mux: "cbr",
      pmt_pid: 4096,
      video_pid: 256,
      read_offset_grains: 2,
    },
  };
}

export const creating = reactive({ draft: null }); // { value } of the channel being created

export function startNew(direction) {
  if (!creating.draft || creating.draft.value.direction !== direction) creating.draft = { value: toForm(blankChannel(direction)) };
  selectChannel(NEW);
}
export function discardNew() {
  creating.draft = null;
  selectChannel(live.channels[0]?.id || "");
}

/** POST /api/v1/channels. An empty id gets the next free chN; an empty listener port the next free one. */
export async function createChannel() {
  const body = toBody(creating.draft.value);
  if (!body.id) delete body.id;
  if (!body.label) body.label = body.id || "";
  // A new egress starts with one stereo AAC track, as the gateway's own egress default.
  if (body.direction === "egress") body.egress.audio_preset = "stereo";
  const result = await act(() => api.post("/api/v1/channels", body), (r) => `${r.id}: created.`);
  if (result) {
    creating.draft = null;
    storeChannel(result);
    syncDraft(result.id, toForm(result));
    syncMatrix(result.id, matrixOf(result));
    selectChannel(result.id);
  }
  return result;
}

// ---- audio matrix drafts -----------------------------------------------------------
// Ingest: the routes of the first audio output (decoded track/channel to MXL channel, with gain
// and mute per tap). Egress: the audio tracks (codec, layout, MXL channels, language, gain, mute).

export function matrixOf(channel) {
  const output = channel.audio_outputs?.[0] || { channels: 16, routes: [] };
  const channels = Math.max(2, Math.min(64, Number(output.channels) || 16));
  const routes = clone(output.routes || []).slice(0, channels);
  while (routes.length < channels) routes.push([]);
  return { channels, routes, tracks: clone(channel.egress?.audio_tracks || []) };
}

export const matrixDrafts = reactive({}); // channel id -> { value, base, ui }

function syncMatrix(id, fresh) {
  const d = matrixDrafts[id];
  const c = canon(fresh);
  if (!d || canon(d.value) === d.base) {
    if (!d || d.base !== c) matrixDrafts[id] = { value: fresh, base: c, ui: d?.ui || { gains: {}, mutes: {} } };
  } else {
    d.base = c;
  }
}
export const isMatrixDirty = (id) => {
  const d = matrixDrafts[id];
  return !!d && canon(d.value) !== d.base;
};
export function revertMatrix(id) {
  const channel = live.channels.find((c) => c.id === id);
  if (channel) matrixDrafts[id] = { value: matrixOf(channel), base: canon(matrixOf(channel)), ui: { gains: {}, mutes: {} } };
}

/** PUT /api/v1/channels/{id}/matrix with the part the channel's direction uses. */
export async function applyMatrix(id) {
  const d = matrixDrafts[id];
  const channel = live.channels.find((c) => c.id === id);
  if (!d || !channel) return undefined;
  const body =
    channel.direction === "egress"
      ? { egress: { audio_tracks: d.value.tracks } }
      : { audio_outputs: [{ channels: d.value.channels, preset: "custom", routes: d.value.routes }] };
  const result = await act(() => api.put(`/api/v1/channels/${encodeURIComponent(id)}/matrix`, body), `${id}: audio saved. Only this channel restarted.`);
  if (result) {
    storeChannel(result);
    revertMatrix(id);
  }
  return result;
}

// ---- egress route (POST /api/v1/channels/{id}/route) ------------------------------------
// An edit is kept per channel until it is sent or dropped.

export const routeEdits = reactive({}); // channel id -> { video_domain, video_flow, audio_domain, audio_flow, active }

export function routeOf(id) {
  const r = statusById.value[id]?.route || {};
  return { video_domain: r.video_domain || "", video_flow: r.video_flow || "", audio_domain: r.audio_domain || "", audio_flow: r.audio_flow || "", active: Boolean(r.video_active) };
}
export async function applyRoute(id) {
  const r = routeEdits[id];
  if (!r) return undefined;
  const body = { ...r, video_active: r.active, audio_active: r.active };
  delete body.active;
  const result = await act(() => api.post(`/api/v1/channels/${encodeURIComponent(id)}/route`, body), `${id}: MXL source set.`);
  if (result) delete routeEdits[id];
  return result;
}

// ---- settings (configuration) -------------------------------------------------------

export const settingsEdits = reactive({}); // key -> new value (string)
export const importDraft = reactive({ text: "" });

/** PUT /api/v1/config with the changed keys. LOG_LEVEL applies at once, the others after a restart. */
export async function saveSettings() {
  const keys = Object.keys(settingsEdits);
  const result = await act(() => api.put("/api/v1/config", { ...settingsEdits }));
  if (result) {
    live.config = result;
    for (const key of keys) delete settingsEdits[key];
    const later = keys.filter((k) => k !== "LOG_LEVEL");
    live.notice = `Saved ${keys.join(", ")}.${later.length ? " Restart the gateway to apply " + later.join(", ") + "." : ""}`;
  }
  return result;
}

/** POST /api/v1/config/import: a JSON document (as exported) or KEY=value lines. */
export async function importConfig(text) {
  const json = text.trim().startsWith("{");
  const result = await api.post("/api/v1/config/import", text, json ? "application/json" : "text/plain");
  live.config = result;
  await refreshChannels();
  return result;
}

// ---- live connection -----------------------------------------------------------------

// The server pushes every channel's status four times a second. Merging it into the existing
// objects keeps the tiles in place.
function applyStatus(payload) {
  if (!payload || !payload.channels) return;
  const byId = new Map(live.status.map((c) => [c.id, c]));
  const next = payload.channels.map((incoming) => {
    const existing = byId.get(incoming.id);
    if (!existing) return incoming;
    Object.assign(existing, incoming);
    return existing;
  });
  if (next.length !== live.status.length || next.some((c, i) => c !== live.status[i])) live.status = next;
}

export async function refreshChannels() {
  try {
    const body = await api.get("/api/v1/channels");
    live.channels = body.channels || [];
    for (const channel of live.channels) {
      syncDraft(channel.id, toForm(channel));
      syncMatrix(channel.id, matrixOf(channel));
    }
    for (const id of Object.keys(drafts)) {
      if (!live.channels.some((c) => c.id === id) && !isDirty(id)) delete drafts[id];
    }
    live.error = "";
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
}

async function refreshStatus() {
  try {
    applyStatus(await api.get("/api/v1/status"));
    live.error = "";
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
}

export async function refreshConfig() {
  try {
    live.config = await api.get("/api/v1/config");
  } catch {
    /* keep the last answer; the connection banner tells */
  }
}

export async function refreshDomains() {
  try {
    live.domains = (await api.get("/api/v1/domains")).domains || [];
  } catch {
    /* keep the last answer */
  }
}

async function refreshInfo() {
  try {
    live.info = await api.get("/api/v1/info");
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
}

async function refreshSlow() {
  live.ready = await probe("/readyz");
  try {
    live.nmos = await api.get("/api/v1/nmos");
  } catch {
    /* keep the last answer */
  }
  refreshConfig();
  refreshChannels();
  refreshDomains();
}

let socket = null;
let retry = 0;
let timers = [];

function connect() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${proto}//${location.host}/api/v1/events`);
  socket.onopen = () => {
    live.connected = true;
    live.everConnected = true;
    live.error = "";
    refreshInfo(); // the process may have restarted with another version or file
  };
  socket.onmessage = (ev) => {
    try {
      applyStatus(JSON.parse(ev.data));
    } catch {
      /* a broken frame is dropped; the next one replaces it */
    }
  };
  socket.onclose = () => {
    live.connected = false;
    retry = setTimeout(connect, 1000);
  };
}

/** Loads info, status, channels, NMOS, domains, configuration and readiness, then follows the WebSocket; polls while it is down. */
export async function startLive() {
  await refreshInfo();
  await Promise.all([refreshChannels(), refreshStatus()]);
  refreshSlow();
  connect();
  timers = [
    setInterval(() => {
      if (!live.connected) refreshStatus();
    }, 2000),
    // Configuration, registration, readiness and the domains change without a push.
    setInterval(refreshSlow, 5000),
    setInterval(() => view.tick++, 1000),
  ];
}

export function stopLive() {
  timers.forEach(clearInterval);
  clearTimeout(retry);
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
}
