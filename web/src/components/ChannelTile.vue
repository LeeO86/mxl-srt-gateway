<script setup>
// One channel on the dashboard: state, thumbnail (about once a second), SRT statistics, codec and
// frame sync numbers, meters, alarms. A click on the picture opens its settings.
import { computed, inject, ref } from "vue";
import Meters from "./Meters.vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, fmtBitrate, num, stateKind } from "../api.js";
import { selectChannel, statusById, view } from "../store.js";

const props = defineProps({ channel: { type: Object, required: true } });
const go = inject("go");

const s = computed(() => statusById.value[props.channel.id] || { state: "idle", srt: {}, meters: [], alarms: [] });
const state = computed(() => (props.channel.enabled ? s.value.state : "idle"));
const egress = computed(() => props.channel.direction === "egress");
const shown = ref(false);

const stats = computed(() => {
  const srt = s.value.srt || {};
  const list = [
    { k: "Peer", v: s.value.peer || "–", title: s.value.peer },
    { k: "RTT", v: srt.connected ? `${num(srt.rtt_ms, 1)} ms` : "–" },
    { k: egress.value ? "Send rate" : "Receive rate", v: fmtBitrate(srt.bitrate_bps) },
    { k: "Lost / retrans", v: `${srt.loss ?? 0} / ${srt.retrans ?? 0}`, title: "packets lost / retransmitted since the connection" },
    { k: "Dropped", v: srt.drop ?? 0, title: "packets dropped (too late)" },
    { k: "SRT buffer", v: srt.connected ? `${num(srt.buffer_ms)} ms` : "–" },
  ];
  if (egress.value) {
    list.push(
      { k: "Encoder", v: s.value.encoder || "–", title: s.value.encoder },
      { k: "Encode", v: s.value.encode_fps ? `${num(s.value.encode_fps, 1)} fps` : "–" },
      { k: "Repeat / drop", v: `${s.value.repeats ?? 0} / ${s.value.drops ?? 0}`, title: "MXL grains repeated / skipped" },
    );
  } else {
    list.push(
      { k: "Decoder", v: s.value.decoder || "–", title: s.value.decoder },
      { k: "Repeat / drop", v: `${s.value.repeats ?? 0} / ${s.value.drops ?? 0}`, title: "frame synchroniser repeats / drops" },
      { k: "Audio FIFO", v: `${num(s.value.audio_fifo_ms)} ms`, title: `drift ${num(s.value.audio_drift_ppm, 1)} ppm` },
    );
  }
  return list;
});

function open(tab) {
  selectChannel(props.channel.id);
  go(tab);
}
</script>

<template>
  <div class="panel tile">
    <h3>
      <span class="name" :title="channel.id">{{ channel.label || channel.id }}</span>
      <span class="badge">{{ channel.direction }}</span>
      <span class="spacer"></span>
      <Pill v-if="!channel.enabled" text="disabled" kind="neutral" />
      <Pill v-else :text="STATE_TEXT[state] || state" :kind="stateKind(state)" />
    </h3>
    <div class="picture" title="Open the channel's settings" @click="open('channels')">
      <img :src="`${s.thumbnail || `/api/v1/channels/${encodeURIComponent(channel.id)}/thumbnail`}?t=${view.tick}`" alt="" :style="{ opacity: shown ? 1 : 0 }" @load="shown = true" @error="shown = false" />
      <div v-if="!shown" class="none">{{ channel.enabled ? "no picture yet" : "disabled" }}</div>
      <span class="ov tl">{{ s.source_format || "–" }} → {{ s.target_format || "–" }}</span>
      <span v-if="s.failover_active" class="ov tr" :title="egress ? 'the second destination is connected' : 'the backup input is on air'">{{ egress ? "copy connected" : "backup on air" }}</span>
      <span v-if="s.timecode" class="ov br mono">{{ s.timecode }}</span>
    </div>
    <div class="stats">
      <div v-for="x in stats" :key="x.k" :title="x.title"><b>{{ x.k }}</b>{{ x.v }}</div>
    </div>
    <Meters v-if="!egress" :peaks="s.meters || []" :label="`${channel.label} MXL audio levels`" />
    <p v-if="s.error" class="alarm bad">{{ s.error }}</p>
    <p v-for="a in s.alarms || []" :key="a" class="alarm">{{ a }}</p>
    <div class="actions" style="margin-top: 0.6rem">
      <button class="btn small secondary" @click="open('audio')">Audio</button>
      <button class="btn small secondary" @click="open('channels')">Settings</button>
    </div>
  </div>
</template>
