<template>
  <div class="status-bar">
    <div class="status-message">{{ statusMessage }}</div>
    <div class="time-display">{{ currentTime }}</div>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'

// Props
defineProps({
  statusMessage: {
    type: String,
    default: 'Ready'
  }
})

// Time display
const currentTime = ref('')
let timer = null

function updateTime() {
  const now = new Date()
  currentTime.value = now.toLocaleString()
}

onMounted(() => {
  updateTime()
  timer = setInterval(updateTime, 1000)
})

onUnmounted(() => {
  clearInterval(timer)
})
</script>

<style scoped>
.status-bar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 0 16px;
  height: 24px;
  background: #f0f2f5;
  border-top: 1px solid #d9d9d9;
  font-size: 12px;
}
.status-message {
  color: #555;
}
.time-display {
  color: #555;
}
</style>
