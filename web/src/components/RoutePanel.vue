<script setup>
// The MXL source of an egress channel: what IS-05 (or the API) routed to its receivers, and a
// picker over the flows on this host (GET /api/v1/domains) for POST /api/v1/channels/{id}/route.
import { computed } from "vue";
import IdCode from "./IdCode.vue";
import Pill from "./Pill.vue";
import { flowDetail } from "../api.js";
import { applyRoute, live, routeEdits, routeOf } from "../store.js";

const props = defineProps({ id: { type: String, required: true } });

const current = computed(() => routeOf(props.id));
// A first route starts active: picking a flow should be enough.
const edit = computed(() => routeEdits[props.id] || { ...current.value, active: current.value.active || !(current.value.video_flow || current.value.audio_flow) });
const dirty = computed(() => Boolean(routeEdits[props.id]));

const flows = (kind) =>
  live.domains
    .flatMap((d) =>
      (d.flows || [])
        .filter((f) => (f.format ? f.format.endsWith(`:${kind}`) : (f.media_type || "").startsWith(`${kind}/`)))
        .map((f) => ({ value: `${d.id}|${f.id}`, label: `${d.label || d.id.slice(0, 8)} › ${f.label || f.id.slice(0, 8)} (${flowDetail(f)})${d.mirror ? " · mirror" : ""}` })),
    )
    .sort((a, b) => a.label.localeCompare(b.label));
function options(kind) {
  const list = flows(kind);
  const value = `${edit.value[`${kind}_domain`]}|${edit.value[`${kind}_flow`]}`;
  if (edit.value[`${kind}_flow`] && !list.some((o) => o.value === value)) list.unshift({ value, label: `${edit.value[`${kind}_flow`].slice(0, 8)}… (not on this host now)` });
  return list;
}
const selected = (kind) => (edit.value[`${kind}_flow`] ? `${edit.value[`${kind}_domain`]}|${edit.value[`${kind}_flow`]}` : "");
function pick(kind, value) {
  const [domain, flow] = value ? value.split("|") : ["", ""];
  routeEdits[props.id] = { ...edit.value, [`${kind}_domain`]: domain, [`${kind}_flow`]: flow };
}
function setActive(active) {
  routeEdits[props.id] = { ...edit.value, active };
}
const domainLabel = (id) => live.domains.find((d) => d.id === id)?.label;
const flowLabel = (domain, id) => live.domains.find((d) => d.id === domain)?.flows?.find((f) => f.id === id)?.label;
</script>

<template>
  <div class="panel">
    <h3>
      MXL source
      <span class="spacer"></span>
      <Pill v-if="!current.video_flow && !current.audio_flow" text="not routed" kind="neutral" />
      <Pill v-else :text="current.active ? 'active' : 'inactive'" :kind="current.active ? 'ok' : 'neutral'" title="master_enable of the route" />
    </h3>
    <dl class="kv">
      <dt>Video</dt>
      <dd>
        <template v-if="current.video_flow">{{ flowLabel(current.video_domain, current.video_flow) || "" }} <IdCode :id="current.video_flow" /></template>
        <span v-else class="muted">not routed</span>
      </dd>
      <dt>Audio</dt>
      <dd>
        <template v-if="current.audio_flow">{{ flowLabel(current.audio_domain, current.audio_flow) || "" }} <IdCode :id="current.audio_flow" /></template>
        <span v-else class="muted">not routed</span>
      </dd>
      <dt>Domain</dt>
      <dd>{{ domainLabel(current.video_domain) || "" }} <IdCode :id="current.video_domain" /></dd>
    </dl>
    <div class="sub">
      <label :for="`route-v-${id}`">Video flow</label>
      <select :id="`route-v-${id}`" :value="selected('video')" :class="{ dirty: dirty && edit.video_flow !== current.video_flow }" @change="pick('video', $event.target.value)">
        <option value="">(keep the routed one)</option>
        <option v-for="o in options('video')" :key="o.value" :value="o.value">{{ o.label }}</option>
      </select>
      <label :for="`route-a-${id}`">Audio flow</label>
      <select :id="`route-a-${id}`" :value="selected('audio')" :class="{ dirty: dirty && edit.audio_flow !== current.audio_flow }" @change="pick('audio', $event.target.value)">
        <option value="">(keep the routed one)</option>
        <option v-for="o in options('audio')" :key="o.value" :value="o.value">{{ o.label }}</option>
      </select>
      <label class="check"><input type="checkbox" :checked="edit.active" @change="setActive($event.target.checked)" /> Active (master_enable)</label>
      <p class="note">
        Usually an NMOS controller routes this channel with IS-05 (NMOS tab). This sets the route directly; the next IS-05 activation replaces it.
        A flow that is not there yet makes the channel wait (slate) until it appears.
      </p>
      <div class="actions" style="margin-top: 0.3rem">
        <button class="btn secondary" :disabled="!dirty" @click="delete routeEdits[id]">Revert</button>
        <button class="btn" :disabled="!dirty || !(edit.video_flow || edit.audio_flow)" @click="applyRoute(id)">Set source</button>
      </div>
    </div>
  </div>
</template>
