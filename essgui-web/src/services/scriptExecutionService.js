// ScriptExecutionService.js - Fixed version with proper event handling
import { reactive, ref } from 'vue'
import { dserv } from './dserv.js'
import { eventService } from './eventService.js'

class ScriptExecutionService {
  constructor() {
    this.activeScripts = reactive(new Map())
    this.scriptContexts = new Map()
    this.canvasRegistry = new Map() // Track multiple canvases
    this.eventHandlers = new Map() // Simple key -> array of handlers
    this.isInitialized = false

    // Script state management
    this.globalScriptData = reactive({})
    this.scriptErrors = reactive([])
  }

  async initialize() {
    if (this.isInitialized) return

    // Listen for visualization scripts from backend
    dserv.on('datapoint:ess/visualization_scripts', (data) => {
      console.log('Received visualization scripts from ESS:', data.data)
      this.handleVisualizationScripts(data.data)
    }, 'ScriptExecutionService')

    // IMPORTANT: Connect to event service for routing events to scripts
    eventService.addHandler((event) => {
      this.routeEventToScripts(event)
    })

    // Listen for per-trial data updates
    dserv.on('datapoint:ess/trial_data', (data) => {
      this.updateTrialData(data.data)
    }, 'ScriptExecutionService_TrialData')

    this.isInitialized = true
    console.log('ScriptExecutionService initialized with event routing')
  }

  // Route events from eventService to script handlers
    routeEventToScripts(event) {
  if (event.type === 'obs_reset' || event.type === 'name_update') {
    return
  }

  // Create key format that matches registration: "type,subtype"
  const key = `${event.type},${event.subtype}`
  const handlers = this.eventHandlers.get(key) || []

//  if (handlers.length > 0) {
//    console.log(`Routing event ${key} to ${handlers.length} script handlers`)
//    console.log('Event data:', event)
//  }

    handlers.forEach(handlerInfo => {
      try {
        handlerInfo.handler(event)
      } catch (error) {
        console.error(`Event handler error for ${key}:`, error)
        this.scriptErrors.push({
          scriptId: handlerInfo.scriptId,
          canvasId: handlerInfo.canvasId,
          error: error.message,
          timestamp: Date.now()
        })
      }
    })
  }

  updateTrialData(trialData) {
    if (!trialData) return

    // Route data to any script that has registered an update handler
    const key = '__trial_update_handler__'
    const handlers = this.eventHandlers.get(key) || []

    handlers.forEach(handlerInfo => {
      try {
        // The handler is the script's update function
        handlerInfo.handler(trialData)
      } catch (error) {
        console.error(`Trial update handler error for script ${handlerInfo.scriptId}:`, error)
        this.scriptErrors.push({
          scriptId: handlerInfo.scriptId,
          canvasId: handlerInfo.canvasId,
          error: error.message,
          timestamp: Date.now()
        })
      }
    })
  }

  // Register a canvas for script drawing
  registerCanvas(canvasId, canvasContext, metadata = {}) {
    if (!canvasId || typeof canvasId !== 'string') {
      console.error('Invalid canvasId provided to registerCanvas:', canvasId)
      return () => {}
    }

    if (!canvasContext) {
      console.error('No canvas context provided to registerCanvas for:', canvasId)
      return () => {}
    }

    console.log(`Registering canvas: ${canvasId}`)

    this.canvasRegistry.set(canvasId, {
      ctx: canvasContext,
      metadata: {
        width: metadata.width || 300,
        height: metadata.height || 300,
        degreesHorizontal: metadata.degreesHorizontal || 20,
        degreesVertical: metadata.degreesVertical || 20,
        type: metadata.type || 'general',
        ...metadata
      },
      elements: new Map(), // Custom elements for this canvas
      drawCallbacks: new Set() // Custom draw functions
    })

    console.log(`Successfully registered canvas: ${canvasId}`)
    console.log(`Total registered canvases: ${this.canvasRegistry.size}`)

    return () => this.unregisterCanvas(canvasId)
  }

  unregisterCanvas(canvasId) {
    this.canvasRegistry.delete(canvasId)
    console.log(`Unregistered canvas: ${canvasId}`)
  }

// Fixed createDrawingAPI method - replace the entire method in ScriptExecutionService.js
createDrawingAPI(canvasId) {
  const canvasData = this.canvasRegistry.get(canvasId)
  if (!canvasData) {
    console.warn(`Canvas ${canvasId} not found`)
    return null
  }

  const { metadata } = canvasData

  // Helper function for coordinate conversion
  const degreesToPixels = (degX, degY) => {
    const centerX = metadata.width / 2
    const centerY = metadata.height / 2
    const pixPerDegX = metadata.width / metadata.degreesHorizontal
    const pixPerDegY = metadata.height / metadata.degreesVertical
    return {
      x: centerX + (degX * pixPerDegX),
      y: centerY - (degY * pixPerDegY) // Y inverted
    }
  }

  // Define the API object with proper references
  const api = {
    // Canvas info
    getCanvasSize: () => ({ width: metadata.width, height: metadata.height }),
    getVisualRange: () => ({
      horizontal: metadata.degreesHorizontal,
      vertical: metadata.degreesVertical
    }),

    // Coordinate conversion
    degreesToPixels,

    pixelsToDegrees: (pixX, pixY) => {
      const centerX = metadata.width / 2
      const centerY = metadata.height / 2
      const pixPerDegX = metadata.width / metadata.degreesHorizontal
      const pixPerDegY = metadata.height / metadata.degreesVertical
      return {
        x: (pixX - centerX) / pixPerDegX,
        y: -1 * (pixY - centerY) / pixPerDegY // Y inverted
      }
    },

    // Element management
    addElement: (element) => {
      const id = element.id || `elem_${Date.now()}_${Math.random().toString(36).substr(2, 6)}`
      canvasData.elements.set(id, {
        id,
        type: element.type || 'circle',
        x: element.x || 0, // degrees
        y: element.y || 0, // degrees
        visible: element.visible !== false,
        zIndex: element.zIndex || 0,
        created: Date.now(),
        ...element
      })
      return id
    },

    updateElement: (id, updates) => {
      if (canvasData.elements.has(id)) {
        const element = canvasData.elements.get(id)
        Object.assign(element, updates)
        return true
      }
      return false
    },

    removeElement: (id) => {
      return canvasData.elements.delete(id)
    },

    clearElements: () => {
      canvasData.elements.clear()
    },

      drawCircle: (x, y, radius, options = {}) => {
	  // REMOVED: Immediate drawing. We now only add/update the element list.
	  // The renderCanvas loop is the single source of truth for drawing.

	  // store the element for persistent rendering
	  const id = options.id || `circle_${Date.now()}_${Math.random().toString(36).substr(2, 6)}`
	  return api.addElement({
	      id: id,
	      type: 'circle',
	      x, y, radius,
	      ...options
	  })
      },

      drawRectangle: (x, y, width, height, options = {}) => {
	  // REMOVED: Immediate drawing.

	  const id = options.id || `rectangle_${Date.now()}_${Math.random().toString(36).substr(2, 6)}`
	  return api.addElement({
	      id: id,
	      type: 'rectangle',
	      x, y, width, height,
	      ...options
	  })
      },

      drawText: (x, y, text, options = {}) => {
	  // REMOVED: Immediate drawing.

	  const id = options.id || `text_${Date.now()}_${Math.random().toString(36).substr(2, 6)}`
	  return api.addElement({
	      id: id,
	      type: 'text',
	      x, y, text,
	      ...options
	  })
      },

      drawLine: (x1, y1, x2, y2, options = {}) => {
	  // REMOVED: Immediate drawing.

	  const id = options.id || `line_${Date.now()}_${Math.random().toString(36).substr(2, 6)}`
          return api.addElement({
              id: id,
              type: 'line',
              x1, y1, x2, y2,
              ...options
          })
      },

      // Custom draw function registration
      registerDrawFunction: (func) => {
	  canvasData.drawCallbacks.add(func)
	  return () => canvasData.drawCallbacks.delete(func)
      }
  }

    return api
}

  // Register event handler for scripts
registerEventHandler(eventKey, handler, scriptId, canvasId) {
  let keys = [];

  // SPECIAL CASE: Handle the trial update handler
  if (eventKey === 'trial_update') {
    keys = ['__trial_update_handler__']
  } else if (Array.isArray(eventKey)) {
    // Array format: [type, subtype] - exact match
    const key = eventKey.join(',');
    keys = [key];
  } else if (typeof eventKey === 'number') {
    // Single number: match any subtype for this event type
    const type = eventKey;

    // Generate keys for common subtypes (0-5 should cover most cases)
    keys = [`${type},0`, `${type},1`, `${type},2`, `${type},3`, `${type},4`, `${type},5`];
  } else {
    // String format: use as-is
    keys = [String(eventKey)];
  }

  // Store the drawing API with each handler
  const drawAPI = this.createDrawingAPI(canvasId);

  // Register handler for all generated keys
  keys.forEach(key => {
    if (!this.eventHandlers.has(key)) {
      this.eventHandlers.set(key, []);
    }

    // Wrap the handler to provide draw API
    const wrappedHandler = (event) => {
      const originalDraw = globalThis.draw;
      globalThis.draw = drawAPI;

      try {
        handler(event);
      } finally {
        if (originalDraw !== undefined) {
          globalThis.draw = originalDraw;
        } else {
          delete globalThis.draw;
        }
      }
    };

    this.eventHandlers.get(key).push({
      handler: wrappedHandler,
      scriptId: scriptId,
      canvasId: canvasId
    });
  });
}



  // SIMPLE script execution using eval
  executeScript(scriptCode, scriptId, canvasId) {
    try {
//      console.log(`Executing script ${scriptId} for canvas ${canvasId}`)

      const canvasData = this.canvasRegistry.get(canvasId)
      if (!canvasData) {
        throw new Error(`Canvas ${canvasId} not found`)
      }

      // Create drawing API
      const drawingAPI = this.createDrawingAPI(canvasId)

      // Store current context for cleanup
      const originalVars = {}

      // Create safe execution environment
      const scriptGlobals = {
        // Drawing API
        draw: drawingAPI,

        // Event registration - THIS IS THE KEY FIX!
        registerEventHandler: (eventKey, handler) => {
          this.registerEventHandler(eventKey, handler, scriptId, canvasId)
        },

        // NEW: Register a handler for per-trial data updates
        registerTrialUpdateHandler: (handler) => {
          // We reuse the event handler mechanism with a special key
          this.registerEventHandler('trial_update', handler, scriptId, canvasId)
        },

        // State management
        setData: (key, value) => {
          this.globalScriptData[key] = value
        },

        getData: (key) => {
          return this.globalScriptData[key]
        },

        // System state access
        getSystemState: () => ({
          currentSystem: dserv.state.currentSystem,
          currentProtocol: dserv.state.currentProtocol,
          currentVariant: dserv.state.currentVariant,
          inObs: dserv.state.inObs,
          obsId: dserv.state.obsId,
          obsTotal: dserv.state.obsTotal
        }),

        // Utility functions
        setTimeout: setTimeout,
        clearTimeout: clearTimeout,
        Math: Math,
        Array: Array,
        Object: Object,
        Date: Date
      }

      // Set globals temporarily
      Object.keys(scriptGlobals).forEach(key => {
        if (window[key] !== undefined) {
          originalVars[key] = window[key]
        }
        window[key] = scriptGlobals[key]
      })

      try {
        // Execute script in global scope
        eval(`
          (function() {
            "use strict";
            ${scriptCode}
          })();
        `)

        // Track successful execution
        this.activeScripts.set(scriptId, {
          id: scriptId,
          canvasId,
          code: scriptCode,
          loadedAt: Date.now(),
          status: 'loaded',
          lastExecution: Date.now()
        })

//        console.log(`Successfully executed script ${scriptId}`)

      } finally {
        // Restore original globals
        Object.keys(scriptGlobals).forEach(key => {
          if (originalVars[key] !== undefined) {
            window[key] = originalVars[key]
          } else {
            delete window[key]
          }
        })
      }

    } catch (error) {
      console.error(`Failed to execute script ${scriptId}:`, error)

      this.scriptErrors.push({
        scriptId,
        canvasId,
        error: error.message,
        timestamp: Date.now()
      })

      this.activeScripts.set(scriptId, {
        id: scriptId,
        canvasId,
        code: scriptCode,
        loadedAt: Date.now(),
        status: 'error',
        error: error.message
      })

      throw error
    }
  }

    // Handle visualization scripts from backend
  handleVisualizationScripts(scriptsJson) {
    try {
      const scripts = JSON.parse(scriptsJson || '{}')

      // Clear existing scripts
      this.clearAllScripts()

      // Execute scripts based on key naming convention
      Object.entries(scripts).forEach(([scriptKey, jsCode]) => {
//        console.log(`Processing script: ${scriptKey}`)

        // Check if key specifies a target canvas
        if (scriptKey.includes(':')) {
          // Format: "canvasId:scriptName" - target specific canvas
          const [targetCanvas, scriptName] = scriptKey.split(':', 2)

          if (this.canvasRegistry.has(targetCanvas)) {
            const scriptId = `${scriptName}_${targetCanvas}`
//            console.log(`Executing script ${scriptId} on canvas ${targetCanvas}`)
            this.executeScript(jsCode, scriptId, targetCanvas)
          } else {
            console.warn(`Target canvas "${targetCanvas}" not found. Available:`, Array.from(this.canvasRegistry.keys()))
          }

        } else {
          console.warn(`Script key "${scriptKey}" doesn't match targeting pattern. Use "canvasId:scriptName". Available canvases:`, Array.from(this.canvasRegistry.keys()))
        }
      })

    } catch (error) {
      console.error('Failed to parse visualization scripts:', error)
    }
  }

  // Render canvas elements (called from canvas render loops)
    renderCanvas(canvasId) {

	const canvasData = this.canvasRegistry.get(canvasId)
    if (!canvasData) return

    // Sort elements by zIndex
    const elements = Array.from(canvasData.elements.values())
      .filter(el => el.visible)
      .sort((a, b) => (a.zIndex || 0) - (b.zIndex || 0))

    // Draw elements
    elements.forEach(element => {
      this.drawElement(canvasId, element)
    })

    // Execute custom draw functions
canvasData.drawCallbacks.forEach(drawFunc => {
  try {
    // Provide draw API to custom functions
    const drawAPI = this.createDrawingAPI(canvasId)
    const originalDraw = globalThis.draw
    globalThis.draw = drawAPI

    try {
      drawFunc()
    } finally {
      // Restore original draw
      if (originalDraw !== undefined) {
        globalThis.draw = originalDraw
      } else {
        delete globalThis.draw
      }
    }
  } catch (error) {
    console.error('Custom draw function error:', error)
  }
})
  }

  // Draw individual element
  drawElement(canvasId, element) {
    const canvasData = this.canvasRegistry.get(canvasId)
    if (!canvasData) return

    const { ctx, metadata } = canvasData

    // Calculate position
    const centerX = metadata.width / 2
    const centerY = metadata.height / 2
    const pixPerDegX = metadata.width / metadata.degreesHorizontal
    const pixPerDegY = metadata.height / metadata.degreesVertical
    const pos = {
      x: centerX + (element.x * pixPerDegX),
      y: centerY - (element.y * pixPerDegY) // Y inverted
    }

    ctx.save()
    ctx.globalAlpha = element.opacity || 1

    switch (element.type) {
      case 'circle':
        ctx.fillStyle = element.fillColor || element.color || '#ffffff'
        ctx.strokeStyle = element.strokeColor || '#000000'
        ctx.lineWidth = element.lineWidth || 1

        ctx.beginPath()
        ctx.arc(pos.x, pos.y, element.radius || 5, 0, 2 * Math.PI)
        if (element.fillColor || element.color) ctx.fill()
        if (element.lineWidth > 0) ctx.stroke()
        break

      case 'rectangle': {
	ctx.fillStyle = element.fillColor || element.color || '#ffffff'
	ctx.strokeStyle = element.strokeColor || '#000000'
	ctx.lineWidth = element.lineWidth || 1

	// Convert element width/height from degrees to pixels
	const widthPx = (element.width || 10) * pixPerDegX
	const heightPx = (element.height || 10) * pixPerDegY

	const rectX = pos.x - widthPx / 2
	const rectY = pos.y - heightPx / 2

	// Use pixel values
	if (element.fillColor || element.color) ctx.fillRect(rectX, rectY, widthPx, heightPx)
	if (element.lineWidth > 0) ctx.strokeRect(rectX, rectY, widthPx, heightPx)
	break
      }

      case 'text':
        ctx.fillStyle = element.color || '#ffffff'
        ctx.font = `${element.fontSize || 12}px ${element.fontFamily || 'monospace'}`
        ctx.textAlign = 'center'
        ctx.textBaseline = 'middle'

        ctx.fillText(element.text || '', pos.x, pos.y)
        break
    }

    ctx.restore()
  }

  // Global data management
  setGlobalData(key, value) {
    this.globalScriptData[key] = value
  }

  getGlobalData(key) {
    return this.globalScriptData[key]
  }

  // Cleanup
  clearAllScripts() {
    console.log('Clearing all visualization scripts')
    this.activeScripts.clear()
    this.scriptContexts.clear()
    this.eventHandlers.clear()
    this.scriptErrors.splice(0) // Clear reactive array

    // Clear elements from all canvases
    this.canvasRegistry.forEach(canvasData => {
      canvasData.elements.clear()
      canvasData.drawCallbacks.clear()
    })
  }

  // Status and debugging
  getStatus() {
    return {
      initialized: this.isInitialized,
      activeScripts: Array.from(this.activeScripts.values()),
      registeredCanvases: Array.from(this.canvasRegistry.keys()),
      eventHandlers: this.eventHandlers.size,
      eventHandlerKeys: Array.from(this.eventHandlers.keys()),
      errors: this.scriptErrors,
      globalData: this.globalScriptData
    }
  }

  cleanup() {
    this.clearAllScripts()
    this.canvasRegistry.clear()
    this.isInitialized = false
  }
}

// Singleton instance
export const scriptExecutionService = new ScriptExecutionService()

// Vue composable
export function useScriptExecution() {
  // Ensure service is initialized
  if (!scriptExecutionService.isInitialized) {
    console.log('useScriptExecution: Initializing service...')
    scriptExecutionService.initialize().catch(console.error)
  }

  return {
    registerCanvas: (canvasId, canvasContext, metadata) => {
      console.log(`useScriptExecution: registerCanvas called for ${canvasId}`)
      return scriptExecutionService.registerCanvas(canvasId, canvasContext, metadata)
    },
    renderCanvas: (canvasId) => {
      return scriptExecutionService.renderCanvas(canvasId)
    },
    executeScript: (scriptCode, scriptId, canvasId) => {
      return scriptExecutionService.executeScript(scriptCode, scriptId, canvasId)
    },
    clearCanvas: (canvasId) => {
      const canvasData = scriptExecutionService.canvasRegistry.get(canvasId)
      if (canvasData) {
        canvasData.elements.clear()
        canvasData.drawCallbacks.clear()
      }
    },
    getStatus: () => scriptExecutionService.getStatus(),
    activeScripts: scriptExecutionService.activeScripts,
    globalData: scriptExecutionService.globalScriptData,
    errors: scriptExecutionService.scriptErrors,
    service: scriptExecutionService
  }
}

// Auto-initialize when imported
scriptExecutionService.initialize().catch(console.error)

// Make globally available for debugging
window.scriptExecutionService = scriptExecutionService
