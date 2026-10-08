<script setup>
// The SRT side of a channel (§5.0, §5.1, §5.7): mode, addresses and ports, stream ids, latency and
// packet options, encryption and access. The passphrase is write-only: the API only says whether
// one is set.
import { computed } from "vue";
import Segmented from "./Segmented.vue";

const props = defineProps({
  ep: { type: Object, required: true }, // the form (edited in place)
  orig: { type: Object, default: null }, // the applied form, for the changed-field marks
  direction: { type: String, required: true },
  idp: { type: String, required: true }, // id prefix of the inputs
  portRange: { type: String, default: "" },
});

const MODES = computed(() => [
  {
    value: "caller",
    label: "Caller",
    title: props.direction === "egress" ? "The gateway connects to a remote listener and pushes the stream" : "The gateway connects to a remote sender and pulls the stream",
  },
  {
    value: "listener",
    label: "Listener",
    title: props.direction === "egress" ? "A remote caller connects to this port and pulls the stream" : "A remote caller connects to this port and pushes the stream",
  },
  { value: "rendezvous", label: "Rendezvous", title: "Both sides call each other on the same port" },
]);
const KEYS = [
  { value: 0, label: "Auto", title: "AES-256 for internet exposure, else AES-128 when a passphrase is set" },
  { value: 16, label: "AES-128" },
  { value: 24, label: "AES-192" },
  { value: 32, label: "AES-256" },
];
const EXPOSURE = [
  { value: "internal", label: "Internal", title: "WAN or lab network" },
  { value: "internet", label: "Internet", title: "Needs a passphrase and, as a listener, a stream-id allow-list" },
];

const listens = computed(() => props.ep.mode !== "caller");
const calls = computed(() => props.ep.mode !== "listener");
const cls = (k) => ({ dirty: props.orig && props.ep[k] !== props.orig[k] });
</script>

<template>
  <div class="row tight">
    <Segmented v-model="ep.mode" :options="MODES" label="SRT mode" />
    <span class="muted small">{{ MODES.find((m) => m.value === ep.mode)?.title }}</span>
  </div>
  <div class="fields grid">
    <div v-if="calls">
      <label :for="`${idp}-rhost`">Remote host</label>
      <input :id="`${idp}-rhost`" v-model.trim="ep.remote_host" :class="cls('remote_host')" placeholder="host or IP" />
    </div>
    <div v-if="calls">
      <label :for="`${idp}-rport`">Remote port</label>
      <input :id="`${idp}-rport`" v-model.number="ep.remote_port" type="number" min="1" max="65535" :class="cls('remote_port')" />
    </div>
    <div>
      <label :for="`${idp}-laddr`">Local address{{ listens ? "" : " (optional)" }}</label>
      <input :id="`${idp}-laddr`" v-model.trim="ep.local_address" :class="cls('local_address')" placeholder="all interfaces" />
    </div>
    <div>
      <label :for="`${idp}-lport`">Local port{{ listens ? ` (${portRange || "SRT_PORT_RANGE"}; 0: next free)` : " (0: any)" }}</label>
      <input :id="`${idp}-lport`" v-model.number="ep.local_port" type="number" min="0" max="65535" :class="cls('local_port')" />
    </div>
    <div>
      <label :for="`${idp}-lat`">Latency in ms</label>
      <input :id="`${idp}-lat`" v-model.number="ep.latency_ms" type="number" min="20" max="10000" step="10" :class="cls('latency_ms')" />
    </div>
    <div v-if="calls">
      <label :for="`${idp}-cto`">Connect timeout in ms</label>
      <input :id="`${idp}-cto`" v-model.number="ep.connect_timeout_ms" type="number" min="100" step="100" :class="cls('connect_timeout_ms')" />
    </div>
    <div v-if="ep.mode === 'caller'">
      <label :for="`${idp}-sid`">Stream ID (sent)</label>
      <input :id="`${idp}-sid`" v-model.trim="ep.streamid" :class="cls('streamid')" />
    </div>
    <div v-if="ep.mode === 'listener'" style="grid-column: span 2">
      <label :for="`${idp}-acc`">Accepted stream IDs (empty: any{{ ep.exposure === "internet" ? "; required for internet" : "" }})</label>
      <input :id="`${idp}-acc`" v-model="ep.accepted_streamids" :class="cls('accepted_streamids')" placeholder="news-1, news-2" />
    </div>
  </div>

  <div class="group-caption">Encryption and access</div>
  <div class="choices">
    <div>
      <label>Exposure</label>
      <Segmented v-model="ep.exposure" :options="EXPOSURE" label="Exposure" />
    </div>
    <div>
      <label>Key length</label>
      <Segmented v-model="ep.pbkeylen" :options="KEYS" label="Key length" />
    </div>
  </div>
  <div class="fields grid">
    <div>
      <label :for="`${idp}-pass`">Passphrase (write-only)</label>
      <input
        :id="`${idp}-pass`"
        v-model="ep.passphrase"
        type="password"
        autocomplete="new-password"
        :class="{ dirty: ep.passphrase }"
        :disabled="ep.clear_passphrase"
        :placeholder="ep.passphrase_set ? 'set; empty keeps it' : 'none'"
      />
      <label v-if="ep.passphrase_set" class="check"><input v-model="ep.clear_passphrase" type="checkbox" /> Remove the passphrase</label>
    </div>
    <div style="grid-column: span 2">
      <label :for="`${idp}-allow`">Allowed peers (IPs or CIDR ranges; empty: any)</label>
      <input :id="`${idp}-allow`" v-model="ep.peer_allow" :class="cls('peer_allow')" placeholder="10.0.0.0/8, 192.0.2.7" />
    </div>
  </div>

  <div class="group-caption">Packets</div>
  <div class="fields grid">
    <div>
      <label :for="`${idp}-payload`">Payload size in bytes</label>
      <input :id="`${idp}-payload`" v-model.number="ep.payload_size" type="number" min="100" max="1456" :class="cls('payload_size')" />
    </div>
    <div>
      <label :for="`${idp}-oh`">Overhead in % (retransmits)</label>
      <input :id="`${idp}-oh`" v-model.number="ep.overhead" type="number" min="5" max="100" :class="cls('overhead')" />
    </div>
    <div>
      <label :for="`${idp}-maxbw`">Max bandwidth in bytes/s (0: none)</label>
      <input :id="`${idp}-maxbw`" v-model.number="ep.max_bandwidth" type="number" min="0" :class="cls('max_bandwidth')" />
    </div>
  </div>
</template>
