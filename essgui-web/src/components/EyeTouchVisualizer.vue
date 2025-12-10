<template>
  <div class="eye-touch-visualizer">
    <!-- Header with status -->
    <div class="visualizer-header">
      <div style="display: flex; flex-direction: column; gap: 4px; align-items: flex-end;">
        <div style="display: flex; align-items: center; gap: 8px; justify-content: flex-end;">
          <span style="font-size: 11px; text-align: right;">Eye:</span>
          <div class="window-indicators-mini">
            <span v-for="i in 8" :key="i - 1" class="window-dot"
              :class="{ active: eyeWindows[i - 1].active, inside: (eyeWindowStatusMask & (1 << (i - 1))) !== 0 }">{{ i - 1
              }}</span>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 8px; justify-content: flex-end;">
          <span style="font-size: 11px; text-align: right;">Touch:</span>
          <div class="window-indicators-mini">
            <span v-for="i in 8" :key="`touch-${i - 1}`" class="window-dot touch-dot"
              :class="{ active: touchWindows[i - 1]?.active, inside: (touchWindowStatusMask & (1 << (i - 1))) !== 0 }">{{
              i-1 }}</span>
          </div>
        </div>
      </div>
    </div>

    <!-- Main canvas area -->
    <div class="canvas-container" ref="canvasContainer">
      <canvas ref="canvasRef" :width="canvasSize.width" :height="canvasSize.height" class="visualization-canvas" :class="{
        'virtual-eye-enabled': virtualEyeEnabled,
        'virtual-touch-enabled': virtualTouchEnabled
      }" @mousedown="handleMouseDown" @mousemove="handleMouseMove" @mouseup="handleMouseUp"
        @mouseleave="handleMouseLeave" />
      <div class="info-overlay">
        <div class="coordinate-info">
          Eye: {{ eyePosition.x.toFixed(2) }}°, {{ eyePosition.y.toFixed(2) }}°
          <div v-if="showTouchPosition && (touchPosition.x !== 0 || touchPosition.y !== 0)">
            Touch: {{ touchPosition.x }}, {{ touchPosition.y }}
          </div>
          <div v-if="virtualEyeEnabled && virtualEye.active">
            Virtual Eye: {{ virtualEye.x.toFixed(2) }}°, {{ virtualEye.y.toFixed(2) }}°
          </div>
          <div v-if="virtualTouchEnabled && virtualTouch.active">
            Virtual Touch: {{ virtualTouch.x }}, {{ virtualTouch.y }}
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
                <a-checkbox v-model:checked="virtualEyeEnabled" style="font-size: 11px;">Virtual Eye</a-checkbox>
                <a-checkbox v-model:checked="virtualTouchEnabled" style="font-size: 11px;">Virtual Touch</a-checkbox>
              </a-space>
            </div>
            <a-space size="small">
              <a-button v-if="virtualEyeEnabled" size="small" @click="resetVirtualEye" :icon="h(ReloadOutlined)"
                style="padding: 1px 6px; font-size: 10px;">
                Reset Eye
              </a-button>
              <a-button v-if="virtualTouchEnabled" size="small" @click="resetVirtualTouch" :icon="h(ReloadOutlined)"
                style="padding: 1px 6px; font-size: 10px;">
                Clear Touch
              </a-button>
            </a-space>
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

// Component state - eye position in degrees
const eyePosition = ref({ x: 0, y: 0 })
const touchPosition = ref({ x: 0, y: 0 })
const showTouchPosition = ref(false)

// Separate virtual input states
const virtualEyeEnabled = ref(false)
const virtualTouchEnabled = ref(false)

// Virtual eye state - purely in degrees, backend handles movement
const virtualEye = reactive({
  x: 0,           // degrees
  y: 0,           // degrees
  active: false,
  isDragging: false,
  dragOffset: { x: 0, y: 0 }
})

// Virtual touch state
const virtualTouch = reactive({
  x: 0,           // screen pixels
  y: 0,           // screen pixels
  active: false,
  isDragging: false,
  dragStartPos: { x: 0, y: 0 },
  lastDragPos: { x: 0, y: 0 }
})

// Animation state tracking
const canvasReady = ref(false)
const animationRunning = ref(false)

// Use dserv's central state
const connected = computed(() => dserv.state.connected)
const eyeWindows = computed(() => dserv.state.eyeWindows)
const eyeWindowStatusMask = computed(() => dserv.state.eyeWindowStatusMask)

const touchWindows = computed(() => {
  if (!dserv.state.touchWindows) {
    dserv.state.touchWindows = Array(8).fill(null).map((_, i) => ({
      id: i, active: false, state: 0, type: 'rectangle',
      center: { x: 0, y: 0 }, centerRaw: { x: 400, y: 320 },
      size: { width: 2, height: 2 }, sizeRaw: { width: 100, height: 100 }
    }))
  }
  return dserv.state.touchWindows
})

const touchWindowStatusMask = computed(() => dserv.state.touchWindowStatusMask || 0)

const screenDimensions = computed(() => ({
  width: dserv.state.screenWidth || 800,
  height: dserv.state.screenHeight || 600,
  halfX: dserv.state.screenHalfX || 10.0,
  halfY: dserv.state.screenHalfY || 7.5
}))

const displayOptions = reactive({
  showTrails: false,
  showLabels: true
})

// Canvas refs and state
const canvasRef = ref(null)
const canvasContainer = ref(null)
const canvasSize = ref({ width: 300, height: 300 })

// Visual range in degrees (matching FLTK's xextent/yextent = 16*2 = 32)
const xExtent = 32.0
const yExtent = 32.0

// Trail history
const eyeHistory = ref([])
const maxTrailLength = 50

let ctx = null
let animationId = null

// Degrees per pixel (matching FLTK calculation)
const degPerPixel = computed(() => {
  const w = canvasSize.value.width
  const h = canvasSize.value.height
  const dpx = xExtent / w
  const dpy = dpx * (h / w)
  return { x: dpx, y: dpy }
})

const canvasCenter = computed(() => ({
  x: canvasSize.value.width / 2,
  y: canvasSize.value.height / 2
}))

// Convert degrees to canvas pixels (matching FLTK)
function degreesToCanvas(degX, degY) {
  return {
    x: canvasCenter.value.x + (degX / degPerPixel.value.x),
    y: canvasCenter.value.y - (degY / degPerPixel.value.y)
  }
}

// Convert canvas pixels to degrees
function canvasToDegrees(canvasX, canvasY) {
  return {
    x: (canvasX - canvasCenter.value.x) * degPerPixel.value.x,
    y: -(canvasY - canvasCenter.value.y) * degPerPixel.value.y
  }
}

// Convert touch screen pixels to degrees
function touchPixelsToDegrees(pixX, pixY) {
  const screen = screenDimensions.value
  const screenPixPerDegX = screen.width / (2 * screen.halfX)
  const screenPixPerDegY = screen.height / (2 * screen.halfY)
  return {
    x: (pixX - screen.width / 2) / screenPixPerDegX,
    y: -(pixY - screen.height / 2) / screenPixPerDegY
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

// Virtual eye position update
function updateVirtualEyePosition(degX, degY) {
  virtualEye.x = degX
  virtualEye.y = degY
  virtualEye.active = true
  sendVirtualEyeTarget(degX, degY)
}

async function sendVirtualEyeTarget(x, y) {
  try {
    // Tell virtual_eye subprocess to hold this position
    await dserv.essCommand(`send virtual_eye {set_eye ${x} ${y}}`)
  } catch (error) {
    console.error('Failed to send virtual eye target:', error)
  }
}

async function setVirtualEyeControl(enabled) {
  try {
    if (enabled) {
      await dserv.essCommand('send virtual_eye start')
    } else {
      await dserv.essCommand('send virtual_eye stop')
    }
  } catch (error) {
    console.error('Failed to set virtual eye control:', error)
  }
}

async function sendVirtualTouchData(x, y, eventType = 0) {
  try {
    const cmd = `set d [binary format s3 {${x} ${y} ${eventType}}]; dservSetData mtouch/event 0 4 $d; unset d`
    await dserv.essCommand(cmd)
  } catch (error) {
    console.error('Failed to send virtual touch data:', error)
  }
}

// Mouse event handlers
function handleMouseDown(event) {
  const rect = canvasRef.value.getBoundingClientRect()
  const mouseX = event.clientX - rect.left
  const mouseY = event.clientY - rect.top

  if (virtualEyeEnabled.value && virtualEye.active) {
    const eyePos = degreesToCanvas(virtualEye.x, virtualEye.y)
    const eyeDistance = Math.sqrt(Math.pow(mouseX - eyePos.x, 2) + Math.pow(mouseY - eyePos.y, 2))
    if (eyeDistance <= 13) {
      virtualEye.isDragging = true
      virtualEye.dragOffset = { x: eyePos.x - mouseX, y: eyePos.y - mouseY }
      event.preventDefault()
      return
    }
  }

  if (virtualTouchEnabled.value && !virtualEye.isDragging) {
    const degPos = canvasToDegrees(mouseX, mouseY)
    const touchPixels = degreesToTouchPixels(degPos.x, degPos.y)
    virtualTouch.x = touchPixels.x
    virtualTouch.y = touchPixels.y
    virtualTouch.active = true
    virtualTouch.isDragging = true
    virtualTouch.dragStartPos = { x: touchPixels.x, y: touchPixels.y }
    virtualTouch.lastDragPos = { x: touchPixels.x, y: touchPixels.y }
    sendVirtualTouchData(touchPixels.x, touchPixels.y, 0)
    event.preventDefault()
  }
}

function handleMouseMove(event) {
  const rect = canvasRef.value.getBoundingClientRect()
  const mouseX = event.clientX - rect.left
  const mouseY = event.clientY - rect.top

  if (virtualEyeEnabled.value && virtualEye.isDragging) {
    const newCanvasX = mouseX + virtualEye.dragOffset.x
    const newCanvasY = mouseY + virtualEye.dragOffset.y
    const degPos = canvasToDegrees(newCanvasX, newCanvasY)
    const maxX = xExtent / 2
    const maxY = yExtent / 2
    degPos.x = Math.max(-maxX, Math.min(maxX, degPos.x))
    degPos.y = Math.max(-maxY, Math.min(maxY, degPos.y))
    updateVirtualEyePosition(degPos.x, degPos.y)
  }

  if (virtualTouchEnabled.value && virtualTouch.isDragging) {
    const degPos = canvasToDegrees(mouseX, mouseY)
    const touchPixels = degreesToTouchPixels(degPos.x, degPos.y)
    const dragThreshold = 2
    if (Math.abs(touchPixels.x - virtualTouch.lastDragPos.x) > dragThreshold ||
      Math.abs(touchPixels.y - virtualTouch.lastDragPos.y) > dragThreshold) {
      virtualTouch.x = touchPixels.x
      virtualTouch.y = touchPixels.y
      virtualTouch.lastDragPos = { x: touchPixels.x, y: touchPixels.y }
      sendVirtualTouchData(touchPixels.x, touchPixels.y, 1)
    }
  }
}

function handleMouseUp(event) {
  if (virtualEye.isDragging) virtualEye.isDragging = false
  if (virtualTouch.isDragging) {
    sendVirtualTouchData(virtualTouch.x, virtualTouch.y, 2)
    virtualTouch.isDragging = false
    setTimeout(() => { if (!virtualTouch.isDragging) virtualTouch.active = false }, 500)
  }
}

function handleMouseLeave(event) {
  if (virtualEye.isDragging) virtualEye.isDragging = false
  if (virtualTouch.isDragging) {
    sendVirtualTouchData(virtualTouch.x, virtualTouch.y, 2)
    virtualTouch.isDragging = false
    virtualTouch.active = false
  }
}

// Data handlers
function handleEyePosition(data) {
  if (virtualEyeEnabled.value) return

  try {
    const value = data.value || data.data
    if (data.name !== 'ess/em_pos' || !value || typeof value !== 'string') return

    const parts = value.split(' ')
    if (parts.length < 2) return

    // Format: "h_deg v_deg" (horizontal first, then vertical)
    const em_x = parseFloat(parts[0])  // horizontal
    const em_y = parseFloat(parts[1])  // vertical
    if (isNaN(em_x) || isNaN(em_y)) return

    eyePosition.value = { x: em_x, y: em_y }

    if (displayOptions.showTrails) {
      eyeHistory.value.push({ ...eyePosition.value, timestamp: Date.now() })
      if (eyeHistory.value.length > maxTrailLength) eyeHistory.value.shift()
    }
  } catch (error) {
    console.error('Error handling eye position:', error)
  }
}

function handleTouchEvent(data) {
  if (virtualTouchEnabled.value && virtualTouch.active) return
  try {
    const value = data.value || data.data
    if (data.name !== 'mtouch/event') return
    let x, y, eventType
    if (Array.isArray(value)) {
      if (value.length < 3) return
      [x, y, eventType] = value.map(Number)
    } else if (typeof value === 'string') {
      const parts = value.split(' ')
      if (parts.length < 3) return
      [x, y, eventType] = parts.map(Number)
    } else return
    if (isNaN(x) || isNaN(y) || isNaN(eventType)) return
    switch (eventType) {
      case 0: touchPosition.value = { x, y }; showTouchPosition.value = true; break
      case 1: if (showTouchPosition.value) touchPosition.value = { x, y }; break
      case 2: showTouchPosition.value = false; touchPosition.value = { x: 0, y: 0 }; break
    }
  } catch (error) { console.error('Error handling touch event:', error) }
}

function handleTouchWindowSetting(data) {
  try {
    const value = data.value || data.data
    if (data.name !== 'ess/touch_region_setting' || !value || typeof value !== 'string') return
    const parts = value.split(' ')
    if (parts.length < 8) return
    const [reg, active, state, type, cx, cy, dx, dy] = parts.map(Number)
    if (parts.some(p => isNaN(Number(p)))) return
    if (reg >= 0 && reg < 8) {
      if (!dserv.state.touchWindows) {
        dserv.state.touchWindows = Array(8).fill(null).map((_, i) => ({
          id: i, active: false, state: 0, type: 'rectangle',
          center: { x: 0, y: 0 }, centerRaw: { x: 400, y: 320 },
          size: { width: 2, height: 2 }, sizeRaw: { width: 100, height: 100 }
        }))
      }
      dserv.state.touchWindows[reg] = {
        id: reg, active: active === 1, state: state,
        type: type === 1 ? 'ellipse' : 'rectangle',
        center: touchPixelsToDegrees(cx, cy),
        centerRaw: { x: cx, y: cy },
        size: {
          width: Math.abs(dx / screenDimensions.value.width * (2 * screenDimensions.value.halfX)),
          height: Math.abs(dy / screenDimensions.value.height * (2 * screenDimensions.value.halfY))
        },
        sizeRaw: { width: dx, height: dy }
      }
    }
  } catch (error) { console.error('Error handling touch window setting:', error) }
}

function handleTouchWindowStatus(data) {
  try {
    const value = data.value || data.data
    if (data.name !== 'ess/touch_region_status' || !value || typeof value !== 'string') return
    const parts = value.split(' ')
    if (parts.length < 4) return
    const [changes, states, touch_x, touch_y] = parts.map(Number)
    if (parts.some(p => isNaN(Number(p)))) return
    dserv.state.touchWindowStatusMask = states
    if (!dserv.state.touchWindows) {
      dserv.state.touchWindows = Array(8).fill(null).map((_, i) => ({
        id: i, active: false, state: 0, type: 'rectangle',
        center: { x: 0, y: 0 }, centerRaw: { x: 400, y: 320 },
        size: { width: 2, height: 2 }, sizeRaw: { width: 100, height: 100 }
      }))
    }
    for (let i = 0; i < 8; i++) {
      dserv.state.touchWindows[i].state = ((states & (1 << i)) !== 0) && dserv.state.touchWindows[i].active
    }
    if (!isNaN(touch_x) && !isNaN(touch_y)) touchPosition.value = { x: touch_x, y: touch_y }
  } catch (error) { console.error('Error handling touch window status:', error) }
}

function handleScreenDimensions(data) {
  const value = data.value || data.data
  switch (data.name) {
    case 'ess/screen_w': dserv.state.screenWidth = parseInt(value, 10) || 800; break
    case 'ess/screen_h': dserv.state.screenHeight = parseInt(value, 10) || 600; break
    case 'ess/screen_halfx': dserv.state.screenHalfX = parseFloat(value) || 10.0; break
    case 'ess/screen_halfy': dserv.state.screenHalfY = parseFloat(value) || 7.5; break
  }
}

// Canvas setup
function setupCanvas() {
  if (!canvasRef.value) return false
  try {
    ctx = canvasRef.value.getContext('2d')
    if (!ctx) return false
    canvasReady.value = true
    return true
  } catch (error) {
    canvasReady.value = false
    return false
  }
}

function startAnimation() {
  if (animationId) cancelAnimationFrame(animationId)
  if (!ctx || !canvasRef.value) return
  animationRunning.value = true
  function animate() {
    if (!animationRunning.value || !ctx || !canvasRef.value) {
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
  if (animationId) { cancelAnimationFrame(animationId); animationId = null }
}

function render() {
  if (!ctx || !canvasRef.value) return
  try {
    ctx.fillStyle = '#000000'
    ctx.fillRect(0, 0, canvasSize.value.width, canvasSize.value.height)
    drawGrid()
    if (displayOptions.showTrails) drawTrails()
    eyeWindows.value.forEach(window => { if (window.active) drawEyeWindow(window) })
    if (touchWindows.value) touchWindows.value.forEach(window => { if (window && window.active) drawTouchWindow(window) })
    if (virtualEyeEnabled.value && virtualEye.active) drawVirtualEye()
    else if (!virtualEyeEnabled.value) drawEyePosition()
    if (virtualTouchEnabled.value && virtualTouch.active) drawVirtualTouch()
    else if (showTouchPosition.value) drawTouchPosition()
  } catch (error) { console.error('Render error:', error) }
}

function drawGrid() {
  ctx.strokeStyle = '#333333'
  ctx.lineWidth = 1
  for (let deg = -15; deg <= 15; deg += 5) {
    const pos = degreesToCanvas(deg, 0)
    ctx.beginPath(); ctx.moveTo(pos.x, 0); ctx.lineTo(pos.x, canvasSize.value.height); ctx.stroke()
  }
  for (let deg = -15; deg <= 15; deg += 5) {
    const pos = degreesToCanvas(0, deg)
    ctx.beginPath(); ctx.moveTo(0, pos.y); ctx.lineTo(canvasSize.value.width, pos.y); ctx.stroke()
  }
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
    if (i === 0) ctx.moveTo(pos.x, pos.y)
    else ctx.lineTo(pos.x, pos.y)
  }
  ctx.globalAlpha = 0.5
  ctx.stroke()
  ctx.globalAlpha = 1.0
}

function drawEyeWindow(window) {
  const pos = degreesToCanvas(window.center.x, window.center.y)
  const halfW = window.size.width / degPerPixel.value.x
  const halfH = window.size.height / degPerPixel.value.y
  const isInside = (eyeWindowStatusMask.value & (1 << window.id)) !== 0
  if (isInside) {
    ctx.fillStyle = 'rgb(100, 50, 50)'
    if (window.type === 'ellipse') {
      ctx.beginPath(); ctx.ellipse(pos.x, pos.y, halfW, halfH, 0, 0, 2 * Math.PI); ctx.fill()
    } else ctx.fillRect(pos.x - halfW, pos.y - halfH, 2 * halfW, 2 * halfH)
  }
  ctx.strokeStyle = '#ff0000'
  ctx.lineWidth = isInside ? 2 : 1
  if (window.type === 'ellipse') {
    ctx.beginPath(); ctx.ellipse(pos.x, pos.y, halfW, halfH, 0, 0, 2 * Math.PI); ctx.stroke()
  } else ctx.strokeRect(pos.x - halfW, pos.y - halfH, 2 * halfW, 2 * halfH)
  ctx.fillStyle = '#ff0000'
  ctx.beginPath(); ctx.arc(pos.x, pos.y, 2, 0, 2 * Math.PI); ctx.fill()
  if (displayOptions.showLabels) {
    ctx.fillStyle = '#ff0000'
    ctx.font = '12px monospace'
    ctx.fillText(`E${window.id}`, pos.x - halfW + 2, pos.y - halfH - 5)
  }
}

function drawTouchWindow(window) {
  const pos = degreesToCanvas(window.center.x, window.center.y)
  const halfW = window.size.width / degPerPixel.value.x
  const halfH = window.size.height / degPerPixel.value.y
  const isInside = (touchWindowStatusMask.value & (1 << window.id)) !== 0
  if (isInside) {
    ctx.fillStyle = 'rgb(50, 100, 100)'
    if (window.type === 'ellipse') {
      ctx.beginPath(); ctx.ellipse(pos.x, pos.y, halfW, halfH, 0, 0, 2 * Math.PI); ctx.fill()
    } else ctx.fillRect(pos.x - halfW, pos.y - halfH, 2 * halfW, 2 * halfH)
  }
  ctx.strokeStyle = '#00ffff'
  ctx.lineWidth = isInside ? 2 : 1
  if (window.type === 'ellipse') {
    ctx.beginPath(); ctx.ellipse(pos.x, pos.y, halfW, halfH, 0, 0, 2 * Math.PI); ctx.stroke()
  } else ctx.strokeRect(pos.x - halfW, pos.y - halfH, 2 * halfW, 2 * halfH)
  ctx.fillStyle = '#00ffff'
  ctx.beginPath(); ctx.arc(pos.x, pos.y, 2, 0, 2 * Math.PI); ctx.fill()
  if (displayOptions.showLabels) {
    ctx.fillStyle = '#00ffff'
    ctx.font = '12px monospace'
    ctx.fillText(`T${window.id}`, pos.x + halfW - 20, pos.y + halfH + 15)
  }
}

function drawEyePosition() {
  const pos = degreesToCanvas(eyePosition.value.x, eyePosition.value.y)
  ctx.fillStyle = '#ffffff'
  ctx.strokeStyle = '#ff0000'
  ctx.lineWidth = 2
  ctx.beginPath(); ctx.arc(pos.x, pos.y, 5, 0, 2 * Math.PI); ctx.fill(); ctx.stroke()
  ctx.strokeStyle = '#ffffff'
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.moveTo(pos.x - 10, pos.y); ctx.lineTo(pos.x + 10, pos.y)
  ctx.moveTo(pos.x, pos.y - 10); ctx.lineTo(pos.x, pos.y + 10)
  ctx.stroke()
}

function drawVirtualEye() {
  const pos = degreesToCanvas(virtualEye.x, virtualEye.y)
  ctx.fillStyle = '#ffffff'
  ctx.strokeStyle = virtualEye.isDragging ? '#00ff00' : '#ff8c00'
  ctx.lineWidth = 2
  ctx.beginPath(); ctx.arc(pos.x, pos.y, 8, 0, 2 * Math.PI); ctx.fill(); ctx.stroke()
  ctx.strokeStyle = '#000000'
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.moveTo(pos.x - 6, pos.y); ctx.lineTo(pos.x + 6, pos.y)
  ctx.moveTo(pos.x, pos.y - 6); ctx.lineTo(pos.x, pos.y + 6)
  ctx.stroke()
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
  const size = 6
  ctx.beginPath()
  ctx.moveTo(pos.x, pos.y - size); ctx.lineTo(pos.x + size, pos.y)
  ctx.lineTo(pos.x, pos.y + size); ctx.lineTo(pos.x - size, pos.y)
  ctx.closePath(); ctx.fill(); ctx.stroke()
}

function drawVirtualTouch() {
  const touchDegrees = touchPixelsToDegrees(virtualTouch.x, virtualTouch.y)
  const pos = degreesToCanvas(touchDegrees.x, touchDegrees.y)
  ctx.fillStyle = '#ff8c00'
  ctx.strokeStyle = virtualTouch.isDragging ? '#ffffff' : '#cc6600'
  ctx.lineWidth = virtualTouch.isDragging ? 3 : 2
  const size = virtualTouch.isDragging ? 11 : 9
  ctx.beginPath()
  ctx.moveTo(pos.x, pos.y - size); ctx.lineTo(pos.x + size, pos.y)
  ctx.lineTo(pos.x, pos.y + size); ctx.lineTo(pos.x - size, pos.y)
  ctx.closePath(); ctx.fill(); ctx.stroke()
  ctx.fillStyle = '#ffffff'
  ctx.font = 'bold 8px monospace'
  ctx.fillText('V', pos.x - 2, pos.y + 2)
  if (virtualTouch.isDragging) {
    const startDegrees = touchPixelsToDegrees(virtualTouch.dragStartPos.x, virtualTouch.dragStartPos.y)
    const startPos = degreesToCanvas(startDegrees.x, startDegrees.y)
    ctx.strokeStyle = 'rgba(255, 140, 0, 0.3)'
    ctx.lineWidth = 1
    ctx.setLineDash([5, 5])
    ctx.beginPath(); ctx.moveTo(startPos.x, startPos.y); ctx.lineTo(pos.x, pos.y); ctx.stroke()
    ctx.setLineDash([])
  }
}

async function refreshWindows() {
  try {
    for (let i = 0; i < 8; i++) await dserv.essCommand(`ainGetRegionInfo ${i}`)
    for (let i = 0; i < 8; i++) await dserv.essCommand(`touchGetRegionInfo ${i}`)
  } catch (error) { console.error('Failed to refresh windows:', error) }
}

async function resetVirtualEye() { updateVirtualEyePosition(0, 0) }
function resetVirtualTouch() { virtualTouch.active = false; virtualTouch.isDragging = false; virtualTouch.x = 0; virtualTouch.y = 0 }

function handleResize() {
  if (!canvasContainer.value) return
  const rect = canvasContainer.value.getBoundingClientRect()
  const padding = 20
  const size = Math.min(rect.width - padding, rect.height - padding)
  canvasSize.value = { width: size, height: size }
  nextTick(() => {
    if (canvasRef.value && !ctx) setupCanvas()
    if (canvasReady.value && !animationRunning.value) startAnimation()
  })
}

let cleanupDserv = null
let resizeObserver = null

onMounted(() => {
  nextTick(() => {
    if (setupCanvas()) {
      cleanupDserv = dserv.registerComponent('EyeTouchVisualizer')
      dserv.on('datapoint:ess/em_pos', handleEyePosition, 'EyeTouchVisualizer')
      dserv.on('datapoint:mtouch/event', handleTouchEvent, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/touch_region_setting', handleTouchWindowSetting, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/touch_region_status', handleTouchWindowStatus, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/screen_w', handleScreenDimensions, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/screen_h', handleScreenDimensions, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/screen_halfx', handleScreenDimensions, 'EyeTouchVisualizer')
      dserv.on('datapoint:ess/screen_halfy', handleScreenDimensions, 'EyeTouchVisualizer')
      resizeObserver = new ResizeObserver(() => handleResize())
      if (canvasContainer.value) resizeObserver.observe(canvasContainer.value)
      handleResize()
      startAnimation()
      if (dserv.state.connected) refreshWindows()
      virtualEye.x = 0; virtualEye.y = 0; virtualEye.active = false
    }
  })
})

onUnmounted(() => {
  stopAnimation()
  if (resizeObserver) resizeObserver.disconnect()
  if (cleanupDserv) cleanupDserv()
  canvasReady.value = false
  ctx = null
})

watch(canvasReady, (ready) => { if (ready && !animationRunning.value) startAnimation() })
watch(() => displayOptions.showTrails, (showTrails) => { if (!showTrails) eyeHistory.value = [] })

watch(virtualEyeEnabled, async (enabled) => {
  await setVirtualEyeControl(enabled)
  if (enabled) {
    if (eyePosition.value.x !== 0 || eyePosition.value.y !== 0) updateVirtualEyePosition(eyePosition.value.x, eyePosition.value.y)
    else updateVirtualEyePosition(0, 0)
  } else { virtualEye.active = false; virtualEye.isDragging = false }
})

watch(virtualTouchEnabled, (enabled) => {
  if (!enabled) {
    if (virtualTouch.isDragging) sendVirtualTouchData(virtualTouch.x, virtualTouch.y, 2)
    virtualTouch.active = false; virtualTouch.isDragging = false
  }
})

defineExpose({
  refreshWindows, resetVirtualEye, resetVirtualTouch,
  getEyePosition: () => eyePosition.value, getTouchPosition: () => touchPosition.value,
  getWindowStates: () => eyeWindows.value, getWindowStatusMask: () => eyeWindowStatusMask.value,
  getTouchWindowStates: () => touchWindows.value, getTouchWindowStatusMask: () => touchWindowStatusMask.value,
  getVirtualEyePosition: () => ({ x: virtualEye.x, y: virtualEye.y }),
  setVirtualEyeEnabled: (enabled) => { virtualEyeEnabled.value = enabled },
  setVirtualTouchEnabled: (enabled) => { virtualTouchEnabled.value = enabled }
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

.visualization-canvas.virtual-eye-enabled {
  cursor: crosshair;
}

.visualization-canvas.virtual-touch-enabled {
  cursor: pointer;
}

.visualization-canvas.virtual-eye-enabled.virtual-touch-enabled {
  cursor: crosshair;
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

:deep(.ant-checkbox-wrapper) {
  font-size: 11px;
  line-height: 1.2;
}

:deep(.ant-checkbox) {
  margin-right: 4px;
}
</style>
