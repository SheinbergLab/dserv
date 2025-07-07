<!-- LogPanel.vue -->
<template>
  <div class="panel">
    <div class="panel-header">
      <span>Live Data Log</span>
      <div class="panel-controls">
        <input 
          type="text" 
          placeholder="Filter..." 
          class="filter-input"
        />
        <button class="panel-btn" title="Clear Log">🗑️</button>
      </div>
    </div>
    <div class="panel-content">
      <div class="coming-soon">
        <h3>📄 Log Panel</h3>
        <p>Live data logging and filtering coming soon...</p>
        <ul>
          <li>Real-time variable updates</li>
          <li>Filtering and search</li>
          <li>Export log data</li>
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

<style scoped>
.panel {
  height: 100%;
  display: flex;
  flex-direction: column;
}

.panel-header {
  height: 28px;
  background: linear-gradient(to bottom, #f0f0f0, #e0e0e0);
  border-bottom: 1px solid #ccc;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 8px;
  font-weight: 500;
  font-size: 12px;
}

.panel-controls {
  display: flex;
  gap: 4px;
  align-items: center;
}

.panel-btn {
  width: 20px;
  height: 20px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  font-size: 12px;
  cursor: pointer;
  border-radius: 2px;
}

.panel-btn:hover {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.filter-input {
  width: 80px;
  padding: 2px 4px;
  border: 1px solid #999;
  font-size: 10px;
  margin-right: 4px;
}

.panel-content {
  flex: 1;
  padding: 20px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.coming-soon {
  text-align: center;
  color: #666;
  max-width: 400px;
}

.coming-soon h3 {
  margin-bottom: 16px;
  font-size: 18px;
  color: #333;
}

.coming-soon p {
  margin-bottom: 16px;
  font-size: 14px;
  line-height: 1.5;
}

.coming-soon ul {
  text-align: left;
  display: inline-block;
  margin-top: 16px;
}

.coming-soon li {
  margin: 8px 0;
  font-size: 13px;
}
</style>