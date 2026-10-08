<script setup>
// Where a setting comes from (GET /api/v1/config `origin`): ENV (read-only), FILE, or DEFAULT (shown with `all`).
defineProps({
  origin: { type: String, default: "default" },
  all: { type: Boolean, default: false },
});
const ORIGIN = { env: "ENV", file: "FILE", default: "DEFAULT" };
</script>

<template>
  <span v-if="all || origin !== 'default'" class="badge" :class="origin" :title="origin === 'env' ? 'set by an environment variable: read-only' : ''">{{ ORIGIN[origin] || origin }}</span>
</template>
