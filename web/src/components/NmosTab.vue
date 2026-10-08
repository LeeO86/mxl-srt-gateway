<script setup>
// NMOS and MXL (SPECIFICATION.md §4, §7): the node and its registration, every sender, flow and
// receiver (ingest channels send, egress channels receive) with what is routed, and the MXL
// domains and flows this host has.
import { computed } from "vue";
import IdCode from "./IdCode.vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, flowDetail, stateKind } from "../api.js";
import { live, statusById } from "../store.js";

const n = computed(() => live.nmos);
const values = computed(() => live.config || {});
const base = computed(() => (n.value ? `http://${live.info?.host_address || location.hostname}:${n.value.port}` : ""));
const registration = computed(() => {
  if (!n.value?.built) return { text: "NMOS not built", kind: "bad" };
  if (!n.value.registry) return { text: "no registry", kind: "neutral" };
  return live.ready?.body?.registered ? { text: "registered", kind: "ok" } : { text: "not registered", kind: "warn" };
});
const rows = computed(() =>
  (n.value?.resources || []).map((r) => {
    const status = statusById.value[r.channel];
    const route = status?.route || {};
    const video = !r.label.endsWith(" audio"); // receivers are "<label> video" and "<label> audio"
    return {
      ...r,
      state: status?.state || "idle",
      routeFlow: r.kind === "receiver" ? (video ? route.video_flow : route.audio_flow) : "",
      routeDomain: r.kind === "receiver" ? (video ? route.video_domain : route.audio_domain) : "",
      active: r.kind === "receiver" ? (video ? route.video_active : route.audio_active) : null,
      is05: r.kind === "sender" || r.kind === "receiver" ? `${base.value}/x-nmos/connection/v1.1/single/${r.kind}s/${r.id}/active` : "",
    };
  }),
);
// This gateway's domain first, then by label; flows video, audio, data, then by label.
const KIND = (f) => ["video", "audio", "data"].findIndex((k) => (f.format || "").endsWith(`:${k}`));
const domains = computed(() =>
  [...live.domains]
    .sort((a, b) => Number(b.own) - Number(a.own) || (a.label || a.id).localeCompare(b.label || b.id))
    .map((d) => ({ ...d, flows: [...(d.flows || [])].sort((a, b) => KIND(a) - KIND(b) || (a.label || "").localeCompare(b.label || "")) })),
);
</script>

<template>
  <div v-if="!n" class="empty">Loading…</div>
  <template v-else>
    <div class="grid two">
      <div class="panel">
        <h3>Node <span class="spacer"></span><Pill :text="registration.text" :kind="registration.kind" /></h3>
        <dl class="kv">
          <dt>Label</dt>
          <dd>{{ live.info?.label || "–" }}</dd>
          <dt>Node id</dt>
          <dd><code>{{ n.node_id }}</code></dd>
          <dt>Device</dt>
          <dd>{{ n.device_label }} <code>{{ n.device_id }}</code></dd>
          <dt>Address</dt>
          <dd>{{ live.info?.host_address || "–" }}:{{ n.port }} (node and connection API; WebSocket {{ n.port + 1 }})</dd>
          <dt>Registry</dt>
          <dd>
            {{ n.registry ? `${n.registry}:${n.registry_port}` : "none: not registered" }}
            <span v-if="n.registry" class="muted">(query {{ values.NMOS_QUERY_ADDRESS }}:{{ values.NMOS_QUERY_PORT }})</span>
          </dd>
          <dt>DNS-SD</dt>
          <dd>{{ n.dns_sd ? "on" : "off" }}</dd>
          <dt>Seed</dt>
          <dd><code>{{ values.NMOS_SEED || "–" }}</code></dd>
          <dt>Node</dt>
          <dd><Pill :text="n.running ? 'running' : 'not running'" :kind="n.running ? 'ok' : 'bad'" /> <span v-if="n.error" class="muted small">{{ n.error }}</span></dd>
        </dl>
        <div class="note">
          Raw resources: <a :href="`${base}/x-nmos/node/v1.3/self`" target="_blank" rel="noopener">self</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/senders`" target="_blank" rel="noopener">senders</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/receivers`" target="_blank" rel="noopener">receivers</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/flows`" target="_blank" rel="noopener">flows</a> ·
          <a :href="`${base}/x-nmos/connection/v1.1/single`" target="_blank" rel="noopener">connection</a>
        </div>
      </div>
      <div class="panel">
        <h3>How routing works</h3>
        <p class="note" style="margin-top: 0">
          An <b>ingest</b> channel writes one video and one audio flow per audio output into this gateway's MXL domain, each with a BCP-007-03
          sender (transport <code>urn:x-nmos:transport:mxl</code>) grouped as <code>&lt;label&gt;:Video</code> and <code>:Audio &lt;n&gt;</code>.
          A new output format makes a new flow id; the sender follows.
        </p>
        <p class="note">
          An <b>egress</b> channel has a video and an audio receiver. A controller activates a sender's <code>mxl_domain_id</code> and
          <code>mxl_flow_id</code> with IS-05; a flow that is not there yet makes the channel wait (slate) until it appears. The egress can also be
          routed on the Channels tab without IS-05. Routes are kept across a restart.
        </p>
        <p class="note">The SRT side is set on the Channels tab, not with NMOS.</p>
      </div>
    </div>

    <div class="panel">
      <h3>Senders, flows and receivers</h3>
      <table>
        <thead>
          <tr><th>Channel</th><th>Kind</th><th>Label</th><th>Id</th><th>State</th><th>Routed flow</th><th>Domain</th><th></th></tr>
        </thead>
        <tbody>
          <tr v-for="r in rows" :key="r.kind + r.id">
            <td>{{ r.channel }}</td>
            <td>{{ r.kind }}</td>
            <td>{{ r.label }}</td>
            <td><IdCode :id="r.id" /></td>
            <td><Pill :text="STATE_TEXT[r.state] || r.state" :kind="stateKind(r.state)" /></td>
            <td>
              <template v-if="r.kind === 'receiver'">
                <IdCode :id="r.routeFlow" />
                <span v-if="r.routeFlow" class="pill" :class="r.active ? 'ok' : 'neutral'">{{ r.active ? "active" : "inactive" }}</span>
              </template>
            </td>
            <td><IdCode v-if="r.kind === 'receiver'" :id="r.routeDomain" /></td>
            <td class="small"><a v-if="r.is05" :href="r.is05" target="_blank" rel="noopener">IS-05</a></td>
          </tr>
        </tbody>
      </table>
      <p v-if="!rows.length" class="empty">No channel, so no sender or receiver.</p>
      <p class="note">Hover an id for all of it.</p>
    </div>

    <div class="panel">
      <h3>MXL domains on this host <span class="muted small" style="text-transform: none; letter-spacing: 0">{{ values.MXL_DOMAIN_SCAN_PATH }}</span></h3>
      <table>
        <thead>
          <tr><th>Domain</th><th>Id</th><th>Flow</th><th>Flow id</th><th>Format</th></tr>
        </thead>
        <tbody>
          <template v-for="dm in domains" :key="dm.id">
            <tr>
              <td :rowspan="Math.max(1, dm.flows.length)">
                {{ dm.label || "–" }}
                <span v-if="dm.own" class="pill ok">this gateway</span>
                <span v-if="dm.mirror" class="pill neutral">mirror</span>
                <div class="desc mono">{{ dm.path }}</div>
              </td>
              <td :rowspan="Math.max(1, dm.flows.length)"><IdCode :id="dm.id" /></td>
              <template v-if="dm.flows.length">
                <td>{{ dm.flows[0].label || "–" }}</td>
                <td><IdCode :id="dm.flows[0].id" /></td>
                <td>{{ flowDetail(dm.flows[0]) }}</td>
              </template>
              <td v-else colspan="3" class="muted">no flow</td>
            </tr>
            <tr v-for="f in dm.flows.slice(1)" :key="f.id">
              <td>{{ f.label || "–" }}</td>
              <td><IdCode :id="f.id" /></td>
              <td>{{ flowDetail(f) }}</td>
            </tr>
          </template>
        </tbody>
      </table>
      <p v-if="!domains.length" class="empty">No MXL domain under {{ values.MXL_DOMAIN_SCAN_PATH }}.</p>
      <p class="note">Every directory with a <code>domain_def.json</code>; fabrics mirrors are marked. Egress channels read from these.</p>
    </div>
  </template>
</template>
