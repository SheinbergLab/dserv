<template>
  <div class="graphics-canvas-container" ref="containerRef">
    <canvas
      ref="canvasRef"
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

    <div v-if="showDebug" class="debug-panel">
      <div class="debug-line">Canvas: {{ canvasWidth }}×{{ canvasHeight }}</div>
      <div class="debug-line">Container: {{ containerSize.width }}×{{ containerSize.height }}</div>
      <div class="debug-line">Stream: {{ streamId }}</div>
      <div class="debug-line">Auto-scale: {{ autoScale ? 'ON' : 'OFF' }}</div>
      <div class="debug-line">Connected: {{ isConnected ? 'YES' : 'NO' }}</div>
      <div class="debug-line">Last Update: {{ lastUpdate || 'Never' }}</div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick } from 'vue'
import { useGraphicsRenderer } from '@/composables/useGraphicsRenderer'

const props = defineProps({
  width: { type: Number, default: null }, // null = responsive
  height: { type: Number, default: null }, // null = responsive
  streamId: { type: String, required: true },
  showStats: { type: Boolean, default: false },
  showDebug: { type: Boolean, default: false },
  canvasClass: { type: String, default: 'graphics-canvas' },
  autoScale: { type: Boolean, default: true },
  aspectRatio: { type: Number, default: null }, // 4/3, 16/9, etc.
  minWidth: { type: Number, default: 320 },
  minHeight: { type: Number, default: 240 },
  maxWidth: { type: Number, default: 1920 },
  maxHeight: { type: Number, default: 1080 }
})

const emit = defineEmits(['mousemove', 'click', 'canvasReady', 'resize'])

// Refs
const canvasRef = ref(null)
const containerRef = ref(null)
const canvasWidth = ref(props.width || 640)
const canvasHeight = ref(props.height || 480)
const containerSize = ref({ width: 0, height: 0 })

// Resize observer
let resizeObserver = null

// Calculate responsive canvas size
const calculateCanvasSize = () => {
  if (!containerRef.value) return

  const rect = containerRef.value.getBoundingClientRect()
  containerSize.value = { width: rect.width, height: rect.height }

  // If explicit dimensions provided, use those
  if (props.width && props.height) {
    canvasWidth.value = props.width
    canvasHeight.value = props.height
    return
  }

  // Skip if container has no size
  if (rect.width <= 0 || rect.height <= 0) {
    console.log('Container has no size, skipping resize')
    return
  }

  let width = Math.floor(rect.width)
  let height = Math.floor(rect.height)

  // Apply aspect ratio if specified
  if (props.aspectRatio) {
    const containerAspect = width / height
    if (containerAspect > props.aspectRatio) {
      // Container is wider than desired aspect
      width = Math.floor(height * props.aspectRatio)
    } else {
      // Container is taller than desired aspect
      height = Math.floor(width / props.aspectRatio)
    }
  }

  // Apply min/max constraints
  width = Math.max(props.minWidth, Math.min(props.maxWidth, width))
  height = Math.max(props.minHeight, Math.min(props.maxHeight, height))

  // Only update if size actually changed
  if (width !== canvasWidth.value || height !== canvasHeight.value) {
    console.log(`Canvas resizing: ${canvasWidth.value}×${canvasHeight.value} → ${width}×${height}`)

    canvasWidth.value = width
    canvasHeight.value = height

    // Update actual canvas element
    if (canvasRef.value) {
      canvasRef.value.width = width
      canvasRef.value.height = height
    }

    // Notify renderer of size change
    if (renderer && renderer.resizeCanvas) {
      renderer.resizeCanvas(width, height)
    }

    emit('resize', { width, height })
  }
}

// Setup resize observer
const setupResizeObserver = () => {
  if (!containerRef.value) return

  resizeObserver = new ResizeObserver(() => {
    requestAnimationFrame(calculateCanvasSize)
  })

  resizeObserver.observe(containerRef.value)
}

// Initialize graphics renderer
const {
  renderStats,
  lastUpdate,
  dataReceived,
  isConnected,
  renderData,
  resizeCanvas,
  dispose
} = useGraphicsRenderer(canvasRef, {
  width: canvasWidth.value,
  height: canvasHeight.value,
  streamId: props.streamId,
  autoScale: props.autoScale
})

// Store renderer reference for resizing
let renderer = null

// Handle mouse events with coordinate conversion
const handleMouseMove = (event) => {
  const rect = canvasRef.value.getBoundingClientRect()
  const canvasX = event.clientX - rect.left
  const canvasY = event.clientY - rect.top

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

// Manual resize method (for parent components)
const resize = (width, height) => {
  canvasWidth.value = width
  canvasHeight.value = height
  if (canvasRef.value) {
    canvasRef.value.width = width
    canvasRef.value.height = height
  }
  if (renderer && renderer.resizeCanvas) {
    renderer.resizeCanvas(width, height)
  }
}

// Expose methods for parent components
defineExpose({
  resize,
  renderData,
  canvasRef,
  getCanvasSize: () => ({ width: canvasWidth.value, height: canvasHeight.value })
})

onMounted(async () => {
  await nextTick()

  // Store renderer reference
  renderer = { resizeCanvas }

  // Calculate initial size
  calculateCanvasSize()

  // Setup resize observer
  setTimeout(() => {
    setupResizeObserver()
    calculateCanvasSize() // Recalculate after observer setup
  }, 100)

  emit('canvasReady', {
    canvas: canvasRef.value,
    width: canvasWidth.value,
    height: canvasHeight.value
  })
})

onUnmounted(() => {
  if (resizeObserver) {
    resizeObserver.disconnect()
  }
  if (dispose) {
    dispose()
  }
})
</script>

<style scoped>
.graphics-canvas-container {
  width: 100%;
  height: 100%;
  display: flex;
  justify-content: center;
  align-items: center;
  position: relative;
}

canvas {
  display: block;
  max-width: 100%;
  max-height: 100%;
  border: 1px solid #e5e7eb;
  border-radius: 4px;
  background: white;
  cursor: crosshair;
}

canvas:hover {
  border-color: #3b82f6;
}

.render-stats {
  position: absolute;
  top: 8px;
  left: 8px;
  padding: 4px 8px;
  background: rgba(0, 0, 0, 0.8);
  color: #00ff00;
  font-size: 11px;
  font-family: 'Courier New', monospace;
  border-radius: 3px;
  pointer-events: none;
  z-index: 10;
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
  position: absolute;
  bottom: 8px;
  right: 8px;
  padding: 8px;
  background: rgba(0, 0, 0, 0.8);
  color: #00ff00;
  font-size: 11px;
  font-family: 'Courier New', monospace;
  border-radius: 4px;
  pointer-events: none;
  z-index: 10;
}

.debug-line {
  margin-bottom: 2px;
  white-space: nowrap;
}

.debug-line:last-child {
  margin-bottom: 0;
}
</style>
