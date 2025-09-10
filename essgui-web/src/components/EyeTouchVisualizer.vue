<template>
  <div class="eye-touch-visualizer">
    <!-- Header with status -->
    <div class="visualizer-header">
      <div style="display: flex; flex-direction: column; gap: 4px; align-items: flex-end;">
        <div style="display: flex; align-items: center; gap: 8px; justify-content: flex-end;">
          <span style="font-size: 11px; text-align: right;">Eye:</span>
          <div class="window-indicators-mini">
            <span
              v-for="i in 8"
              :key="i-1"
              class="window-dot"
              :class="{ active: eyeWindows[i-1].active, inside: (eyeWindowStatusMask & (1 << (i-1))) !== 0 }"
            >{{ i-1 }}</span>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 8px; justify-content: flex-end;">
          <span style="font-size: 11px; text-align: right;">Touch:</span>
          <div class="window-indicators-mini">
            <span
              v-for="i in 8"
              :key="`touch-${i-1}`"
              class="window-dot touch-dot"
              :class="{ active: touchWindows[i-1]?.active, inside: (touchWindowStatusMask & (1 << (i-1))) !== 0 }"
            >{{ i-1 }}</span>
          </div>
        </div>
      </div>
    </div>

    <!-- Main canvas area -->
    <div class="canvas-container" ref="canvasContainer">
      <canvas
        ref="canvasRef"
        :width="canvasSize.width"
        :height="canvasSize.height"
        class="visualization-canvas"
        @mousedown="handleMouseDown"
        @click="handleCanvasClick"
      />
      <div class="info-overlay">
        <div class="coordinate-info">
          Raw: {{ eyePositionRaw.x }}, {{ eyePositionRaw.y }}
          <div v-if="showTouchPosition && (touchPosition.x !== 0 || touchPosition.y !== 0)">
            Touch: {{ touchPosition.x }}, {{ touchPosition.y }}
          </div>
          <div v-if="virtualMode && virtualInputEnabled">
            Virtual: {{ virtualEye.x.toFixed(2) }}°, {{ virtualEye.y.toFixed(2) }}°
          </div>
        </div>
      </div>
    </div>

    <!-- Bottom toolbar -->
    <div class="visualizer-toolbar">
      <a-row align="middle">
        <a-col :span="24">
          <a-space size="small" style="width: 100%; justify-content: space-between;">
            <div>
              <a-space size="small">
                <a-checkbox v-model:checked="displayOptions.showTrails" style="font-size: 11px;">Trails</a-checkbox>
                <a-checkbox v-model:checked="virtualInputEnabled" style="font-size: 11px;">Virtual Input</a-checkbox>
              </a-space>
            </div>
            <a-button
              v-if="virtualInputEnabled"
              size="small"
              @click="resetVirtualInput"
              :icon="h(ReloadOutlined)"
              style="padding: 1px 6px; font-size: 10px;"
            >
              Reset Virtual
            </a-button>
          </a-space>
        </a-col>
      </a-row>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'
import { ReloadOutlined } from '@ant-design/icons-vue'
import { h } from 'vue'
import { dserv } from '../services/dserv.js'

// Conversion constants
const ADC_CENTER = 2048
const ADC_TO_DEG = 200.0

// Component state - only component-specific data
const eyePosition = ref({ x: 0, y: 0 })
const eyePositionRaw = ref({ x: 2048, y: 2048 })
const touchPosition = ref({ x: 0, y: 0 })
const showTouchPosition = ref(false)

// Virtual input state
const virtualInputEnabled = ref(false)
const virtualMode = ref(false) // true when using virtual input instead of real data
const virtualEye = reactive({
  x: 0,           // degrees
  y: 0,           // degrees
  adcX: ADC_CENTER,
  adcY: ADC_CENTER,
  isDragging: false,
  dragOffset: { x: 0, y: 0 }
})

// Virtual touch state
const virtualTouch = reactive({
  x: 0,           // screen pixels
  y: 0,           // screen pixels
  active: false,
  isDragging: false
})

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

// Use central state for touch windows (with safe defaults)
const touchWindows = computed(() => {
  if (!dserv.state.touchWindows) {
    // Initialize if not present
    dserv.state.touchWindows = Array(8).fill(null).map((_, i) => ({
      id: i,
      active: false,
      state: 0,
      type: 'rectangle',
      center: { x: 0, y: 0 },
      centerRaw: { x: 400, y: 320 },
      size: { width: 2, height: 2 },
      sizeRaw: { width: 100, height: 100 }
    }))
  }
  return dserv.state.touchWindows
})

const touchWindowStatusMask = computed(() => dserv.state.touchWindowStatusMask || 0)

// Screen dimensions from central state (with defaults)
const screenDimensions = computed(() => ({
  width: dserv.state.screenWidth || 800,
  height: dserv.state.screenHeight || 600,
  halfX: dserv.state.screenHalfX || 10.0,
  halfY: dserv.state.screenHalfY || 7.5
}))

// Display options
const displayOptions = reactive({
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

// Convert canvas pixels to degrees
function canvasToDegrees(canvasX, canvasY) {
  return {
    x: (canvasX - canvasCenter.value.x) / pixelsPerDegree.value.x,
    y: -1.0 * (canvasY - canvasCenter.value.y) / pixelsPerDegree.value.y // Y inverted
  }
}

// Convert degrees to ADC values
function degreesToADC(degX, degY) {
  return {
    x: Math.round(degX * ADC_TO_DEG + ADC_CENTER),
    y: Math.round(-degY * ADC_TO_DEG + ADC_CENTER) // Y inverted for ADC
  }
}

// Convert touch screen pixels to degrees
function touchPixelsToDegrees(pixX, pixY) {
  const screen = screenDimensions.value
  const screenPixPerDegX = screen.width / (2 * screen.halfX)
  const screenPixPerDegY = screen.height / (2 * screen.halfY)

  return {
    x: (pixX - screen.width / 2) / screenPixPerDegX,
    y: -1 * (pixY - screen.height / 2) / screenPixPerDegY
  }
}

// Convert degrees to touch screen pixels
function degreesToTouchPixels(degX, degY) {
  const screen = screenDimensions.value
  const screenPixPerDegX = screen.width / (2 * screen.halfX)
  const screenPixPerDegY = screen.height / (2 * screen.halfY)

  return {
    x: Math.round(degX * screenPixPerDegX + screen.width / 2),
    y: Math.round(-degY * screenPixPerDegY + screen.height / 2)
  }
}

// Virtual eye position update and sending
function updateVirtualEyePosition(degX, degY) {
  virtualEye.x = degX
  virtualEye.y = degY

  const adc = degreesToADC(degX, degY)
  virtualEye.adcX = adc.x
  virtualEye.adcY = adc.y

  // Send to dserv
  sendVirtualEyeData()
}

// Send virtual eye data to dserv
async function sendVirtualEyeData() {
  try {
    const cmd = `set d [binary format s2 {${virtualEye.adcY} ${virtualEye.adcX}}]; dservSetData ain/vals 0 4 $d; unset d`
    await dserv.essCommand(cmd)
  } catch (error) {
    console.error('Failed to send virtual eye data:', error)
  }
}

async function sendVirtualTouchData(x, y, eventType = 0) {
  try {
    // Send touch data in the new unified event format
    const cmd = `set d [binary format s3 {${x} ${y} ${eventType}}]; dservSetData mtouch/event 0 6 $d; unset d`
    await dserv.essCommand(cmd)
  } catch (error) {
    console.error('Failed to send virtual touch data:', error)
  }
}

// Mouse event handlers
function handleMouseDown(event) {
  if (!virtualInputEnabled.value) return

  const rect = canvasRef.value.getBoundingClientRect()
  const mouseX = event.clientX - rect.left
  const mouseY = event.clientY - rect.top

  // Check if clicking on virtual eye marker
  const eyePos = degreesToCanvas(virtualEye.x, virtualEye.y)
  const eyeDistance = Math.sqrt(
    Math.pow(mouseX - eyePos.x, 2) + Math.pow(mouseY - eyePos.y, 2)
  )

  if (eyeDistance <= 8 + 5) { // 8px radius + 5px tolerance
    virtualEye.isDragging = true
    virtualEye.dragOffset = {
      x: eyePos.x - mouseX,
      y: eyePos.y - mouseY
    }
    event.preventDefault()
    return
  }
}

function handleMouseMove(event) {
  if (!virtualInputEnabled.value) return

  if (virtualEye.isDragging) {
    const rect = canvasRef.value.getBoundingClientRect()
    const mouseX = event.clientX - rect.left
    const mouseY = event.clientY - rect.top

    // Calculate new position with drag offset
    const newCanvasX = mouseX + virtualEye.dragOffset.x
    const newCanvasY = mouseY + virtualEye.dragOffset.y

    // Convert to degrees and constrain
    const degPos = canvasToDegrees(newCanvasX, newCanvasY)
    const maxX = visualRange.horizontal / 2
    const maxY = visualRange.vertical / 2

    degPos.x = Math.max(-maxX, Math.min(maxX, degPos.x))
    degPos.y = Math.max(-maxY, Math.min(maxY, degPos.y))

    updateVirtualEyePosition(degPos.x, degPos.y)
  }
}

function handleMouseUp() {
  if (virtualEye.isDragging) {
    virtualEye.isDragging = false
  }
}

function handleCanvasClick(event) {
  if (!virtualInputEnabled.value || virtualEye.isDragging) return

  const rect = canvasRef.value.getBoundingClientRect()
  const mouseX = event.clientX - rect.left
  const mouseY = event.clientY - rect.top

  const degPos = canvasToDegrees(mouseX, mouseY)
  const touchPixels = degreesToTouchPixels(degPos.x, degPos.y)

  // Show virtual touch immediately
  virtualTouch.x = touchPixels.x
  virtualTouch.y = touchPixels.y
  virtualTouch.active = true

  // Send press event
  sendVirtualTouchData(touchPixels.x, touchPixels.y, 0) // PRESS

  // Send release event after brief delay to simulate realistic touch
  setTimeout(() => {
    sendVirtualTouchData(touchPixels.x, touchPixels.y, 2) // RELEASE
    virtualTouch.active = false
  }, 100) // Shorter delay since we're not relying on timeouts for display
}

// Data handlers
function handleEyePosition(data) {
  // Skip real eye data if in virtual mode
  if (virtualMode.value && virtualInputEnabled.value) return

  try {
    const value = data.value || data.data

    if (data.name === 'ess/em_pos') {
      if (!value || typeof value !== 'string') {
        console.warn('Invalid ess/em_pos data:', value);
        return;
      }

      const parts = value.split(' ');
      if (parts.length < 4) {
        console.warn('Insufficient ess/em_pos data parts:', parts);
        return;
      }

      const [rx, ry, x, y] = parts.map(Number);

      if (isNaN(rx) || isNaN(ry) || isNaN(x) || isNaN(y)) {
        console.warn('Invalid ess/em_pos coordinates:', { rx, ry, x, y, value });
        return;
      }

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
  } catch (error) {
    console.error('Error handling eye position data:', error, data);
  }
}

function handleTouchEvent(data) {
  try {
    const value = data.value || data.data

    if (data.name === 'mtouch/event') {
      let x, y, eventType;

      if (Array.isArray(value)) {
        if (value.length < 3) {
          console.warn('Insufficient mtouch/event array elements:', value);
          return;
        }
        [x, y, eventType] = value.map(Number);
      } else if (typeof value === 'string') {
        const parts = value.split(' ');
        if (parts.length < 3) {
          console.warn('Insufficient mtouch/event data parts:', parts);
          return;
        }
        [x, y, eventType] = parts.map(Number);
      } else {
        console.warn('Invalid mtouch/event data format (expected array or string):', value);
        return;
      }

      if (isNaN(x) || isNaN(y) || isNaN(eventType)) {
        console.warn('Invalid mtouch/event coordinates:', { x, y, eventType, value });
        return;
      }

      // Handle different event types
      switch (eventType) {
        case 0: // PRESS
          touchPosition.value = { x, y };
          showTouchPosition.value = true;
          break;
          
        case 1: // DRAG
          if (showTouchPosition.value) {
            touchPosition.value = { x, y };
          }
          break;
          
        case 2: // RELEASE
          showTouchPosition.value = false;
          touchPosition.value = { x: 0, y: 0 };
          break;
      }
    }
  } catch (error) {
    console.error('Error handling touch event data:', error, data);
  }
}

// Touch window settings handler
function handleTouchWindowSetting(data) {
  try {
    const value = data.value || data.data

    if (data.name === 'ess/touch_region_setting') {
      if (!value || typeof value !== 'string') {
        console.warn('Invalid ess/touch_region_setting data:', value);
        return;
      }

      const parts = value.split(' ');
      if (parts.length < 8) {
        console.warn('Insufficient ess/touch_region_setting data parts:', parts);
        return;
      }

      const [reg, active, state, type, cx, cy, dx, dy] = parts.map(Number);

      if (parts.some(p => isNaN(Number(p)))) {
        console.warn('Invalid ess/touch_region_setting data:', { reg, active, state, type, cx, cy, dx, dy, value });
        return;
      }

      if (reg >= 0 && reg < 8) {
        if (!dserv.state.touchWindows) {
          dserv.state.touchWindows = Array(8).fill(null).map((_, i) => ({
            id: i,
            active: false,
            state: 0,
            type: 'rectangle',
            center: { x: 0, y: 0 },
            centerRaw: { x: 400, y: 320 },
            size: { width: 2, height: 2 },
            sizeRaw: { width: 100, height: 100 }
          }))
        }

        dserv.state.touchWindows[reg] = {
          id: reg,
          active: active === 1,
          state: state,
          type: type === 1 ? 'ellipse' : 'rectangle',
          center: touchPixelsToDegrees(cx, cy),
          centerRaw: { x: cx, y: cy },
          size: {
            width: Math.abs(dx / screenDimensions.value.width * (2 * screenDimensions.value.halfX)),
            height: Math.abs(dy / screenDimensions.value.height * (2 * screenDimensions.value.halfY))
          },
          sizeRaw: { width: dx, height: dy }
        };
      }
    }
  } catch (error) {
    console.error('Error handling touch window setting:', error, data);
  }
}

function handleTouchWindowStatus(data) {
  try {
    const value = data.value || data.data

    if (data.name === 'ess/touch_region_status') {
      if (!value || typeof value !== 'string') {
        console.warn('Invalid ess/touch_region_status data:', value);
        return;
      }

      const parts = value.split(' ');
      if (parts.length < 4) {
        console.warn('Insufficient ess/touch_region_status data parts:', parts);
        return;
      }

      const [changes, states, touch_x, touch_y] = parts.map(Number);

      if (parts.some(p => isNaN(Number(p)))) {
        console.warn('Invalid ess/touch_region_status data:', { changes, states, touch_x, touch_y, value });
        return;
      }

      if (!dserv.state.touchWindowStatusMask) {
        dserv.state.touchWindowStatusMask = 0;
      }

      const previousMask = dserv.state.touchWindowStatusMask;
      dserv.state.touchWindowStatusMask = states;

      if (!dserv.state.touchWindows) {
        dserv.state.touchWindows = Array(8).fill(null).map((_, i) => ({
          id: i,
          active: false,
          state: 0,
          type: 'rectangle',
          center: { x: 0, y: 0 },
          centerRaw: { x: 400, y: 320 },
          size: { width: 2, height: 2 },
          sizeRaw: { width: 100, height: 100 }
        }))
      }

      // Update window states based on mask
      for (let i = 0; i < 8; i++) {
        dserv.state.touchWindows[i].state = ((states & (1 << i)) !== 0) && dserv.state.touchWindows[i].active;
      }

      // Update touch position if provided (but don't control display state)
      if (touch_x !== undefined && touch_y !== undefined && !isNaN(touch_x) && !isNaN(touch_y)) {
        touchPosition.value = { x: touch_x, y: touch_y };
      }
    }
  } catch (error) {
    console.error('Error handling touch window status:', error, data);
  }
}

// Screen dimension handlers
function handleScreenDimensions(data) {
  const value = data.value || data.data

  switch (data.name) {
    case 'ess/screen_w':
      if (!dserv.state.screenWidth) dserv.state.screenWidth = 800;
      dserv.state.screenWidth = parseInt(value, 10);
      break;
    case 'ess/screen_h':
      if (!dserv.state.screenHeight) dserv.state.screenHeight = 600;
      dserv.state.screenHeight = parseInt(value, 10);
      break;
    case 'ess/screen_halfx':
      if (!dserv.state.screenHalfX) dserv.state.screenHalfX = 10.0;
      dserv.state.screenHalfX = parseFloat(value);
      break;
    case 'ess/screen_halfy':
      if (!dserv.state.screenHalfY) dserv.state.screenHalfY = 7.5;
      dserv.state.screenHalfY = parseFloat(value);
      break;
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
  if (animationId) {
    cancelAnimationFrame(animationId)
  }

  if (!ctx || !canvasRef.value) {
    return
  }

  animationRunning.value = true

  function animate() {
    if (!animationRunning.value) {
      return
    }

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

// Cleaned up render function
function render() {
  if (!ctx || !canvasRef.value) {
    return
  }

  try {
    // Clear canvas
    ctx.fillStyle = '#000000'
    ctx.fillRect(0, 0, canvasSize.value.width, canvasSize.value.height)

    // Draw layers in order
    drawGrid()

    if (displayOptions.showTrails) {
      drawTrails()
    }

    // Draw eye windows
    eyeWindows.value.forEach(window => {
      if (window.active) {
        drawEyeWindow(window)
      }
    })

    // Draw touch windows if enabled
    if (touchWindows.value) {
      touchWindows.value.forEach(window => {
        if (window && window.active) {
          drawTouchWindow(window)
        }
      })
    }

    // Draw eye position (real or virtual)
    if (virtualInputEnabled.value) {
      drawVirtualEye()
    } else {
      drawEyePosition()
    }

    // Draw touch position
    if (showTouchPosition.value && (touchPosition.value.x !== 0 || touchPosition.value.y !== 0)) {
      drawTouchPosition()
    }

    // Draw virtual touch if active
    if (virtualInputEnabled.value && virtualTouch.active) {
      drawVirtualTouch()
    }

  } catch (error) {
    console.error('Render error:', error)
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

// Drawing functions
function drawGrid() {
  ctx.strokeStyle = '#333333'
  ctx.lineWidth = 1

  for (let deg = -10; deg <= 10; deg += 5) {
    const x = canvasCenter.value.x + (deg * pixelsPerDegree.value.x)
    ctx.beginPath()
    ctx.moveTo(x, 0)
    ctx.lineTo(x, canvasSize.value.height)
    ctx.stroke()
  }

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

function drawEyeWindow(window) {
  const pos = degreesToCanvas(window.center.x, window.center.y)
  const size = {
    width: window.size.width * pixelsPerDegree.value.x,
    height: window.size.height * pixelsPerDegree.value.y
  }

  const isInside = (eyeWindowStatusMask.value & (1 << window.id)) !== 0
  ctx.strokeStyle = isInside ? '#00ff00' : '#ff0000'
  ctx.lineWidth = isInside ? 3 : 1

  if (window.type === 'ellipse') {
    ctx.beginPath()
    ctx.ellipse(
      pos.x, pos.y,
      size.width, size.height,
      0, 0, 2 * Math.PI
    )
    ctx.stroke()
  } else {
    ctx.strokeRect(
      pos.x - size.width / 2,
      pos.y - size.height / 2,
      size.width,
      size.height
    )
  }

  if (displayOptions.showLabels) {
    ctx.fillStyle = ctx.strokeStyle
    ctx.font = '12px monospace'
    ctx.fillText(
      `E${window.id}`,
      pos.x - size.width / 2 + 2,
      pos.y - size.height / 2 - 5
    )
  }

  ctx.fillRect(pos.x - 2, pos.y - 2, 4, 4)
}

function drawTouchWindow(window) {
  const pos = degreesToCanvas(window.center.x, window.center.y)
  const size = {
    width: window.size.width * pixelsPerDegree.value.x,
    height: window.size.height * pixelsPerDegree.value.y
  }

  const isInside = (touchWindowStatusMask.value & (1 << window.id)) !== 0
  ctx.strokeStyle = isInside ? '#00ffff' : '#0088aa'
  ctx.lineWidth = isInside ? 3 : 1

  if (window.type === 'ellipse') {
    ctx.beginPath()
    ctx.ellipse(
      pos.x, pos.y,
      size.width, size.height,
      0, 0, 2 * Math.PI
    )
    ctx.stroke()
  } else {
    ctx.strokeRect(
      pos.x - size.width / 2,
      pos.y - size.height / 2,
      size.width,
      size.height
    )
  }

  if (displayOptions.showLabels) {
    ctx.fillStyle = ctx.strokeStyle
    ctx.font = '12px monospace'
    ctx.fillText(
      `T${window.id}`,
      pos.x + size.width / 2 - 15,
      pos.y + size.height / 2 + 15
    )
  }

  ctx.fillStyle = ctx.strokeStyle
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

function drawVirtualEye() {
  const pos = degreesToCanvas(virtualEye.x, virtualEye.y)

  // Draw circle with different styling for virtual mode
  ctx.fillStyle = '#ffffff'
  ctx.strokeStyle = virtualEye.isDragging ? '#00ff00' : '#ff8c00' // Orange for virtual
  ctx.lineWidth = 2

  ctx.beginPath()
  ctx.arc(pos.x, pos.y, 8, 0, 2 * Math.PI) // Slightly larger for easier dragging
  ctx.fill()
  ctx.stroke()

  // Draw crosshair
  ctx.strokeStyle = '#000000'
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.moveTo(pos.x - 6, pos.y)
  ctx.lineTo(pos.x + 6, pos.y)
  ctx.moveTo(pos.x, pos.y - 6)
  ctx.lineTo(pos.x, pos.y + 6)
  ctx.stroke()

  // Draw "V" indicator for virtual
  ctx.fillStyle = '#ff8c00'
  ctx.font = 'bold 10px monospace'
  ctx.fillText('V', pos.x - 3, pos.y - 12)
}

function drawTouchPosition() {
  const touchDegrees = touchPixelsToDegrees(touchPosition.value.x, touchPosition.value.y)
  const pos = degreesToCanvas(touchDegrees.x, touchDegrees.y)

  ctx.fillStyle = '#00ffff'
  ctx.strokeStyle = '#0088aa'
  ctx.lineWidth = 2

  ctx.beginPath()
  ctx.moveTo(pos.x, pos.y - 7)
  ctx.lineTo(pos.x + 7, pos.y)
  ctx.lineTo(pos.x, pos.y + 7)
  ctx.lineTo(pos.x - 7, pos.y)
  ctx.closePath()
  ctx.fill()
  ctx.stroke()
}

function drawVirtualTouch() {
  const touchDegrees = touchPixelsToDegrees(virtualTouch.x, virtualTouch.y)
  const pos = degreesToCanvas(touchDegrees.x, touchDegrees.y)

  // Draw orange diamond for virtual touch
  ctx.fillStyle = '#ff8c00'
  ctx.strokeStyle = '#cc6600'
  ctx.lineWidth = 2

  ctx.beginPath()
  ctx.moveTo(pos.x, pos.y - 9)
  ctx.lineTo(pos.x + 9, pos.y)
  ctx.lineTo(pos.x, pos.y + 9)
  ctx.lineTo(pos.x - 9, pos.y)
  ctx.closePath()
  ctx.fill()
  ctx.stroke()

  // Draw "V" indicator for virtual
  ctx.fillStyle = '#ffffff'
  ctx.font = 'bold 8px monospace'
  ctx.fillText('V', pos.x - 2, pos.y + 2)
}

// Actions
async function refreshWindows() {
  try {
    // Refresh eye windows
    for (let i = 0; i < 8; i++) {
      await dserv.essCommand(`ainGetRegionInfo ${i}`)
    }
    // Refresh touch windows
    for (let i = 0; i < 8; i++) {
      await dserv.essCommand(`touchGetRegionInfo ${i}`)
    }
  } catch (error) {
    console.error('Failed to refresh windows:', error)
  }
}

function resetView() {
  // Clear trails
  eyeHistory.value = []

  // Clear touch position and any pending timeout
  showTouchPosition.value = false
  touchPosition.value = { x: 0, y: 0 }

  // Clear virtual touch
  virtualTouch.active = false

  // Redraw canvas
  if (ctx) {
    render()
  }
}

function resetVirtualInput() {
  // Reset virtual eye to center
  updateVirtualEyePosition(0, 0)

  // Clear virtual touch
  virtualTouch.active = false
  virtualTouch.x = 0
  virtualTouch.y = 0
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

      // Listen for eye events
      dserv.on('datapoint:ess/em_pos', handleEyePosition, 'EyeTouchVisualizer')

      // Listen for touch events
      dserv.on('datapoint:mtouch/event', handleTouchEvent, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/touch_region_setting', handleTouchWindowSetting, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/touch_region_status', handleTouchWindowStatus, 'EyeTouchVisualizer')

      // Listen for screen dimension events
      dserv.on('datapoint:ess/screen_w', handleScreenDimensions, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/screen_h', handleScreenDimensions, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/screen_halfx', handleScreenDimensions, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/screen_halfy', handleScreenDimensions, 'EyeTouchVisualizer')

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

      // Initialize virtual eye at center
      updateVirtualEyePosition(0, 0)
    }
  })

  // Add global mouse event listeners for dragging
  document.addEventListener('mousemove', handleMouseMove)
  document.addEventListener('mouseup', handleMouseUp)
})

onUnmounted(() => {
  // Stop animation first
  stopAnimation()

  // Clean up resize observer
  if (resizeObserver) {
    resizeObserver.disconnect()
  }

  // Remove global event listeners
  document.removeEventListener('mousemove', handleMouseMove)
  document.removeEventListener('mouseup', handleMouseUp)

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

// Watch virtual input mode changes
watch(virtualInputEnabled, (enabled) => {
  if (enabled) {
    // Initialize virtual eye at current real eye position if available
    if (eyePosition.value.x !== 0 || eyePosition.value.y !== 0) {
      updateVirtualEyePosition(eyePosition.value.x, eyePosition.value.y)
    }
    virtualMode.value = true
  } else {
    virtualMode.value = false
    virtualEye.isDragging = false
    virtualTouch.active = false
  }
})

// Expose methods for parent components
defineExpose({
  refreshWindows,
  resetVirtualInput,
  getEyePosition: () => eyePosition.value,
  getEyePositionRaw: () => eyePositionRaw.value,
  getTouchPosition: () => touchPosition.value,
  getWindowStates: () => eyeWindows.value,
  getWindowStatusMask: () => eyeWindowStatusMask.value,
  getTouchWindowStates: () => touchWindows.value,
  getTouchWindowStatusMask: () => touchWindowStatusMask.value,
  getVirtualEyePosition: () => ({ x: virtualEye.x, y: virtualEye.y }),
  getVirtualEyeADC: () => ({ x: virtualEye.adcX, y: virtualEye.adcY }),
  setVirtualInputEnabled: (enabled) => { virtualInputEnabled.value = enabled }
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
  user-select: none;
}

.visualization-canvas.virtual-enabled {
  cursor: pointer;
}

.visualization-canvas.virtual-enabled:active {
  cursor: grabbing;
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
  gap: 4px;
  flex-wrap: wrap;
}

.window-indicators-mini {
  display: flex;
  gap: 2px;
  flex-wrap: wrap;
}

.window-dot {
  width: 18px;
  height: 18px;
  border-radius: 3px;
  background: #333;
  border: 1px solid #666;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 9px;
  color: #999;
  transition: all 0.2s;
  flex-shrink: 0;
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

.window-dot.touch-dot.active {
  border-color: #13c2c2;
}

.window-dot.touch-dot.inside {
  background: #13c2c2;
  border-color: #13c2c2;
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
