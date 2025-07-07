<template>
  <div class="title-bar">
    <div class="title-bar-text">
      <span class="app-icon">{{ appIcon }}</span>
      {{ appName }}
    </div>
    <div class="title-bar-controls">
      <button 
        class="title-bar-control minimize" 
        @click="$emit('minimize')"
        title="Minimize"
      >
        −
      </button>
      <button 
        class="title-bar-control maximize" 
        @click="$emit('maximize')"
        title="Maximize"
      >
        □
      </button>
      <button 
        class="title-bar-control close" 
        @click="$emit('close')"
        title="Close"
      >
        ×
      </button>
    </div>
  </div>
</template>

<script setup>
defineProps({
  appName: {
    type: String,
    required: true
  },
  appIcon: {
    type: String,
    default: '📡'
  }
})

defineEmits(['minimize', 'maximize', 'close'])
</script>

<style scoped>
.title-bar {
  height: 30px;
  background: linear-gradient(to bottom, #f8f8f8, #e8e8e8);
  border-bottom: 1px solid #999;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 8px;
  user-select: none;
  flex-shrink: 0;
}

.title-bar-text {
  font-weight: 500;
  display: flex;
  align-items: center;
  gap: 6px;
}

.app-icon {
  font-size: 16px;
}

.title-bar-controls {
  display: flex;
  gap: 2px;
}

.title-bar-control {
  width: 20px;
  height: 20px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  font-size: 12px;
  font-weight: bold;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 2px;
}

.title-bar-control:hover {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.title-bar-control.close:hover {
  background: #ff6b6b;
  color: white;
}
</style>