<template>
  <div class="toolbar">
    <!-- Connection Controls -->
    <div class="toolbar-group">
      <button 
        class="toolbar-btn" 
        @click="$emit('connect')" 
        :disabled="isConnected"
        title="Connect to dserv"
      >
        <span class="icon">🔌</span>
        Connect
      </button>
      <button 
        class="toolbar-btn" 
        @click="$emit('disconnect')" 
        :disabled="!isConnected"
        title="Disconnect from dserv"
      >
        <span class="icon">❌</span>
        Disconnect
      </button>
    </div>
    
    <div class="toolbar-separator"></div>
    
    <!-- Panel Selection -->
    <div class="toolbar-group">
      <button 
        v-for="panel in panelButtons" 
        :key="panel.name"
        class="toolbar-btn" 
        @click="$emit('panel-change', panel.name)" 
        :class="{ active: activePanel === panel.name }"
        :title="panel.tooltip"
      >
        <span class="icon">{{ panel.icon }}</span>
        {{ panel.label }}
      </button>
    </div>

    <div class="toolbar-spacer"></div>
    
    <!-- Status Display -->
    <div class="status-group">
      <span 
        class="status-indicator" 
        :class="connectionStatus"
        :title="connectionTooltip"
      >
        {{ connectionText }}
      </span>
      <span class="status-text">
        {{ serverInfo }}
      </span>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

const props = defineProps({
  connectionStatus: {
    type: String,
    default: 'disconnected'
  },
  activePanel: {
    type: String,
    default: 'query'
  },
  isConnected: {
    type: Boolean,
    default: false
  },
  serverStatus: {
    type: Object,
    default: () => ({})
  }
})

defineEmits(['connect', 'disconnect', 'panel-change'])

// Panel configuration
const panelButtons = [
  {
    name: 'query',
    label: 'Query',
    icon: '🔍',
    tooltip: 'Query variables'
  },
  {
    name: 'subscribe',
    label: 'Subscribe',
    icon: '📡',
    tooltip: 'Manage subscriptions'
  },
  {
    name: 'dataframe',
    label: 'Data',
    icon: '📊',
    tooltip: 'View dataframes'
  },
  {
    name: 'monitor',
    label: 'Monitor',
    icon: '📈',
    tooltip: 'Position monitor'
  },
  {
    name: 'log',
    label: 'Log',
    icon: '📄',
    tooltip: 'Live data log'
  }
]

// Computed properties
const connectionText = computed(() => {
  return props.isConnected ? 'Connected' : 'Disconnected'
})

const connectionTooltip = computed(() => {
  if (props.isConnected) {
    return 'Connected to dserv server'
  }
  return 'Not connected to dserv server'
})

const serverInfo = computed(() => {
  if (props.serverStatus.callbackPort) {
    return `Port: ${props.serverStatus.callbackPort}`
  }
  return 'Port: ---'
})
</script>

<style scoped>
.toolbar {
  height: 32px;
  background: linear-gradient(to bottom, #f8f8f8, #e8e8e8);
  border-bottom: 1px solid #ccc;
  display: flex;
  align-items: center;
  padding: 0 8px;
  gap: 8px;
  flex-shrink: 0;
}

.toolbar-group {
  display: flex;
  gap: 2px;
}

.toolbar-btn {
  height: 24px;
  padding: 0 8px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  font-size: 11px;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 4px;
  border-radius: 2px;
  user-select: none;
}

.toolbar-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.toolbar-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.toolbar-btn.active {
  background: linear-gradient(to bottom, #d0d0d0, #b0b0b0);
  border-color: #777;
}

.icon {
  font-size: 12px;
}

.toolbar-separator {
  width: 1px;
  height: 20px;
  background: #999;
  margin: 0 4px;
}

.toolbar-spacer {
  flex: 1;
}

.status-group {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 11px;
}

.status-indicator {
  padding: 2px 6px;
  border-radius: 2px;
  font-weight: 500;
  user-select: none;
}

.status-indicator.connected {
  background: #90EE90;
  color: #006400;
}

.status-indicator.disconnected {
  background: #FFB6C1;
  color: #8B0000;
}

.status-indicator.connecting {
  background: #FFE4B5;
  color: #FF8C00;
}

.status-text {
  color: #666;
  font-family: monospace;
}
</style>