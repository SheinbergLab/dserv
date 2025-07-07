<!-- DataFramePanel.vue -->
<template>
  <div class="panel">
    <div class="panel-header">
      <span>Data Frames</span>
      <div class="panel-controls">
        <button class="panel-btn" title="Export">💾</button>
        <button class="panel-btn" title="Refresh">🔄</button>
      </div>
    </div>
    <div class="panel-content">
      <div class="coming-soon">
        <h3>📊 DataFrame Panel</h3>
        <p>DataFrame visualization and analysis coming soon...</p>
        <ul>
          <li>View stimdg and trialdg dataframes</li>
          <li>Data analysis and filtering</li>
          <li>Export functionality</li>
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