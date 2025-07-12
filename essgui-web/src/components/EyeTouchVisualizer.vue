<template>
  <div class="eye-touch-visualizer">
    <!-- Header with status -->
    <div class="visualizer-header">
      <a-row :gutter="12" align="middle">
        <a-col :span="6">
          <div class="window-status-mini">
            <span style="font-size: 11px;">Eye windows:</span>
            <div class="window-indicators-mini">
              <span
                v-for="i in 8"
                :key="i-1"
                class="window-dot"
                :class="{ active: eyeWindows[i-1].active, inside: (eyeWindowStatusMask & (1 << (i-1))) !== 0 }"
              >{{ i-1 }}</span>
            </div>
          </div>
        </a-col>
      </a-row>
    </div>

    <!-- Main canvas area -->
    <div class="canvas-container" ref="canvasContainer">
      <canvas
        ref="canvasRef"
        :width="canvasSize.width"
        :height="canvasSize.height"
        class="visualization-canvas"
      />
      <div class="info-overlay">
        <div class="coordinate-info">
          Raw: {{ eyePositionRaw.x }}, {{ eyePositionRaw.y }}
        </div>
      </div>
    </div>

    <!-- Bottom toolbar -->
    <div class="visualizer-toolbar">
      <a-row align="middle">
        <a-col :span="12">
          <a-space size="small">
            <a-checkbox v-model:checked="displayOptions.showGrid" style="font-size: 11px;">Grid</a-checkbox>
            <a-checkbox v-model:checked="displayOptions.showTrails" style="font-size: 11px;">Trails</a-checkbox>
          </a-space>
        </a-col>
        <a-col :span="12" style="text-align: right;">
          <a-space size="small">
            <a-button
              size="small"
              @click="resetView"
              :icon="h(ReloadOutlined)"
              style="padding: 1px 6px; font-size: 10px;"
            >
              Reset View
            </a-button>
          </a-space>
        </a-col>
      </a-row>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'
import { ReloadOutlined, SyncOutlined } from '@ant-design/icons-vue'
import { h } from 'vue'
import { dserv } from '../services/dserv.js'

// Conversion constants
const ADC_CENTER = 2048
const ADC_TO_DEG = 200.0

// Component state - only component-specific data
const eyePosition = ref({ x: 0, y: 0 })
const eyePositionRaw = ref({ x: 2048, y: 2048 })

// Animation state tracking for robustness
const canvasReady = ref(false)
const animationRunning = ref(false)

// Use dserv's central state for system-wide data
const connected = computed(() => dserv.state.connected)
const systemState = computed(() => {
  const state = dserv.state.status || 'stopped'
  return state.charAt(0).toUpperCase() + state.slice(1)
})
const observationActive = computed(() => dserv.state.inObs)
const observationId = computed(() => dserv.state.obsId + 1) // Display as 1-based
const observationTotal = computed(() => dserv.state.obsTotal)

// Use central state for eye windows
const eyeWindows = computed(() => dserv.state.eyeWindows)
const eyeWindowStatusMask = computed(() => dserv.state.eyeWindowStatusMask)

// Display options
const displayOptions = reactive({
  showGrid: true,
  showTrails: false,
  showLabels: true
})

// Canvas refs and state
const canvasRef = ref(null)
const canvasContainer = ref(null)
const canvasSize = ref({ width: 300, height: 300 })
const visualRange = { horizontal: 20.0, vertical: 20.0 } // degrees

// Trail history
const eyeHistory = ref([])
const maxTrailLength = 50

// Canvas context and animation
let ctx = null
let animationId = null

// Computed properties for canvas
const pixelsPerDegree = computed(() => ({
  x: canvasSize.value.width / visualRange.horizontal,
  y: canvasSize.value.height / visualRange.vertical
}))

const canvasCenter = computed(() => ({
  x: canvasSize.value.width / 2,
  y: canvasSize.value.height / 2
}))

// Utility functions
function convertPosition(xAdc, yAdc) {
  return {
    x: (xAdc - ADC_CENTER) / ADC_TO_DEG,
    y: -1 * (yAdc - ADC_CENTER) / ADC_TO_DEG // Y inverted
  }
}

// Convert degrees to canvas pixels
function degreesToCanvas(degX, degY) {
  return {
    x: canvasCenter.value.x + (degX * pixelsPerDegree.value.x),
    y: canvasCenter.value.y - (degY * pixelsPerDegree.value.y) // Y inverted
  }
}

// Eye position data handler
function handleEyePosition(data) {
  const value = data.value || data.data

  if (data.name === 'ess/em_pos') {
    const [rx, ry, x, y] = value.split(' ').map(Number);
    eyePosition.value = { x, y };
    eyePositionRaw.value = { x: rx, y: ry };

    // Add to trail history if enabled
    if (displayOptions.showTrails) {
      eyeHistory.value.push({ ...eyePosition.value, timestamp: Date.now() })
      if (eyeHistory.value.length > maxTrailLength) {
        eyeHistory.value.shift()
      }
    }
  }
}

// Robust canvas setup
function setupCanvas() {
  if (!canvasRef.value) {
    console.warn('Canvas ref not available')
    return false
  }
  
  try {
    ctx = canvasRef.value.getContext('2d')
    if (!ctx) {
      console.error('Failed to get canvas context')
      return false
    }
    
    canvasReady.value = true
    return true
  } catch (error) {
    console.error('Canvas setup error:', error)
    canvasReady.value = false
    return false
  }
}

// Robust animation system
function startAnimation() {
  // Stop any existing animation first
  if (animationId) {
    cancelAnimationFrame(animationId)
  }
  
  // Ensure canvas is ready
  if (!ctx || !canvasRef.value) {
    return
  }
  
  animationRunning.value = true
  
  function animate() {
    if (!animationRunning.value) {
      return
    }
    
    // Verify canvas context is still valid
    if (!ctx || !canvasRef.value) {
      animationRunning.value = false
      return
    }
    
    render()
    animationId = requestAnimationFrame(animate)
  }
  animate()
}

function stopAnimation() {
  animationRunning.value = false
  if (animationId) {
    cancelAnimationFrame(animationId)
    animationId = null
  }
}

// Enhanced render function with error handling
function render() {
  if (!ctx || !canvasRef.value) {
    return
  }

  try {
    // Clear canvas
    ctx.fillStyle = '#000000'
    ctx.fillRect(0, 0, canvasSize.value.width, canvasSize.value.height)

    // Draw layers in order
    if (displayOptions.showGrid) {
      drawGrid()
    }

    if (displayOptions.showTrails) {
      drawTrails()
    }

    // Draw eye windows
    eyeWindows.value.forEach(window => {
      if (window.active) {
        drawWindow(window)
      }
    })

    // Draw eye position
    drawEyePosition()
  } catch (error) {
    console.error('Render error:', error)
    // Try to restart animation after a brief delay
    setTimeout(() => {
      if (canvasRef.value) {
        ctx = canvasRef.value.getContext('2d')
        if (ctx && !animationRunning.value) {
          startAnimation()
        }
      }
    }, 100)
  }
}

function drawGrid() {
  ctx.strokeStyle = '#333333'
  ctx.lineWidth = 1

  // Vertical lines every 5 degrees
  for (let deg = -10; deg <= 10; deg += 5) {
    const x = canvasCenter.value.x + (deg * pixelsPerDegree.value.x)
    ctx.beginPath()
    ctx.moveTo(x, 0)
    ctx.lineTo(x, canvasSize.value.height)
    ctx.stroke()
  }

  // Horizontal lines every 5 degrees
  for (let deg = -10; deg <= 10; deg += 5) {
    const y = canvasCenter.value.y - (deg * pixelsPerDegree.value.y)
    ctx.beginPath()
    ctx.moveTo(0, y)
    ctx.lineTo(canvasSize.value.width, y)
    ctx.stroke()
  }

  // Center crosshair
  ctx.strokeStyle = '#666666'
  ctx.lineWidth = 2
  ctx.beginPath()
  ctx.moveTo(canvasCenter.value.x - 10, canvasCenter.value.y)
  ctx.lineTo(canvasCenter.value.x + 10, canvasCenter.value.y)
  ctx.moveTo(canvasCenter.value.x, canvasCenter.value.y - 10)
  ctx.lineTo(canvasCenter.value.x, canvasCenter.value.y + 10)
  ctx.stroke()
}

function drawTrails() {
  if (eyeHistory.value.length < 2) return

  ctx.strokeStyle = '#ff0000'
  ctx.lineWidth = 2
  ctx.beginPath()

  for (let i = 0; i < eyeHistory.value.length; i++) {
    const pos = degreesToCanvas(eyeHistory.value[i].x, eyeHistory.value[i].y)

    if (i === 0) {
      ctx.moveTo(pos.x, pos.y)
    } else {
      ctx.lineTo(pos.x, pos.y)
    }
  }

  ctx.globalAlpha = 0.5
  ctx.stroke()
  ctx.globalAlpha = 1.0
}

function drawWindow(window) {
  const pos = degreesToCanvas(window.center.x, window.center.y)
  const size = {
    width: window.size.width * pixelsPerDegree.value.x,
    height: window.size.height * pixelsPerDegree.value.y
  }

  // Color based on state
  const isInside = (eyeWindowStatusMask.value & (1 << window.id)) !== 0
  ctx.strokeStyle = isInside ? '#00ff00' : '#ff0000'
  ctx.lineWidth = isInside ? 3 : 1

  if (window.type === 'ellipse') {
    // Draw ellipse
    ctx.beginPath()
    ctx.ellipse(
      pos.x, pos.y,
      size.width, size.height,
      0, 0, 2 * Math.PI
    )
    ctx.stroke()
  } else {
    // Draw rectangle
    ctx.strokeRect(
      pos.x - size.width / 2,
      pos.y - size.height / 2,
      size.width,
      size.height
    )
  }

  // Draw window ID label if enabled
  if (displayOptions.showLabels) {
    ctx.fillStyle = ctx.strokeStyle
    ctx.font = '12px monospace'
    ctx.fillText(
      `${window.id}`,
      pos.x - size.width / 2 + 2,
      pos.y - size.height / 2 - 5
    )
  }

  // Draw center point
  ctx.fillRect(pos.x - 2, pos.y - 2, 4, 4)
}

function drawEyePosition() {
  const pos = degreesToCanvas(eyePosition.value.x, eyePosition.value.y)

  // Draw white circle with red outline
  ctx.fillStyle = '#ffffff'
  ctx.strokeStyle = '#ff0000'
  ctx.lineWidth = 2

  ctx.beginPath()
  ctx.arc(pos.x, pos.y, 5, 0, 2 * Math.PI)
  ctx.fill()
  ctx.stroke()

  // Draw crosshair
  ctx.strokeStyle = '#ffffff'
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.moveTo(pos.x - 10, pos.y)
  ctx.lineTo(pos.x + 10, pos.y)
  ctx.moveTo(pos.x, pos.y - 10)
  ctx.lineTo(pos.x, pos.y + 10)
  ctx.stroke()
}

// Actions
async function refreshWindows() {
  try {
    for (let i = 0; i < 8; i++) {
      await dserv.essCommand(`ainGetRegionInfo ${i}`)
    }
  } catch (error) {
    console.error('Failed to refresh windows:', error)
  }
}

function resetView() {
  // Clear trails
  eyeHistory.value = []

  // Redraw canvas
  if (ctx) {
    render()
  }
}

// Enhanced resize handling with canvas reinitialization
function handleResize() {
  if (!canvasContainer.value) return

  const rect = canvasContainer.value.getBoundingClientRect()
  const padding = 20
  const size = Math.min(rect.width - padding, rect.height - padding)

  canvasSize.value = {
    width: size,
    height: size
  }

  // Reinitialize canvas after resize
  nextTick(() => {
    if (canvasRef.value && !ctx) {
      setupCanvas()
    }
    
    // Restart animation if it's not running but should be
    if (canvasReady.value && !animationRunning.value) {
      startAnimation()
    }
  })
}

// Component lifecycle
let cleanupDserv = null
let resizeObserver = null

onMounted(() => {
  // Setup canvas first
  nextTick(() => {
    if (setupCanvas()) {
      // Register component
      cleanupDserv = dserv.registerComponent('EyeTouchVisualizer')

      // Listen for events
      dserv.on('datapoint:ess/em_pos', handleEyePosition, 'EyeTouchVisualizer')
      dserv.on('connection', (data) => {
        connected.value = data.connected
      }, 'EyeTouchVisualizer')

      // Initialize connection state
      connected.value = dserv.state.connected

      // Set up resize observer
      resizeObserver = new ResizeObserver(() => {
        handleResize()
      })
      if (canvasContainer.value) {
        resizeObserver.observe(canvasContainer.value)
      }

      // Initial resize and start animation
      handleResize()
      startAnimation()

      // Request initial window info
      if (connected.value) {
        refreshWindows()
      }
    }
  })
})

onUnmounted(() => {
  // Stop animation first
  stopAnimation()

  // Clean up resize observer
  if (resizeObserver) {
    resizeObserver.disconnect()
  }

  // Clean up dserv
  if (cleanupDserv) cleanupDserv()
  
  // Clear canvas state
  canvasReady.value = false
  ctx = null
})

// Restart animation when canvas becomes ready
watch(canvasReady, (ready) => {
  if (ready && !animationRunning.value) {
    startAnimation()
  }
})

// Clear trails when disabled
watch(() => displayOptions.showTrails, (showTrails) => {
  if (!showTrails) {
    eyeHistory.value = []
  }
})

// Expose methods for parent components
defineExpose({
  refreshWindows,
  resetView,
  getEyePosition: () => eyePosition.value,
  getEyePositionRaw: () => eyePositionRaw.value,
  getWindowStates: () => eyeWindows.value,
  getWindowStatusMask: () => eyeWindowStatusMask.value
})
</script>

<style scoped>
.eye-touch-visualizer {
  height: 100%;
  display: flex;
  flex-direction: column;
  background: #f0f2f5;
}

.visualizer-header {
  padding: 12px;
  border-bottom: 1px solid #d9d9d9;
  background: #fafafa;
  flex-shrink: 0;
}

.canvas-container {
  flex: 1;
  overflow: hidden;
  background: #000;
  position: relative;
  display: flex;
  align-items: center;
  justify-content: center;
}

.visualization-canvas {
  display: block;
  border: 1px solid #444;
}

.info-overlay {
  position: absolute;
  top: 10px;
  right: 10px;
  background: rgba(0, 0, 0, 0.7);
  color: white;
  padding: 8px 12px;
  border-radius: 4px;
  font-family: monospace;
  font-size: 11px;
}

.coordinate-info {
  line-height: 1.4;
}

.visualizer-toolbar {
  padding: 8px 12px;
  border-top: 1px solid #d9d9d9;
  background: #fafafa;
  flex-shrink: 0;
  min-height: 40px;
  display: flex;
  align-items: center;
}

.window-status-mini {
  display: flex;
  align-items: center;
  gap: 6px;
}

.window-indicators-mini {
  display: flex;
  gap: 4px;
}

.window-dot {
  width: 20px;
  height: 20px;
  border-radius: 4px;
  background: #333;
  border: 1px solid #666;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 10px;
  color: #999;
  transition: all 0.2s;
}

.window-dot.active {
  border-color: #1890ff;
  color: #fff;
}

.window-dot.inside {
  background: #52c41a;
  border-color: #52c41a;
  color: #fff;
}

/* Statistic value styling */
:deep(.ant-statistic-content-value) {
  font-size: 20px;
  font-family: 'Monaco', 'Menlo', monospace;
}

:deep(.ant-statistic-title) {
  font-size: 12px;
}

/* Checkbox styling */
:deep(.ant-checkbox-wrapper) {
  font-size: 11px;
  line-height: 1.2;
}

:deep(.ant-checkbox) {
  margin-right: 4px;
}
</style>