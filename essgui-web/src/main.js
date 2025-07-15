import { createApp } from 'vue'
import App from './App.vue'
import router from './router'
import { createPinia } from 'pinia'
import { dserv } from './services/dserv.js'
import { initializeEventTracking } from './services/eventService.js'

// Import Ant Design Vue styles
import 'ant-design-vue/dist/reset.css'

const app = createApp(App)

app.use(createPinia())
app.use(router)

app.mount('#app')

// Wait for dserv to be connected before starting event tracking
const waitForConnection = () => {
  if (dserv.state.connected) {
    initializeEventTracking(dserv)
  } else {
    dserv.on('connection', (data) => {
      if (data.connected) {
        initializeEventTracking(dserv)
      }
    })
  }
}


// Add this to your main.js file (near the top, after imports)

import { scriptExecutionService } from './services/ScriptExecutionService.js'

// Make service globally accessible for debugging and console access
window.scriptExecutionService = scriptExecutionService

// Add debug helpers
window.debugScriptService = () => {
  console.log('=== Script Service Debug Info ===')
  console.log('Service available:', !!window.scriptExecutionService)
  
  if (window.scriptExecutionService) {
    console.log('Initialized:', window.scriptExecutionService.isInitialized)
    console.log('Active Scripts:', Array.from(window.scriptExecutionService.activeScripts.keys()))
    console.log('Registered Canvases:', Array.from(window.scriptExecutionService.canvasRegistry.keys()))
    console.log('Canvas Registry Size:', window.scriptExecutionService.canvasRegistry.size)
    console.log('Global Data:', window.scriptExecutionService.globalScriptData)
    console.log('Errors:', window.scriptExecutionService.scriptErrors)
    
    // Show canvas details
    window.scriptExecutionService.canvasRegistry.forEach((canvasData, canvasId) => {
      console.log(`Canvas "${canvasId}":`, {
        hasContext: !!canvasData.ctx,
        metadata: canvasData.metadata,
        elements: canvasData.elements.size,
        callbacks: canvasData.drawCallbacks.size
      })
    })
  }
  
  console.log('=== End Debug Info ===')
  return window.scriptExecutionService?.getStatus()
}

// Test function for eyeTouch
window.testEyeTouchScript = () => {
  if (!window.scriptExecutionService) {
    console.error('Script service not available')
    return false
  }
  
  if (!window.scriptExecutionService.canvasRegistry.has('eyeTouch')) {
    console.error('EyeTouch canvas not registered')
    return false
  }
  
  console.log('Testing eyeTouch canvas...')
  
  try {
    window.scriptExecutionService.executeScript(`
      console.log('Direct eyeTouch test starting');
      
      // Clear existing elements
      draw.clearElements();
      
      // Draw bright test elements
      draw.drawCircle(0, 0, 25, {
        fillColor: '#ff0000',
        strokeColor: '#ffffff',
        lineWidth: 3
      });
      
      draw.drawText(0, -6, 'DIRECT TEST', {
        fontSize: 16,
        color: '#00ff00'
      });
      
      // Add corners
      draw.drawCircle(-6, -4, 8, { fillColor: '#ff00ff' });
      draw.drawCircle(6, -4, 8, { fillColor: '#ffff00' });
      draw.drawCircle(-6, 4, 8, { fillColor: '#00ffff' });
      draw.drawCircle(6, 4, 8, { fillColor: '#ff8800' });
      
      console.log('Direct eyeTouch test completed');
      
    `, 'directEyeTouchTest', 'eyeTouch')
    
    console.log('EyeTouch test script executed successfully!')
    return true
    
  } catch (error) {
    console.error('Failed to execute eyeTouch test:', error)
    return false
  }
}

// Initialize the service immediately
scriptExecutionService.initialize().then(() => {
  console.log('ScriptExecutionService initialized in main.js')
}).catch(error => {
  console.error('Failed to initialize ScriptExecutionService in main.js:', error)
})

waitForConnection()
