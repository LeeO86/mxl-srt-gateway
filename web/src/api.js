// REST client for the gateway API (SPECIFICATION.md §7, README) and display helpers.

async function request(path, { method = "GET", body, text = false, type } = {}) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = type || "application/json";
  const resp = await fetch(path, {
    method,
    headers,
    body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
    cache: "no-store",
  });
  const raw = await resp.text();
  let data = raw;
  if (!text) {
    try {
      data = raw ? JSON.parse(raw) : null;
    } catch {
      data = raw;
    }
  }
  if (!resp.ok) {
    const reason = data && typeof data === "object" ? data.error : String(data || "").slice(0, 200);
    throw new Error(reason || `HTTP ${resp.status}`);
  }
  return data;
}

export const api = {
  get: (path) => request(path),
  text: (path) => request(path, { text: true }),
  post: (path, body, type) => request(path, { method: "POST", body: body ?? {}, type }),
  put: (path, body) => request(path, { method: "PUT", body }),
  del: (path) => request(path, { method: "DELETE" }),
};

/** HTTP status and JSON body of a probe (/livez, /readyz) without throwing; status 0 when it does not answer. */
export async function probe(path) {
  try {
    const resp = await fetch(path, { cache: "no-store" });
    let body = null;
    try {
      body = await resp.json();
    } catch {
      /* not JSON */
    }
    return { status: resp.status, body };
  } catch {
    return { status: 0, body: null };
  }
}

/** The first 8 characters of an id; the whole id goes into a title. */
export const shortId = (id) => (id ? String(id).slice(0, 8) : "–");

export const clone = (v) => JSON.parse(JSON.stringify(v));

/** JSON with sorted keys, to compare a draft with the configuration. */
export function canon(value) {
  return JSON.stringify(value, (key, v) =>
    v && typeof v === "object" && !Array.isArray(v) ? Object.fromEntries(Object.keys(v).sort().map((k) => [k, v[k]])) : v,
  );
}

/** Copies text; falls back to a hidden textarea on plain http, where navigator.clipboard is missing. */
export async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const area = document.createElement("textarea");
    area.value = text;
    area.style.position = "fixed";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand("copy");
    area.remove();
    return ok;
  }
}

export function download(name, text, type = "application/json") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

/** Channel states (SPECIFICATION.md §7) as words, and their pill colour. */
export const STATE_TEXT = { idle: "idle", connecting: "connecting", waiting: "waiting", no_signal: "no signal", running: "running", error: "error" };
export const stateKind = (state) => ({ running: "ok", connecting: "warn", waiting: "warn", no_signal: "warn", error: "bad" })[state] || "neutral";

export function fmtBitrate(bps) {
  if (!Number.isFinite(bps) || bps <= 0) return "–";
  if (bps >= 1e6) return `${(bps / 1e6).toFixed(2)} Mbit/s`;
  return `${Math.round(bps / 1e3)} kbit/s`;
}

export function fmtBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "–";
  if (bytes >= 1 << 30) return `${(bytes / (1 << 30)).toFixed(2)} GiB`;
  return `${Math.round(bytes / (1 << 20))} MiB`;
}

export const num = (v, digits = 0) => (Number.isFinite(v) ? v.toFixed(digits) : "–");

/** Linear peak (0–1) to dBFS. */
export const toDb = (v) => (v > 0 ? 20 * Math.log10(v) : -120);

/** One line for an MXL flow from GET /api/v1/domains (format and size or channels). */
export function flowDetail(flow) {
  if (flow.channel_count) return `${flow.channel_count} ch audio`;
  if (flow.frame_width) {
    const rate = flow.grain_rate ? flow.grain_rate.numerator / (flow.grain_rate.denominator || 1) : 0;
    // Interlaced flows declare the frame rate; the name carries the field rate (1080i50).
    const interlaced = (flow.interlace_mode || "progressive").startsWith("interlaced");
    const shown = interlaced ? rate * 2 : rate;
    return `${flow.frame_width}×${flow.frame_height}${interlaced ? "i" : "p"}${shown ? Math.round(shown * 100) / 100 : ""}`;
  }
  return flow.media_type || "";
}

/**
 * Per-channel numbers from /metrics (Prometheus text, prefix mxl_srt_gateway_), keyed by channel id,
 * and the process numbers (resident bytes, CPU seconds, NVENC/NVDEC sessions, GPU memory) under "".
 */
export function parseMetrics(text) {
  const out = { "": {} };
  for (const line of text.split("\n")) {
    const m = /^mxl_srt_gateway_(\w+)(?:\{([^}]*)\})? (\S+)$/.exec(line);
    if (!m) continue;
    const labels = Object.fromEntries([...(m[2] || "").matchAll(/(\w+)="([^"]*)"/g)].map((x) => [x[1], x[2]]));
    if (labels.le) continue;
    const entry = labels.channel ? (out[labels.channel] ||= {}) : out[""];
    entry[m[1]] = Number(m[3]);
  }
  return out;
}
