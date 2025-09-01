// useGraphicsRenderer.js - Enhanced with proper responsive support
import { ref, onUnmounted, nextTick, readonly } from 'vue'
import { dserv } from '@/services/dserv.js'

export function useGraphicsRenderer(canvasRef, options = {}) {
  // Reactive state
  const renderStats = ref('Canvas ready, waiting for data...')
  const lastUpdate = ref(null)
  const dataReceived = ref(false)
  const isConnected = ref(false)

  // Configuration - now mutable to support resizing
  const config = {
    width: options.width || 640,
    height: options.height || 480,
    streamId: options.streamId || 'graphics/main',
    autoScale: options.autoScale !== false,
    backgroundColor: options.backgroundColor || '#ffffff',
    ...options
  }

  const onWindowBoundsChange = options.onWindowBoundsChange || (() => {})

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
  let lastGbufData = null
  let currentBackgroundColor = config.backgroundColor // Track current background

  // Cleanup tracking
  const cleanupFunctions = []

  // Coordinate transformation with current canvas size
  const transformX = (x) => x * scaleX
  const transformY = (y) => config.height - (y * scaleY)
  const transformWidth = (w) => w * scaleX
  const transformHeight = (h) => h * scaleY

  // Color mapping
  // Color mapping
  const colorToHex = (colorIndex) => {
    // Standard palette colors (0-14)
    const colors = [
      '#000000', '#0000FF', '#008000', '#00FFFF', '#FF0000',
      '#FF00FF', '#A52A2A', '#FFFFFF', '#808080', '#ADD8E6',
      '#00FF00', '#E0FFFF', '#FF1493', '#9370DB', '#FFFF00'
    ]

    // If it's a standard palette color, return it
    if (colorIndex >= 0 && colorIndex < colors.length) {
      return colors[colorIndex]
    }

    // Handle packed RGB colors (special colors > 18)
    // Format: (r << 21) + (g << 13) + (b << 5)
    if (colorIndex > 18) {
      const r = (colorIndex >> 21) & 0xFF
      const g = (colorIndex >> 13) & 0xFF
      const b = (colorIndex >> 5) & 0xFF

      // Convert to hex string
      const rHex = r.toString(16).padStart(2, '0')
      const gHex = g.toString(16).padStart(2, '0')
      const bHex = b.toString(16).padStart(2, '0')

      return `#${rHex}${gHex}${bHex}`
    }

    // Default to black for unknown colors
    return '#000000'
  }

  // Calculate scaling factors with current canvas dimensions
  const updateScaling = (sourceWidth, sourceHeight) => {
    if (config.autoScale && sourceWidth && sourceHeight) {
      scaleX = config.width / sourceWidth
      scaleY = config.height / sourceHeight
      console.log(`Graphics scaling updated: source ${sourceWidth}×${sourceHeight} → canvas ${config.width}×${config.height} (${scaleX.toFixed(3)}x, ${scaleY.toFixed(3)}y)`)
    } else {
      scaleX = 1
      scaleY = 1
    }
  }

  // Execute graphics commands (unchanged)
  const executeGBCommand = (commandObj) => {
    const { cmd, args } = commandObj

    try {
      switch (cmd) {
        case 'setwindow':
          windowBounds = {
            llx: args[0], lly: args[1],
            urx: args[2], ury: args[3]
          }

          // Notify component about window bounds change
          onWindowBoundsChange(windowBounds)

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

        case 'setbackground':
          const bgColor = colorToHex(args[0])
          currentBackgroundColor = bgColor // Store new background color
          ctx.fillStyle = bgColor
          ctx.fillRect(0, 0, config.width, config.height)
          // Restore current drawing color after background fill
          ctx.fillStyle = currentColor
          break

        case 'setjust':
          const justValue = args[0]
          if (justValue === -1) {
            currentJustification = 0
          } else if (justValue === 0) {
            currentJustification = 1
          } else if (justValue === 1) {
            currentJustification = 2
          } else {
            currentJustification = 1
          }
          break

        case 'setorientation':
          const orientValue = args[0]
          const orientationMap = { 0: 0, 1: 270, 2: 180, 3: 90 }
          currentOrientation = orientationMap[orientValue] !== undefined ? orientationMap[orientValue] : 0
          break

        case 'setfont':
          currentFont = args[0] || 'Helvetica'
          currentFontSize = (args[1] || 10) * Math.min(scaleX, scaleY)
          ctx.font = `${currentFontSize}px ${currentFont}`
          break

        case 'gsave':
          ctx.save()
          break

        case 'grestore':
          ctx.restore()
          ctx.strokeStyle = currentColor
          ctx.fillStyle = currentColor
          ctx.font = `${currentFontSize}px ${currentFont}`
          break

        case 'setclipregion':
          // Clipping disabled for stability
          break

        case 'circle':
        case 'fcircle':
          ctx.beginPath()
          ctx.arc(
            transformX(args[0]),
            transformY(args[1]),
            transformWidth(args[2]),
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
          ctx.moveTo(transformX(args[0]), transformY(args[1]))
          ctx.lineTo(transformX(args[2]), transformY(args[3]))
          ctx.stroke()
          break

        case 'moveto':
          currentPos = {
            x: transformX(args[0]),
            y: transformY(args[1])
          }
          break

        case 'lineto':
          ctx.beginPath()
          ctx.moveTo(currentPos.x, currentPos.y)
          const newX = transformX(args[0])
          const newY = transformY(args[1])
          ctx.lineTo(newX, newY)
          ctx.stroke()
          currentPos = { x: newX, y: newY }
          break

        case 'frect':
        case 'filledrect':
          const x1 = transformX(args[0])
          const y1 = transformY(args[1])
          const x2 = transformX(args[2])
          const y2 = transformY(args[3])
          const width = Math.abs(x2 - x1)
          const height = Math.abs(y2 - y1)
          ctx.fillRect(
            Math.min(x1, x2),
            Math.min(y1, y2),
            width,
            height
          )
          break

        case 'poly':
        case 'fpoly':
          if (args.length >= 6 && args.length % 2 === 0) {
            ctx.beginPath()

            // Move to first point
            ctx.moveTo(transformX(args[0]), transformY(args[1]))

            // Draw lines to subsequent points
            for (let i = 2; i < args.length; i += 2) {
              ctx.lineTo(transformX(args[i]), transformY(args[i + 1]))
            }

            // Close the polygon
            ctx.closePath()

            // Fill or stroke based on command
            if (cmd === 'fpoly') {
              ctx.fill()
            } else {
              ctx.stroke()
            }
          } else {
            console.warn(`Invalid ${cmd} command: need even number of args >= 6, got ${args.length}`, args)
          }
          break

        case 'drawtext':
          if (currentPos && currentPos.x !== undefined && currentPos.y !== undefined && args[0]) {
            const text = String(args[0])

            ctx.save()
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

    lastGbufData = gbufData

    // Clear canvas with current background color
    ctx.fillStyle = currentBackgroundColor
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

      const statsMessage = `Rendered ${commandCount} commands (${textCommandCount} text) at ${new Date().toLocaleTimeString()} [${config.width}×${config.height}]`
      renderStats.value = statsMessage
      lastUpdate.value = new Date().toLocaleTimeString()

    } catch (error) {
      console.error('Render error:', error)
      renderStats.value = `Render error: ${error.message}`
    }
  }

  // Handle incoming graphics data
  const handleGraphicsData = (data) => {
    dataReceived.value = true
    lastUpdate.value = new Date().toLocaleTimeString()

    try {
      let gbufData

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

  // Initialize renderer
  const initializeRenderer = async () => {
    await nextTick()

    if (!canvasRef.value) {
      console.error('Canvas ref not available')
      return
    }

    ctx = canvasRef.value.getContext('2d')

    // Clear initial canvas with background color
    ctx.fillStyle = config.backgroundColor
    ctx.fillRect(0, 0, config.width, config.height)

    console.log(`Graphics renderer initialized for ${config.streamId} (${config.width}×${config.height})`)

    // Register with dserv
    const componentCleanup = dserv.registerComponent(`GraphicsRenderer-${config.streamId}`)
    cleanupFunctions.push(componentCleanup)

    // Subscribe to graphics data
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

    isConnected.value = dserv.state.connected
    console.log(`Graphics renderer listening for: datapoint:${config.streamId}`)
  }

  // Enhanced resize function for responsive support
  const resizeCanvas = (newWidth, newHeight) => {
    if (!canvasRef.value) {
      console.warn('Cannot resize: canvas ref not available')
      return
    }

    const oldWidth = config.width
    const oldHeight = config.height

    // Update config
    config.width = newWidth
    config.height = newHeight

    // Update actual canvas element
    canvasRef.value.width = newWidth
    canvasRef.value.height = newHeight

    console.log(`Canvas resized from ${oldWidth}×${oldHeight} to ${newWidth}×${newHeight}`)

    // Get fresh context after resize
    ctx = canvasRef.value.getContext('2d')

    // Recalculate scaling if we have window bounds
    if (windowBounds && config.autoScale) {
      const sourceWidth = windowBounds.urx - windowBounds.llx
      const sourceHeight = windowBounds.ury - windowBounds.lly
      updateScaling(sourceWidth, sourceHeight)
    }

    // Redraw last graphics data with new size
    if (lastGbufData) {
      console.log('Redrawing graphics data after resize...')
      renderGbufCommands(lastGbufData)
    } else {
      // Clear to white if no data to redraw
      ctx.fillStyle = 'white'
      ctx.fillRect(0, 0, newWidth, newHeight)
    }
  }

  // Cleanup
  const cleanup = () => {
    cleanupFunctions.forEach(fn => fn && fn())
    cleanupFunctions.length = 0
    console.log(`Graphics renderer cleaned up: ${config.streamId}`)
  }

  // Get scaling info
  const getScaleInfo = () => {
    return { x: scaleX, y: scaleY }
  }

  // Get source dimensions
  const getSourceDimensions = () => {
    if (windowBounds) {
      return {
        width: windowBounds.urx - windowBounds.llx,
        height: windowBounds.ury - windowBounds.lly
      }
    }
    return null
  }

  // Manual render function
  const renderData = (gbufData) => {
    renderGbufCommands(gbufData)
  }

  // Initialize immediately
  initializeRenderer()

  // Return API
  return {
    renderStats: readonly(renderStats),
    lastUpdate: readonly(lastUpdate),
    dataReceived: readonly(dataReceived),
    isConnected: readonly(isConnected),
    renderData,
    resizeCanvas, // Enhanced for responsive support
    getScaleInfo,
    getSourceDimensions,
    dispose: cleanup,
    config // Mutable config for size tracking
  }
}
