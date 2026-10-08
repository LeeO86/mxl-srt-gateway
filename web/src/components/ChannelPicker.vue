<script setup>
// The channel a page edits, with each channel's state; * marks changes not applied. Kept in this browser.
import { computed } from "vue";
import Segmented from "./Segmented.vue";
import { STATE_TEXT } from "../api.js";
import { NEW, creating, live, selectChannel, selectedChannel, statusById } from "../store.js";

const props = defineProps({
  dirty: { type: Function, default: () => false }, // channel id -> bool
  withNew: { type: Boolean, default: false },
});

const options = computed(() => {
  const list = live.channels.map((c) => {
    const state = statusById.value[c.id]?.state || "idle";
    return { value: c.id, label: c.label || c.id, title: `${c.id} · ${c.direction}`, state, dirty: props.dirty(c.id), dir: c.direction === "egress" ? "out" : "in" };
  });
  if (props.withNew && creating.draft) {
    list.push({ value: NEW, label: `New ${creating.draft.value.direction}`, state: "new", dirty: true, dir: "" });
  }
  return list;
});
const current = computed(() => (live.selected === NEW && props.withNew && creating.draft ? NEW : selectedChannel.value?.id));
</script>

<template>
  <div class="group">
    <span class="caption">Channel</span>
    <Segmented :options="options" :model-value="current" label="Channel" @update:model-value="selectChannel">
      <template #default="{ option }">
        <span v-if="option.dir" class="muted small">{{ option.dir }}</span>
        {{ option.label }}<span v-if="option.dirty" title="not applied">*</span><span class="state-tag" :class="option.state">{{ STATE_TEXT[option.state] || option.state }}</span>
      </template>
    </Segmented>
  </div>
</template>
