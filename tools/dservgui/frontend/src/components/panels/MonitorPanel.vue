<!-- MonitorPanel.vue -->
<template>
  <div class="panel">
    <div class="panel-header">
      <span>Position Monitor</span>
      <div class="panel-controls">
        <button class="panel-btn" title="Clear Trail">🧹</button>
        <button class="panel-btn" title="Export">💾</button>
      </div>
    </div>
    <div class="panel-content">
      <div class="coming-soon">
        <h3>📈 Monitor Panel</h3>
        <p>Real-time position monitoring coming soon...</p>
        <ul>
          <li>XY position visualization</li>
          <li>Eye tracking data display</li>
          <li>Trail and grid options</li>
        </ul>
      </div>
    </div>
  </div>
</template>

<script setup>
defineProps({
  isConnected: { type: Boolean, default: false }
})
</script>