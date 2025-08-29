<template>
  <div class="graphics-canvas-container">
    <canvas
      ref="canvasRef"
      :width="canvasWidth"
      :height="canvasHeight"
      :class="canvasClass"
      @mousemove="handleMouseMove"
      @click="handleClick"
    />
    <div v-if="showStats" class="render-stats">
      <div class="stats-line">
        {{ renderStats }}
      </div>
      <div v-if="!isConnected" class="connection-warning">
        ⚠️ Connection Lost
      </div>
      <div v-else-if="!dataReceived" class="waiting-indicator">
        ⏳ Waiting for graphics data on: {{ streamId }}
      </div>
    </div>

    <!-- Debug panel for development -->
    <div v-if="showDebug" class="debug-panel">
      <div class="debug-line">Canvas: {{ canvasWidth }}×{{ canvasHeight }}</div>
      <div class="debug-line">Stream: {{ streamId }}</div>
      <div class="debug-line">Auto-scale: {{ autoScale ? 'ON' : 'OFF' }}</div>
      <div class="debug-line">Connected: {{ isConnected ? 'YES' : 'NO' }}</div>
      <div class="debug-line">Last Update: {{ lastUpdate || 'Never' }}</div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, watch } from 'vue'
import { useGraphicsRenderer } from '@/composables/useGraphicsRenderer'

const props = defineProps({
  width: { type: Number, default: 640 },
  height: { type: Number, default: 480 },
  streamId: { type: String, required: true },
  showStats: { type: Boolean, default: false },
  showDebug: { type: Boolean, default: false },
  canvasClass: { type: String, default: 'graphics-canvas' },
  autoScale: { type: Boolean, default: true },
  resizable: { type: Boolean, default: false }
})

const emit = defineEmits(['mousemove', 'click', 'canvasReady'])

// Canvas ref and reactive dimensions
const canvasRef = ref(null)
const canvasWidth = ref(props.width)
const canvasHeight = ref(props.height)

// Use the graphics renderer composable
const {
  renderStats,
  lastUpdate,
  dataReceived,
  isConnected,
  renderData,
  resizeCanvas,
  config
} = useGraphicsRenderer(canvasRef, {
  width: canvasWidth.value,
  height: canvasHeight.value,
  streamId: props.streamId,
  autoScale: props.autoScale,
  watchSize: props.resizable
})

// Handle mouse events with coordinate conversion
const handleMouseMove = (event) => {
  const rect = canvasRef.value.getBoundingClientRect()
  const canvasX = event.clientX - rect.left
  const canvasY = event.clientY - rect.top

  // Convert to canvas coordinates if needed
  const x = (canvasX / rect.width) * canvasWidth.value
  const y = (canvasY / rect.height) * canvasHeight.value

  emit('mousemove', {
    canvasX,
    canvasY,
    normalizedX: x,
    normalizedY: y,
    originalEvent: event
  })
}

const handleClick = (event) => {
  const rect = canvasRef.value.getBoundingClientRect()
  const canvasX = event.clientX - rect.left
  const canvasY = event.clientY - rect.top

  const x = (canvasX / rect.width) * canvasWidth.value
  const y = (canvasY / rect.height) * canvasHeight.value

  emit('click', {
    canvasX,
    canvasY,
    normalizedX: x,
    normalizedY: y,
    originalEvent: event
  })
}

// Watch for prop changes and update canvas accordingly
watch(() => props.width, (newWidth) => {
  if (newWidth !== canvasWidth.value) {
    canvasWidth.value = newWidth
    if (props.resizable) {
      resizeCanvas(newWidth, canvasHeight.value)
    }
  }
})

watch(() => props.height, (newHeight) => {
  if (newHeight !== canvasHeight.value) {
    canvasHeight.value = newHeight
    if (props.resizable) {
      resizeCanvas(canvasWidth.value, newHeight)
    }
  }
})

// Manual resize method (for parent components)
const resize = (width, height) => {
  canvasWidth.value = width
  canvasHeight.value = height
  resizeCanvas(width, height)
}

// Expose methods for parent components
defineExpose({
  resize,
  renderData,
  canvasRef,
  getCanvasSize: () => ({ width: canvasWidth.value, height: canvasHeight.value })
})

onMounted(() => {
  emit('canvasReady', {
    canvas: canvasRef.value,
    width: canvasWidth.value,
    height: canvasHeight.value
  })
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
  display: block;
}

.graphics-canvas:hover {
  border-color: #3b82f6;
}

.render-stats {
  margin-top: 8px;
  font-size: 0.8rem;
  color: #6b7280;
  text-align: center;
  min-height: 20px;
}

.stats-line {
  margin-bottom: 4px;
}

.connection-warning {
  color: #dc2626;
  font-weight: 500;
}

.waiting-indicator {
  color: #8b5cf6;
  font-style: italic;
}

.debug-panel {
  margin-top: 8px;
  padding: 8px;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 4px;
  font-size: 0.75rem;
  color: #475569;
}

.debug-line {
  margin-bottom: 2px;
  font-family: 'Courier New', monospace;
}

.debug-line:last-child {
  margin-bottom: 0;
}

/* Resizable canvas container */
.graphics-canvas-container.resizable {
  resize: both;
  overflow: auto;
  min-width: 200px;
  min-height: 150px;
  max-width: 1200px;
  max-height: 900px;
}

.graphics-canvas-container.resizable .graphics-canvas {
  width: 100%;
  height: 100%;
  object-fit: contain;
}
</style>
