// Enhanced useGraphicsRenderer.js with debugging for text issues
import { ref, onMounted, onUnmounted, nextTick, readonly, getCurrentInstance } from 'vue'

export function useGraphicsRenderer(canvasRef, options = {}) {
  // Reactive state
  const renderStats = ref('No renders yet')
  const lastUpdate = ref(null)
  const dataReceived = ref(false)
  
  // Configuration
  const config = {
    width: options.width || 640,
    height: options.height || 480,
    streamId: options.streamId || 'graphics/default',
    autoConnect: options.autoConnect ?? true,
    ...options
  }
  
  // Internal state
  let ctx = null
  let currentColor = '#000000'
  let currentPos = { x: 0, y: 0 }
  let currentFont = 'Helvetica'
  let currentFontSize = 10
  let currentJustification = 0  // 0=left, 1=center, 2=right
  let currentOrientation = 0    // 0=0°, 1=270°, 2=180°, 3=90°
  let windowBounds = null
  let unsubscribeFunctions = []
  
  // DEBUG: Add debugging flags
  const DEBUG_TEXT = false  // Disabled - no more verbose text logging
  const DISABLE_CLIPPING = true  // Keep clipping disabled since it was the issue
  const DISABLE_SAVE_RESTORE = false
  
  // Get Vue instance to access global properties
  const instance = getCurrentInstance()
  
  // Y coordinate flip helper
  const flipY = (y) => config.height - y
  
  // Color mapping
  const colorToHex = (colorIndex) => {
    const colors = [
      '#000000', '#0000FF', '#008000', '#00FFFF', '#FF0000',
      '#FF00FF', '#A52A2A', '#FFFFFF', '#808080', '#ADD8E6',
      '#00FF00', '#E0FFFF', '#FF1493', '#9370DB', '#FFFF00'
    ]
    return colors[colorIndex] || '#000000'
  }
  
  // Execute individual graphics command
  const executeGBCommand = (commandObj) => {
    const { cmd, args } = commandObj
    
    try {
      switch (cmd) {
        case 'setwindow':
          windowBounds = {
            llx: args[0], lly: args[1],
            urx: args[2], ury: args[3]
          }
          // Only log if debugging is enabled
          if (DEBUG_TEXT) console.log('📐 Window bounds set:', windowBounds)
          break
          
        case 'setcolor':
          currentColor = colorToHex(args[0])
          ctx.strokeStyle = currentColor
          ctx.fillStyle = currentColor
          if (DEBUG_TEXT) console.log('🎨 Color set to:', currentColor, 'from index:', args[0])
          break
          
        case 'setjust':
          // args: [justification] - -1=left, 0=center, 1=right
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
          if (DEBUG_TEXT) console.log('📝 Text justification set to:', currentJustification, 'from value:', justValue)
          break
          
        case 'setorientation':
          // args: [orientation] - 0=0°, 1=270°, 2=180°, 3=90°
          const orientValue = args[0]
          const orientationMap = { 0: 0, 1: 270, 2: 180, 3: 90 }
          currentOrientation = orientationMap[orientValue] !== undefined ? orientationMap[orientValue] : 0
          if (DEBUG_TEXT) console.log('🔄 Text orientation set to:', currentOrientation, 'from value:', orientValue)
          break
          
        case 'setfont':
          currentFont = args[0] || 'Helvetica'
          currentFontSize = args[1] || 10
          ctx.font = `${currentFontSize}px ${currentFont}`
          if (DEBUG_TEXT) console.log('🔤 Font set to:', currentFont, currentFontSize + 'px')
          break
          
        case 'gsave':
          if (!DISABLE_SAVE_RESTORE) {
            ctx.save()
            if (DEBUG_TEXT) console.log('💾 Graphics state saved')
          } else {
            if (DEBUG_TEXT) console.log('💾 Graphics state save DISABLED for debugging')
          }
          break
          
        case 'grestore':
          if (!DISABLE_SAVE_RESTORE) {
            ctx.restore()
            // Re-apply current state after restore
            ctx.strokeStyle = currentColor
            ctx.fillStyle = currentColor
            ctx.font = `${currentFontSize}px ${currentFont}`
            if (DEBUG_TEXT) console.log('🔄 Graphics state restored, re-applied current settings')
          } else {
            if (DEBUG_TEXT) console.log('🔄 Graphics state restore DISABLED for debugging')
          }
          break
          
        case 'setclipregion':
          if (!DISABLE_CLIPPING) {
            // args: [llx, lly, urx, ury]
            const clipX = args[0]
            const clipY = flipY(args[3])
            const clipWidth = args[2] - args[0]
            const clipHeight = args[3] - args[1]
            
            ctx.beginPath()
            ctx.rect(clipX, clipY, clipWidth, clipHeight)
            ctx.clip()
            if (DEBUG_TEXT) console.log('✂️ Clip region set:', clipX, clipY, clipWidth, clipHeight)
          } else {
            if (DEBUG_TEXT) console.log('✂️ Clipping DISABLED for debugging. Would set:', args)
          }
          break
          
        case 'circle':
        case 'fcircle':
          ctx.beginPath()
          ctx.arc(args[0], flipY(args[1]), args[2], 0, 2 * Math.PI)
          const filled = (cmd === 'fcircle') || args[3]
          if (filled) {
            ctx.fill()
          } else {
            ctx.stroke()
          }
          break
          
        case 'line':
          ctx.beginPath()
          ctx.moveTo(args[0], flipY(args[1]))
          ctx.lineTo(args[2], flipY(args[3]))
          ctx.stroke()
          break
          
        case 'moveto':
          currentPos = { x: args[0], y: flipY(args[1]) }
          if (DEBUG_TEXT) console.log('📍 Moved to:', currentPos)
          break
          
        case 'lineto':
          ctx.beginPath()
          ctx.moveTo(currentPos.x, currentPos.y)
          ctx.lineTo(args[0], flipY(args[1]))
          ctx.stroke()
          currentPos = { x: args[0], y: flipY(args[1]) }
          break
          
        case 'filledrect':
          const width = args[2] - args[0]
          const height = args[3] - args[1]
          ctx.fillRect(args[0], flipY(args[3]), width, height)
          break
          
        case 'drawtext':
          if (currentPos && currentPos.x !== undefined && currentPos.y !== undefined && args[0]) {
            const text = String(args[0])
            
            if (DEBUG_TEXT) {
              console.log('📝 Drawing text:', text)
              console.log('   Position:', currentPos)
              console.log('   Font:', ctx.font)
              console.log('   Color:', ctx.fillStyle)
              console.log('   Justification:', currentJustification)
              console.log('   Orientation:', currentOrientation)
            }
            
            ctx.save()
            
            // Ensure text properties are set correctly
            ctx.font = `${currentFontSize}px ${currentFont}`
            ctx.fillStyle = currentColor
            ctx.textAlign = ['left', 'center', 'right'][currentJustification] || 'left'
            ctx.textBaseline = 'middle'
            
            const x = currentPos.x
            const y = currentPos.y
            
            if (DEBUG_TEXT) {
              console.log('   Final canvas font:', ctx.font)
              console.log('   Final canvas fillStyle:', ctx.fillStyle)
              console.log('   Final canvas textAlign:', ctx.textAlign)
              console.log('   Final position:', x, y)
            }
            
            if (currentOrientation !== 0) {
              ctx.translate(x, y)
              ctx.rotate((currentOrientation * Math.PI) / 180)
              ctx.fillText(text, 0, 0)
              if (DEBUG_TEXT) console.log('   Text drawn with rotation:', currentOrientation)
            } else {
              ctx.fillText(text, x, y)
              if (DEBUG_TEXT) console.log('   Text drawn normally at:', x, y)
            }
            
            // DEBUG: Check if text was actually drawn by sampling pixels
            if (DEBUG_TEXT) {
              const imageData = ctx.getImageData(Math.max(0, x-10), Math.max(0, y-10), 20, 20)
              const hasNonWhitePixels = Array.from(imageData.data).some((val, idx) => 
                idx % 4 < 3 && val < 255  // Check RGB channels for non-white pixels
              )
              console.log('   Pixel check - non-white pixels found near text:', hasNonWhitePixels)
            }
            
            // Debug: Draw a small marker at text position BEFORE restore
            if (DEBUG_TEXT) {
              const originalFillStyle = ctx.fillStyle
              ctx.fillStyle = 'red'
              // Draw marker at the same position where text was drawn
              if (currentOrientation !== 0) {
                // For rotated text, marker is already in the right coordinate system
                ctx.fillRect(-2, -2, 4, 4)
              } else {
                // For normal text, use original coordinates
                ctx.fillRect(x - 2, y - 2, 4, 4)
              }
              ctx.fillStyle = originalFillStyle
              console.log('   Red marker drawn, fillStyle restored to:', ctx.fillStyle)
            }
            
            ctx.restore()
            
            if (DEBUG_TEXT) {
              console.log('   Context restored, final fillStyle:', ctx.fillStyle)
            }
            
            if (DEBUG_TEXT) console.log('   Text rendering complete')
          } else {
            if (DEBUG_TEXT) {
              console.log('📝 Text NOT drawn - missing data:')
              console.log('   currentPos:', currentPos)
              console.log('   text:', args[0])
            }
          }
          break
          
        case 'setlstyle':
          // Line style - disabled for now due to canvas state issues
          // TODO: Fix line dash state management
          if (DEBUG_TEXT) console.log('🖊️ Line style command ignored (disabled)')
          break
          
        case 'setlwidth':
          ctx.lineWidth = Math.max(1, args[0] / 100)
          break
          
        default:
          // Silently ignore unknown commands
          break
      }
    } catch (error) {
      console.error('🎨 Error executing command:', cmd, args, error)
    }
  }
  
  // Render complete gbuf command set
  const renderGbufCommands = (gbufData) => {
    if (!ctx || !gbufData) return
    
    if (DEBUG_TEXT) console.log('🎨 Starting render with', gbufData.commands?.length || 0, 'commands')
    
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
    ctx.textBaseline = 'middle'  // Better for general text positioning
    ctx.setLineDash([])  // Reset to solid lines
    
    // Initialize state variables
    currentColor = '#000000'
    currentPos = { x: 0, y: 0 }
    windowBounds = null
    currentFont = 'Helvetica'
    currentFontSize = 10
    currentJustification = 0
    currentOrientation = 0
    
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
      
      if (DEBUG_TEXT && textCommandCount > 0) {
        console.log('🎨 Render complete:', statsMessage)
      }
      
    } catch (error) {
      console.error('🎨 Render error:', error)
      renderStats.value = `Render error: ${error.message}`
    }
  }
  
  // Handle incoming graphics data
  const handleGraphicsData = (data) => {
    dataReceived.value = true
    lastUpdate.value = new Date().toLocaleTimeString()
    
    try {
      let gbufData = (typeof data.data === 'string') ? JSON.parse(data.data) : data.data
      
      if (gbufData && gbufData.commands && Array.isArray(gbufData.commands)) {
        renderGbufCommands(gbufData)
      } else {
        console.warn('🎨 No commands found in gbuf data')
        renderStats.value = 'No commands found in gbuf data'
      }
      
    } catch (error) {
      console.error('🎨 Failed to parse graphics data:', error)
      renderStats.value = `Parse error: ${error.message}`
    }
  }
  
  // Initialize canvas and dserv connection
  const initializeRenderer = async () => {
    await nextTick()
    
    if (!canvasRef.value) {
      console.error('🎨 Canvas ref not available')
      return
    }
    
    ctx = canvasRef.value.getContext('2d')
    
    // Clear initial canvas
    ctx.fillStyle = 'white'
    ctx.fillRect(0, 0, config.width, config.height)
    
    if (DEBUG_TEXT) console.log('🎨 Canvas initialized:', config.width, 'x', config.height)
    
    // Setup dserv connection if available and auto-connect enabled
    if (config.autoConnect) {
      // Try to get $dserv from Vue instance global properties
      const $dserv = instance?.appContext?.config?.globalProperties?.$dserv || window.$dserv
      
      if ($dserv) {
        const connectionUnsub = $dserv.on('connection', (data) => {
          if (data.connected) {
            console.log('🎨 Subscribing to graphics stream:', config.streamId)
            $dserv.subscribe(config.streamId)
          }
        }, `GraphicsRenderer-${config.streamId}`)
        
        const graphicsUnsub = $dserv.on(`datapoint:${config.streamId}`, handleGraphicsData, `GraphicsRenderer-${config.streamId}`)
        
        unsubscribeFunctions.push(connectionUnsub, graphicsUnsub)
        
        // Check current connection state
        if ($dserv.state && $dserv.state.connected) {
          $dserv.subscribe(config.streamId)
        }
      } else {
        console.warn('🎨 No dserv connection available')
        renderStats.value = 'No dserv connection available'
      }
    }
  }
  
  // Cleanup function
  const cleanup = () => {
    unsubscribeFunctions.forEach(fn => fn && fn())
    unsubscribeFunctions = []
  }
  
  // Manual rendering function (for non-dserv usage)
  const renderData = (gbufData) => {
    renderGbufCommands(gbufData)
  }
  
  // Lifecycle hooks
  onMounted(initializeRenderer)
  onUnmounted(cleanup)
  
  // Return public API
  return {
    // Reactive state
    renderStats: readonly(renderStats),
    lastUpdate: readonly(lastUpdate),
    dataReceived: readonly(dataReceived),
    
    // Methods
    renderData,
    handleGraphicsData,
    cleanup,
    
    // Config access
    config
  }
}