<template>
  <div class="graphics-viewer">
    <!-- Graphics Controls Header -->
    <div v-if="showControls" class="graphics-header">
      <div class="stream-controls">
        <span class="control-label">{{ displayTitle }}:</span>
        <a-select
          v-if="allowStreamSelection"
          v-model:value="selectedStream"
          size="small"
          style="width: 180px;"
        >
          <a-select-option
            v-for="stream in availableStreams"
            :key="stream"
            :value="stream"
          >
            {{ stream }}
          </a-select-option>
        </a-select>
        <span v-else class="stream-display">{{ selectedStream }}</span>

        <a-tag :color="isConnected ? 'green' : 'default'" size="small">
          {{ isConnected ? 'Connected' : 'No Data' }}
        </a-tag>
      </div>

      <div class="action-controls">
        <a-button size="small" @click="sendTestGraphics" :loading="sendingTest">
          Test Graphics
        </a-button>
        <a-button size="small" @click="clearGraphics">
          Clear
        </a-button>
      </div>
    </div>

    <!-- Graphics Canvas Area -->
    <div class="graphics-content">
      <GraphicsCanvas
        ref="canvasRef"
        :stream-id="selectedStream"
        :aspect-ratio="aspectRatio"
        :show-stats="false"
        :show-debug="false"
        :auto-scale="true"
        aspect-ratio-mode="backend"
        :fixed-aspect-ratio="16/9"
        canvas-class="graphics-viewer-canvas"
        @mousemove="handleMouseMove"
        @click="handleClick"
        :enable-input-datapoints="true"
      />
    </div>

    <!-- Graphics Status Footer -->
    <div v-if="showFooter" class="graphics-footer">
      <div class="status-item">
        <strong>Stream:</strong> {{ selectedStream }}
      </div>
      <div class="status-item" v-if="lastMousePos">
        <strong>Mouse:</strong> ({{ lastMousePos.normalizedX.toFixed(0) }}, {{ lastMousePos.normalizedY.toFixed(0) }})
      </div>
      <div class="status-item">
        <strong>Canvas:</strong> {{ canvasSize.width }}×{{ canvasSize.height }}px
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import GraphicsCanvas from './GraphicsCanvas.vue'
import { dserv } from '../services/dserv.js'

// Props for configuration
const props = defineProps({
  streamId: {
    type: String,
    default: 'graphics/main'
  },
  allowStreamSelection: {
    type: Boolean,
    default: true
  },
  availableStreams: {
    type: Array,
    default: () => ['graphics/main', 'graphics/stimulus', 'graphics/feedback', 'graphics/test']
  },
  aspectRatio: {
    type: Number,
    default: null
  },
  showControls: {
    type: Boolean,
    default: true
  },
  showFooter: {
    type: Boolean,
    default: true
  },
  title: {
    type: String,
    default: null
  }
})

// Component state
const canvasRef = ref(null)
const selectedStream = ref(props.streamId)
const isConnected = ref(false)
const sendingTest = ref(false)
const lastMousePos = ref(null)
const canvasSize = ref({ width: 640, height: 480 })

// Computed display title
const displayTitle = computed(() => {
  if (props.title) return props.title
  return props.allowStreamSelection ? 'Graphics Viewer' : selectedStream.value
})

// Event handlers
const handleMouseMove = (mouseData) => {
  lastMousePos.value = mouseData
}

const handleClick = (clickData) => {
 // console.log('Graphics click:', clickData)
}

// Update canvas size from child component
const updateCanvasSize = () => {
  if (canvasRef.value && canvasRef.value.getCanvasSize) {
    canvasSize.value = canvasRef.value.getCanvasSize()
  }
}

// Graphics operations using current canvas size
const sendTestGraphics = async () => {
  sendingTest.value = true
  updateCanvasSize() // Get current size

  try {
    const { width, height } = canvasSize.value
    const testCommand = `
      set gbuf_data {
        "commands": [
          {"cmd": "setwindow", "args": [0, 0, ${width}, ${height}]},
          {"cmd": "setcolor", "args": [1]},
          {"cmd": "circle", "args": [${width/2}, ${height/2}, 100, false]},
          {"cmd": "setcolor", "args": [4]},
          {"cmd": "fcircle", "args": [${width/2}, ${height/2}, 50, true]},
          {"cmd": "setcolor", "args": [0]},
          {"cmd": "setfont", "args": ["Arial", 20]},
          {"cmd": "setjust", "args": [0]},
          {"cmd": "moveto", "args": [${width/2}, ${height/4}]},
          {"cmd": "drawtext", "args": ["Test: [clock format [clock seconds] -format %H:%M:%S]"]}
        ]
      }
      dservSet ${selectedStream.value} $gbuf_data
    `

    await dserv.essCommand(testCommand)
    console.log(`Test graphics sent to ${selectedStream.value} (${width}×${height})`)

  } catch (error) {
    console.error('Graphics test error:', error)
  } finally {
    sendingTest.value = false
  }
}

const clearGraphics = async () => {
  try {
    updateCanvasSize() // Get current size
    const { width, height } = canvasSize.value

    const clearCommand = `
      set gbuf_data {
        "commands": [
          {"cmd": "setwindow", "args": [0, 0, ${width}, ${height}]},
          {"cmd": "setcolor", "args": [7]},
          {"cmd": "filledrect", "args": [0, 0, ${width}, ${height}]}
        ]
      }
      dservSet ${selectedStream.value} $gbuf_data
    `

    await dserv.essCommand(clearCommand)
    console.log(`Graphics cleared on ${selectedStream.value} (${width}×${height})`)

  } catch (error) {
    console.error('Graphics clear error:', error)
  }
}

// Stream management
let currentCleanup = null

const setupStreamListener = (streamId) => {
  if (currentCleanup) {
    currentCleanup()
  }

  isConnected.value = false

  currentCleanup = dserv.on(`datapoint:${streamId}`, (data) => {
    isConnected.value = true
  }, `GraphicsViewer_${streamId}`)
}

// Watch for stream changes
watch(selectedStream, setupStreamListener, { immediate: true })

// Component lifecycle
onMounted(() => {
  console.log('GraphicsViewer mounted with simplified approach')
  // Update canvas size after mount
  setTimeout(updateCanvasSize, 100)
})

onUnmounted(() => {
  if (currentCleanup) {
    currentCleanup()
  }
})

// Expose methods for parent components
defineExpose({
  sendTestGraphics,
  clearGraphics,
  setStream: (stream) => { selectedStream.value = stream },
  updateCanvasSize
})
</script>

<style scoped>
.graphics-viewer {
  height: 100%;
  display: flex;
  flex-direction: column;
  background: #fafafa;
  overflow: hidden;
}

.graphics-header {
  flex-shrink: 0;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px;
  background: white;
  border-bottom: 1px solid #d9d9d9;
}

.stream-controls,
.action-controls {
  display: flex;
  align-items: center;
  gap: 12px;
}

.control-label {
  font-weight: 500;
  font-size: 12px;
  color: #666;
}

.graphics-content {
  flex: 1;
  display: flex;
  justify-content: center;
  align-items: center;
  min-height: 0;
  overflow: hidden;
  padding: 20px;
}

.graphics-footer {
  flex-shrink: 0;
  display: grid;
  grid-template-columns: 1fr 1fr 1fr;
  gap: 8px;
  padding: 8px 12px;
  background: white;
  border-top: 1px solid #d9d9d9;
  font-size: 11px;
}

.status-item {
  padding: 4px 8px;
  background: #f5f5f5;
  border-radius: 4px;
  text-align: center;
}

/* Let the canvas component handle its own sizing */
:deep(.graphics-viewer-canvas) {
  border: 2px solid #d9d9d9;
  border-radius: 6px;
  background: white;
  cursor: crosshair;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
}

:deep(.graphics-viewer-canvas:hover) {
  border-color: #40a9ff;
}
</style>


