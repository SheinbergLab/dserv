<template>
  <div class="panel-container">
    <!-- Query Panel -->
    <QueryPanel 
      v-if="activePanel === 'query'"
      :is-connected="isConnected"
    />
    
    <!-- Subscribe Panel -->
    <SubscribePanel 
      v-if="activePanel === 'subscribe'"
      :is-connected="isConnected"
    />
    
    <!-- DataFrame Panel -->
    <DataFramePanel 
      v-if="activePanel === 'dataframe'"
      :is-connected="isConnected"
    />
    
    <!-- Monitor Panel -->
    <MonitorPanel 
      v-if="activePanel === 'monitor'"
      :is-connected="isConnected"
    />
    
    <!-- Log Panel -->
    <LogPanel 
      v-if="activePanel === 'log'"
      :is-connected="isConnected"
    />
    
    <!-- Default/Welcome Panel -->
    <WelcomePanel 
      v-if="!activePanel || !isValidPanel"
      :connection-status="connectionStatus"
      @connect="$emit('connect')"
    />
  </div>
</template>

<script setup>
import { computed } from 'vue'

// Import panel components
import QueryPanel from './QueryPanel.vue'
import SubscribePanel from './SubscribePanel.vue'
import DataFramePanel from './DataFramePanel.vue'
import MonitorPanel from './MonitorPanel.vue'
import LogPanel from './LogPanel.vue'
import WelcomePanel from './WelcomePanel.vue'

const props = defineProps({
  activePanel: {
    type: String,
    default: 'query'
  },
  isConnected: {
    type: Boolean,
    default: false
  },
  connectionStatus: {
    type: String,
    default: 'disconnected'
  }
})

defineEmits(['connect'])

// Valid panel names
const validPanels = ['query', 'subscribe', 'dataframe', 'monitor', 'log']

const isValidPanel = computed(() => {
  return validPanels.includes(props.activePanel)
})
</script>

<style scoped>
.panel-container {
  height: 100%;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
</style>