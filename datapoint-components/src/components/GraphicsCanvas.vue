<template>
  <div class="graphics-canvas-container">
    <canvas 
      ref="canvasRef"
      :width="width" 
      :height="height"
      :class="canvasClass"
      @mousemove="$emit('mousemove', $event)"
      @click="$emit('click', $event)"
    />
    <div v-if="showStats" class="render-stats">
      {{ renderStats }}
    </div>
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
import { useGraphicsRenderer } from '@/composables/useGraphicsRenderer'

const props = defineProps({
  width: { type: Number, default: 640 },
  height: { type: Number, default: 480 },
  streamId: { type: String, required: true },
  showStats: { type: Boolean, default: false },
  canvasClass: { type: String, default: 'graphics-canvas' }
})

defineEmits(['mousemove', 'click'])

// Canvas ref that will be passed to the composable
const canvasRef = ref(null)

// Use the graphics renderer composable
const { 
  renderStats: rendererStats, 
  lastUpdate,
  dataReceived 
} = useGraphicsRenderer(canvasRef, {
  width: props.width,
  height: props.height,
  streamId: props.streamId,
  autoConnect: true
})

// Compute render stats for display
const renderStats = computed(() => {
  if (!dataReceived.value) {
    return 'Canvas ready, waiting for data...'
  }
  return rendererStats.value
})
</script>

<style scoped>
.graphics-canvas-container {
  display: inline-block;
  position: relative;
}
.graphics-canvas {
  border: 1px solid #e5e7eb;
  border-radius: 4px;
  background: white;
  cursor: crosshair;
}
.render-stats {
  margin-top: 8px;
  font-size: 0.8rem;
  color: #6b7280;
  text-align: center;
}
</style>