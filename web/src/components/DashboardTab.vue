<script setup>
// Dashboard (SPECIFICATION.md §7): every channel with its state, picture, SRT statistics, codec,
// frame sync and audio meters.
import { inject } from "vue";
import ChannelTile from "./ChannelTile.vue";
import { live, startNew } from "../store.js";

const go = inject("go");
function create(direction) {
  startNew(direction);
  go("channels");
}
</script>

<template>
  <div class="toolbar">
    <span class="caption">Channels</span>
    <span class="muted small">Ingest: SRT → MXL · Egress: MXL → SRT. Click a picture for the channel's settings.</span>
    <span class="spacer"></span>
    <button class="btn secondary" @click="create('ingest')">New ingest</button>
    <button class="btn secondary" @click="create('egress')">New egress</button>
  </div>
  <div v-if="!live.channels.length" class="panel empty">No channels yet. Create an ingest (SRT → MXL) or an egress (MXL → SRT) channel.</div>
  <div v-else class="tiles">
    <ChannelTile v-for="c in live.channels" :key="c.id" :channel="c" />
  </div>
</template>
