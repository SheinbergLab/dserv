<template>
  <main-layout />
</template>

<script setup>
import { onMounted } from 'vue'
import MainLayout from './layouts/MainLayout.vue'
import { dserv } from './services/dserv.js'
import { initializeErrorTracking } from './services/errorService.js'

onMounted(async () => {
  // Initialize global error tracking
  try {
    await initializeErrorTracking(dserv)
    console.log('Global error tracking initialized')
  } catch (error) {
    console.error('Failed to initialize error tracking:', error)
  }

  window.dserv = dserv

})
</script>

<style>
body {
  margin: 0;
  padding: 0;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
  font-size: 13px;
}

#app {
  height: 100vh;
  overflow: hidden;
}

/* Global overrides for compact desktop interface */
.ant-layout-sider {
  background: #fafafa !important;
}

.ant-form-item-label > label {
  font-size: 12px;
  font-weight: 500;
}

.ant-select-single .ant-select-selector {
  height: 24px;
  line-height: 22px;
}

.ant-select-single .ant-select-selection-item {
  line-height: 22px;
  font-size: 12px;
}

.ant-btn-sm {
  height: 24px;
  line-height: 22px;
  font-size: 11px;
}

.ant-tabs-small .ant-tabs-tab {
  padding: 4px 12px;
  font-size: 12px;
}

.ant-tag {
  font-size: 11px;
  line-height: 18px;
  padding: 0 6px;
}

/* Make progress circles more compact */
.ant-progress-circle {
  font-size: 10px;
}

/* Compact table styling */
.ant-table-small .ant-table-tbody > tr > td {
  padding: 4px 8px;
  font-size: 11px;
}

.ant-table-small .ant-table-thead > tr > th {
  padding: 4px 8px;
  font-size: 11px;
  font-weight: 500;
  background: #f5f5f5;
}

/* Compact form styling */
.ant-form-item {
  margin-bottom: 8px;
}

.ant-form-item-label {
  padding-bottom: 2px;
}

/* Make tabs more compact */
.ant-tabs-content-holder {
  overflow: auto;
}

/* Ensure layout doesn't scroll */
.ant-layout {
  overflow: hidden;
}

.ant-layout-content {
  overflow: auto;
}
</style>
