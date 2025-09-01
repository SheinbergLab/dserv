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
      <div class="debug-line">Aspect Mode: {{ aspectRatioMode }}</div>
      <div class="debug-line">Backend Ratio: {{ backendAspectRatio ? backendAspectRatio.toFixed(3) : 'N/A' }}</div>
      <div class="debug-line">Connected: {{ isConnected ? 'YES' : 'NO' }}</div>
      <div class="debug-line">Last Update: {{ lastUpdate || 'Never' }}</div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'
import { useGraphicsRenderer } from '@/composables/useGraphicsRenderer'

const props = defineProps({
  width: { type: Number, default: null }, // null = responsive
  height: { type: Number, default: null }, // null = responsive
  streamId: { type: String, required: true },
  showStats: { type: Boolean, default: false },
  showDebug: { type: Boolean, default: false },
  canvasClass: { type: String, default: 'graphics-canvas' },
  autoScale: { type: Boolean, default: true },

  // Legacy aspect ratio prop (for backward compatibility)
  aspectRatio: { type: Number, default: null }, // 4/3, 16/9, etc.

  // New aspect ratio mode system
  aspectRatioMode: {
    type: String,
    default: 'auto', // 'auto' | 'container' | 'backend' | 'fixed'
    validator: value => ['auto', 'container', 'backend', 'fixed'].includes(value)
  },
  fixedAspectRatio: { type: Number, default: null }, // for 'fixed' mode

  // Size constraints
  minWidth: { type: Number, default: 320 },
  minHeight: { type: Number, default: 240 },
  maxWidth: { type: Number, default: 1920 },
  maxHeight: { type: Number, default: 1080 },
  enableInputDatapoints: { type: Boolean, default: false }
})

const emit = defineEmits(['mousemove', 'click', 'canvasReady', 'resize', 'aspectRatioChanged'])

// Refs
const canvasRef = ref(null)
const containerRef = ref(null)
const canvasWidth = ref(props.width || 640)
const canvasHeight = ref(props.height || 480)
const containerSize = ref({ width: 0, height: 0 })

// Backend aspect ratio tracking
const backendAspectRatio = ref(null)
const sourceWindowBounds = ref(null)

// Resize observer
let resizeObserver = null

// Determine effective aspect ratio mode
const effectiveAspectRatioMode = computed(() => {
  // Handle legacy aspectRatio prop
  if (props.aspectRatio && props.aspectRatioMode === 'auto') {
    return 'fixed'
  }

  // Handle auto mode
  if (props.aspectRatioMode === 'auto') {
    return backendAspectRatio.value ? 'backend' : 'container'
  }

  return props.aspectRatioMode
})

// Get target aspect ratio based on current mode
const getTargetAspectRatio = () => {
  const mode = effectiveAspectRatioMode.value

  switch (mode) {
    case 'backend':
      return backendAspectRatio.value || props.fixedAspectRatio

    case 'fixed':
      return props.aspectRatio || props.fixedAspectRatio

    case 'container':
    default:
      return null // Use container aspect ratio
  }
}

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

  // Get target aspect ratio
  const targetAspectRatio = getTargetAspectRatio()

  // Apply aspect ratio constraints if specified
  if (targetAspectRatio) {
    const containerAspect = width / height
    if (containerAspect > targetAspectRatio) {
      // Container is wider than target aspect - constrain width
      width = Math.floor(height * targetAspectRatio)
    } else {
      // Container is taller than target aspect - constrain height
      height = Math.floor(width / targetAspectRatio)
    }
  }

  // Apply min/max constraints
  width = Math.max(props.minWidth, Math.min(props.maxWidth, width))
  height = Math.max(props.minHeight, Math.min(props.maxHeight, height))

  // Only update if size actually changed
  if (width !== canvasWidth.value || height !== canvasHeight.value) {
    const mode = effectiveAspectRatioMode.value
    console.log(`Canvas resizing: ${canvasWidth.value}×${canvasHeight.value} → ${width}×${height}`)
    console.log(`Mode: ${mode}, Target Aspect: ${targetAspectRatio?.toFixed(3) || 'none'}`)

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

    emit('resize', { width, height, aspectRatio: width / height })
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

// Initialize graphics renderer with backend aspect ratio capture
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
  autoScale: props.autoScale,

  // Callback for when backend sends setwindow command
  onWindowBoundsChange: (windowBounds) => {
    if (windowBounds) {
      sourceWindowBounds.value = windowBounds
      const sourceWidth = windowBounds.urx - windowBounds.llx
      const sourceHeight = windowBounds.ury - windowBounds.lly
      const newAspectRatio = sourceWidth / sourceHeight

      if (Math.abs(newAspectRatio - (backendAspectRatio.value || 0)) > 0.001) {
        console.log(`Backend aspect ratio changed: ${backendAspectRatio.value?.toFixed(3) || 'none'} → ${newAspectRatio.toFixed(3)}`)
        backendAspectRatio.value = newAspectRatio

        emit('aspectRatioChanged', {
          aspectRatio: newAspectRatio,
          windowBounds: windowBounds
        })

        // Trigger canvas resize if in backend mode
        if (effectiveAspectRatioMode.value === 'backend') {
          nextTick(() => calculateCanvasSize())
        }
      }
    }
  }
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

  // Existing emit for parent components
  emit('click', { canvasX, canvasY, normalizedX: canvasX, normalizedY: canvasY, originalEvent: event })

  // Send click datapoint directly as TCL dict
  if (props.enableInputDatapoints) {
    const inputString = `{x ${canvasX} y ${Math.floor(canvasHeight.value - canvasY)} w ${canvasWidth.value} h ${canvasHeight.value}}`;

    dserv.essCommand(`dservSet ${props.streamId}/input "${inputString}"`)
      .catch(error => console.error('Failed to send click datapoint:', error))
  }
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

// Force recalculate canvas size (useful when aspect ratio mode changes)
const recalculateCanvasSize = () => {
  calculateCanvasSize()
}

// Watch for aspect ratio mode changes
watch(() => effectiveAspectRatioMode.value, (newMode) => {
  console.log(`Aspect ratio mode changed to: ${newMode}`)
  calculateCanvasSize()
})

// Expose methods for parent components
defineExpose({
  resize,
  renderData,
  canvasRef,
  recalculateCanvasSize,
  getCanvasSize: () => ({ width: canvasWidth.value, height: canvasHeight.value }),
  getBackendAspectRatio: () => backendAspectRatio.value,
  getSourceWindowBounds: () => sourceWindowBounds.value,
  getEffectiveAspectRatioMode: () => effectiveAspectRatioMode.value
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
