<template>
  <div class="status-bar">
    <div class="status-left">
      <span class="status-item">
        <span class="status-icon" :class="connectionStatus">●</span>
        {{ statusMessage }}
      </span>
      <span class="status-item" v-if="additionalInfo">
        {{ additionalInfo }}
      </span>
    </div>
    <div class="status-right">
      <span class="status-item">
        {{ currentTime }}
      </span>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

const props = defineProps({
  statusMessage: {
    type: String,
    default: 'Ready'
  },
  currentTime: {
    type: String,
    required: true
  },
  connectionStatus: {
    type: String,
    default: 'disconnected'
  },
  additionalInfo: {
    type: String,
    default: ''
  }
})
</script>

<style scoped>
.status-bar {
  height: 22px;
  background: #f0f0f0;
  border-top: 1px solid #ccc;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 8px;
  font-size: 11px;
  color: #666;
  flex-shrink: 0;
}

.status-left {
  display: flex;
  align-items: center;
  gap: 16px;
}

.status-right {
  display: flex;
  align-items: center;
  gap: 8px;
}

.status-item {
  display: flex;
  align-items: center;
  gap: 4px;
}

.status-icon {
  font-size: 8px;
  line-height: 1;
}

.status-icon.connected {
  color: #4CAF50;
}

.status-icon.disconnected {
  color: #f44336;
}

.status-icon.connecting {
  color: #FF9800;
}
</style>