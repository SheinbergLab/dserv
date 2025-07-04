<!-- SubscribePanel.vue -->
<template>
  <div class="panel">
    <div class="panel-header">
      <span>Variable Subscriptions</span>
      <div class="panel-controls">
        <button class="panel-btn" title="Clear All">🗑️</button>
        <button class="panel-btn" title="Refresh">🔄</button>
      </div>
    </div>
    <div class="panel-content">
      <div class="coming-soon">
        <h3>🚧 Subscribe Panel</h3>
        <p>Variable subscription management coming soon...</p>
        <ul>
          <li>Subscribe to real-time variable updates</li>
          <li>Manage subscription intervals</li>
          <li>View active subscriptions</li>
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
