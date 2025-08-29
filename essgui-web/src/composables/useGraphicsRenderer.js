// useGraphicsRenderer.js - Updated for new dserv architecture
import { ref, onMounted, onUnmounted, nextTick, readonly, watch } from 'vue'
import { dserv } from '@/services/dserv.js'

export function useGraphicsRenderer(canvasRef, options = {}) {
  // Reactive state
  const renderStats = ref('Canvas ready, waiting for data...')
  const lastUpdate = ref(null)
  const dataReceived = ref(false)
  const isConnected = ref(false)

  // Configuration with defaults
  const config = {
    width: options.width || 640,
    height: options.height || 480,
    streamId: options.streamId || 'graphics/main',
    autoScale: options.autoScale !== false, // Default to true
    ...options
  }

  // Internal state
  let ctx = null
  let currentColor = '#000000'
  let currentPos = { x: 0, y: 0 }
  let currentFont = 'Helvetica'
  let currentFontSize = 10
  let currentJustification = 0
  let currentOrientation = 0
  let windowBounds = null
  let scaleX = 1
  let scaleY = 1
  let lastGbufData = null  // Store last graphics data for redraws

  // Cleanup tracking
  const cleanupFunctions = []

  // Y coordinate flip helper
  const flipY = (y) => config.height - y

  // Scaling helpers
  const scaleX_coord = (x) => x * scaleX
  const scaleY_coord = (y) => y * scaleY
  const scaleWidth = (w) => w * scaleX
  const scaleHeight = (h) => h * scaleY

  // Color mapping
  const colorToHex = (colorIndex) => {
    const colors = [
      '#000000', '#0000FF', '#008000', '#00FFFF', '#FF0000',
      '#FF00FF', '#A52A2A', '#FFFFFF', '#808080', '#ADD8E6',
      '#00FF00', '#E0FFFF', '#FF1493', '#9370DB', '#FFFF00'
    ]
    return colors[colorIndex] || '#000000'
  }

  // Calculate scaling factors
  const updateScaling = (sourceWidth, sourceHeight) => {
    if (config.autoScale && sourceWidth && sourceHeight) {
      scaleX = config.width / sourceWidth
      scaleY = config.height / sourceHeight
      console.log(`Graphics scaling: ${scaleX.toFixed(3)}x, ${scaleY.toFixed(3)}y`)
    } else {
      scaleX = 1
      scaleY = 1
    }
  }

  // Execute individual graphics command with scaling
  const executeGBCommand = (commandObj) => {
    const { cmd, args } = commandObj

    try {
      switch (cmd) {
        case 'setwindow':
          windowBounds = {
            llx: args[0], lly: args[1],
            urx: args[2], ury: args[3]
          }
          // Update scaling based on window bounds if auto-scaling enabled
          if (config.autoScale && windowBounds) {
            const sourceWidth = windowBounds.urx - windowBounds.llx
            const sourceHeight = windowBounds.ury - windowBounds.lly
            updateScaling(sourceWidth, sourceHeight)
          }
          break

        case 'setcolor':
          currentColor = colorToHex(args[0])
          ctx.strokeStyle = currentColor
          ctx.fillStyle = currentColor
          break

        case 'setjust':
          const justValue = args[0]
          if (justValue === -1) {
            currentJustification = 0  // left
          } else if (justValue === 0) {
            currentJustification = 1  // center
          } else if (justValue === 1) {
            currentJustification = 2  // right
          } else {
            currentJustification = 1  // default to center
          }
          break

        case 'setorientation':
          const orientValue = args[0]
          const orientationMap = { 0: 0, 1: 270, 2: 180, 3: 90 }
          currentOrientation = orientationMap[orientValue] !== undefined ? orientationMap[orientValue] : 0
          break

        case 'setfont':
          currentFont = args[0] || 'Helvetica'
          currentFontSize = (args[1] || 10) * Math.min(scaleX, scaleY) // Scale font size
          ctx.font = `${currentFontSize}px ${currentFont}`
          break

        case 'gsave':
          ctx.save()
          break

        case 'grestore':
          ctx.restore()
          // Re-apply current state after restore
          ctx.strokeStyle = currentColor
          ctx.fillStyle = currentColor
          ctx.font = `${currentFontSize}px ${currentFont}`
          break

        case 'setclipregion':
          // Clipping disabled for now - can cause issues without proper save/restore bracketing
          // TODO: Implement proper clipping support when backend graphics are updated
          break

        case 'circle':
        case 'fcircle':
          ctx.beginPath()
          ctx.arc(
            scaleX_coord(args[0]),
            flipY(scaleY_coord(args[1])),
            scaleWidth(args[2]),
            0, 2 * Math.PI
          )
          const filled = (cmd === 'fcircle') || args[3]
          if (filled) {
            ctx.fill()
          } else {
            ctx.stroke()
          }
          break

        case 'line':
          ctx.beginPath()
          ctx.moveTo(scaleX_coord(args[0]), flipY(scaleY_coord(args[1])))
          ctx.lineTo(scaleX_coord(args[2]), flipY(scaleY_coord(args[3])))
          ctx.stroke()
          break

        case 'moveto':
          currentPos = {
            x: scaleX_coord(args[0]),
            y: flipY(scaleY_coord(args[1]))
          }
          break

        case 'lineto':
          ctx.beginPath()
          ctx.moveTo(currentPos.x, currentPos.y)
          const newX = scaleX_coord(args[0])
          const newY = flipY(scaleY_coord(args[1]))
          ctx.lineTo(newX, newY)
          ctx.stroke()
          currentPos = { x: newX, y: newY }
          break

        case 'filledrect':
          const width = scaleWidth(args[2] - args[0])
          const height = scaleHeight(args[3] - args[1])
          ctx.fillRect(
            scaleX_coord(args[0]),
            flipY(scaleY_coord(args[3])),
            width,
            height
          )
          break

        case 'drawtext':
          if (currentPos && currentPos.x !== undefined && currentPos.y !== undefined && args[0]) {
            const text = String(args[0])

            ctx.save()

            // Set text properties
            ctx.font = `${currentFontSize}px ${currentFont}`
            ctx.fillStyle = currentColor
            ctx.textAlign = ['left', 'center', 'right'][currentJustification] || 'left'
            ctx.textBaseline = 'middle'

            const x = currentPos.x
            const y = currentPos.y

            if (currentOrientation !== 0) {
              ctx.translate(x, y)
              ctx.rotate((currentOrientation * Math.PI) / 180)
              ctx.fillText(text, 0, 0)
            } else {
              ctx.fillText(text, x, y)
            }

            ctx.restore()
          }
          break

        case 'setlwidth':
          ctx.lineWidth = Math.max(1, (args[0] / 100) * Math.min(scaleX, scaleY))
          break

        default:
          // Silently ignore unknown commands
          break
      }
    } catch (error) {
      console.error('Error executing graphics command:', cmd, args, error)
    }
  }

  // Render complete gbuf command set
  const renderGbufCommands = (gbufData) => {
    if (!ctx || !gbufData) return

    // Store the data for potential redraws
    lastGbufData = gbufData

    // Clear canvas
    ctx.fillStyle = 'white'
    ctx.fillRect(0, 0, config.width, config.height)

    // Reset transform and clipping
    ctx.setTransform(1, 0, 0, 1, 0, 0)

    // Reset graphics state
    ctx.strokeStyle = '#000000'
    ctx.fillStyle = '#000000'
    ctx.lineWidth = 1
    ctx.font = '10px Helvetica'
    ctx.textAlign = 'left'
    ctx.textBaseline = 'middle'

    // Initialize state variables
    currentColor = '#000000'
    currentPos = { x: 0, y: 0 }
    windowBounds = null
    currentFont = 'Helvetica'
    currentFontSize = 10
    currentJustification = 0
    currentOrientation = 0
    scaleX = 1
    scaleY = 1

    try {
      const commands = gbufData.commands || []
      let commandCount = 0
      let textCommandCount = 0

      for (const command of commands) {
        if (command.cmd && command.args) {
          if (command.cmd === 'drawtext') {
            textCommandCount++
          }
          executeGBCommand(command)
          commandCount++
        }
      }

      const statsMessage = `Rendered ${commandCount} commands (${textCommandCount} text) at ${new Date().toLocaleTimeString()}`
      renderStats.value = statsMessage
      lastUpdate.value = new Date().toLocaleTimeString()

    } catch (error) {
      console.error('Render error:', error)
      renderStats.value = `Render error: ${error.message}`
    }
  }

  // Handle incoming graphics data from new dserv system
  const handleGraphicsData = (data) => {
    dataReceived.value = true
    lastUpdate.value = new Date().toLocaleTimeString()

    try {
      let gbufData

      // Handle different data formats
      if (typeof data.data === 'string') {
        try {
          gbufData = JSON.parse(data.data)
        } catch (parseError) {
          console.error('Failed to parse graphics data JSON:', parseError)
          renderStats.value = `Parse error: ${parseError.message}`
          return
        }
      } else if (typeof data.data === 'object') {
        gbufData = data.data
      } else {
        console.warn('Unexpected graphics data format:', typeof data.data)
        renderStats.value = 'Unexpected data format'
        return
      }

      if (gbufData && gbufData.commands && Array.isArray(gbufData.commands)) {
        // Store the data from datapoint for redraws
        lastGbufData = gbufData
        renderGbufCommands(gbufData)
      } else {
        console.warn('No commands found in gbuf data:', gbufData)
        renderStats.value = 'No commands found in gbuf data'
      }

    } catch (error) {
      console.error('Failed to process graphics data:', error)
      renderStats.value = `Processing error: ${error.message}`
    }
  }

  // Initialize canvas and event listeners
  const initializeRenderer = async () => {
    await nextTick()

    if (!canvasRef.value) {
      console.error('Canvas ref not available')
      return
    }

    ctx = canvasRef.value.getContext('2d')

    // Clear initial canvas
    ctx.fillStyle = 'white'
    ctx.fillRect(0, 0, config.width, config.height)

    console.log(`Graphics renderer initialized for ${config.streamId} (${config.width}x${config.height})`)

    // Register component with new dserv system
    const componentCleanup = dserv.registerComponent(`GraphicsRenderer-${config.streamId}`)
    cleanupFunctions.push(componentCleanup)

    // Subscribe to graphics data using new dserv event system
    const dataUnsubscribe = dserv.on(
      `datapoint:${config.streamId}`,
      handleGraphicsData,
      `GraphicsRenderer-${config.streamId}`
    )
    cleanupFunctions.push(dataUnsubscribe)

    // Watch connection status
    const connectionUnsubscribe = dserv.on(
      'connection',
      (connectionData) => {
        isConnected.value = connectionData.connected
        if (!connectionData.connected) {
          renderStats.value = 'Connection lost'
        } else if (!dataReceived.value) {
          renderStats.value = 'Canvas ready, waiting for data...'
        }
      },
      `GraphicsRenderer-${config.streamId}`
    )
    cleanupFunctions.push(connectionUnsubscribe)

    // Set initial connection status
    isConnected.value = dserv.state.connected

    console.log(`Graphics renderer listening for: datapoint:${config.streamId}`)
  }

  // Cleanup function
  const cleanup = () => {
    cleanupFunctions.forEach(fn => fn && fn())
    cleanupFunctions.length = 0
    console.log(`Graphics renderer cleaned up: ${config.streamId}`)
  }

  // Manual rendering function (for testing)
  const renderData = (gbufData) => {
    renderGbufCommands(gbufData)
  }

  // Resize canvas and update scaling
  const resizeCanvas = (newWidth, newHeight) => {
    if (canvasRef.value) {
      config.width = newWidth
      config.height = newHeight
      canvasRef.value.width = newWidth
      canvasRef.value.height = newHeight

      // Recalculate scaling if we have window bounds
      if (windowBounds && config.autoScale) {
        const sourceWidth = windowBounds.urx - windowBounds.llx
        const sourceHeight = windowBounds.ury - windowBounds.lly
        updateScaling(sourceWidth, sourceHeight)
      }

      console.log(`Canvas resized to ${newWidth}x${newHeight}`)

      // Redraw the last graphics data if available
      if (lastGbufData) {
        renderGbufCommands(lastGbufData)
      } else {
        // Just clear to white if no data to redraw
        if (ctx) {
          ctx.fillStyle = 'white'
          ctx.fillRect(0, 0, newWidth, newHeight)
        }
      }
    }
  }

  // Watch for canvas size changes if reactive
  if (options.watchSize) {
    watch(() => [options.width, options.height], ([newWidth, newHeight]) => {
      if (newWidth && newHeight) {
        resizeCanvas(newWidth, newHeight)
      }
    })
  }

  // Lifecycle hooks
  onMounted(initializeRenderer)
  onUnmounted(cleanup)

  return {
    renderStats: readonly(renderStats),
    lastUpdate: readonly(lastUpdate),
    dataReceived: readonly(dataReceived),
    isConnected: readonly(isConnected),
    renderData, // For manual testing
    resizeCanvas, // For dynamic resizing
    config: readonly(config)
  }
}
