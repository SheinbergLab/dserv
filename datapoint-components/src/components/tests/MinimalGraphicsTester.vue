<template>
  <div class="graphics-tester">
    <!-- Status Bar -->
    <div class="status-bar">
      <h2>🎨 Minimal Graphics Tester</h2>
      <div class="connection-status" :class="{ connected: isConnected }">
        <div class="status-dot"></div>
        <span>{{ isConnected ? 'Connected' : 'Disconnected' }}</span>
      </div>
    </div>

    <!-- Two Column Layout -->
    <div class="main-layout">
      
      <!-- Left: Terminal -->
      <div class="terminal-panel">
        <h3>Tcl Commands</h3>
        
        <!-- Quick Test Buttons -->
        <div class="quick-tests">
          <button @click="testBasicGraphics" class="test-btn">Basic Test</button>
          <button @click="testCircle" class="test-btn">Circle</button>
          <button @click="testLine" class="test-btn">Line</button>
          <button @click="clearGraphics" class="test-btn clear-btn">Clear</button>
        </div>
        
        <!-- Manual Command Input -->
        <div class="command-input">
          <input 
            v-model="currentCommand" 
            @keyup.enter="executeCommand"
            placeholder="Enter Tcl command"
            ref="commandInput"
          >
          <button @click="executeCommand" class="execute-btn">Execute</button>
        </div>
        
        <!-- Debug Info -->
        <div class="debug-info">
          <h4>Graphics Stream Debug</h4>
          <div class="debug-item">
            <strong>Stream:</strong> {{ streamId }}
          </div>
          <div class="debug-item">
            <strong>Connection:</strong> {{ isConnected ? 'Connected' : 'Disconnected' }}
          </div>
        </div>
      </div>

      <!-- Right: Canvas -->
      <div class="canvas-panel">
        <h3>Graphics Output</h3>
        
        <div class="canvas-container">
          <GraphicsCanvas 
            :stream-id="streamId"
            :width="canvasWidth" 
            :height="canvasHeight"
            show-stats
            ref="graphicsCanvas"
            canvas-class="graphics-canvas"
            @mousemove="onCanvasMouseMove"
            @click="onCanvasClick"
          />
          
          <!-- Mouse position overlay -->
          <div class="canvas-overlay" v-if="mousePos">
            {{ mousePos.x.toFixed(1) }}, {{ mousePos.y.toFixed(1) }}
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import GraphicsCanvas from '../GraphicsCanvas.vue'

export default {
  name: 'MinimalGraphicsTester',
  components: {
    GraphicsCanvas
  },
  
  data() {
    return {
      isConnected: false,
      currentCommand: '',
      streamId: 'graphics/test',
      canvasWidth: 640,
      canvasHeight: 480,
      mousePos: null,
      unsubscribeFunctions: []
    }
  },
  
  methods: {
    executeCommand() {
      const command = this.currentCommand.trim();
      if (!command || !this.isConnected) return;
      
      console.log('🚀 Executing graphics command:', command);
      
      const enhancedCommand = `${command}; set gbuf_data [dumpwin json]; dservSet ${this.streamId} $gbuf_data`;

      this.$dserv.sendCommand(enhancedCommand);
      
      this.currentCommand = '';
      this.$refs.commandInput?.focus();
    },

    testBasicGraphics() {
      this.currentCommand = 'clearwin; setcolor 1; circle 320 240 50 1';
      this.executeCommand();
    },

    testCircle() {
      this.currentCommand = 'setcolor 2; circle 200 200 30 0';
      this.executeCommand();
    },

    testLine() {
      this.currentCommand = 'setcolor 4; moveto 100 100; lineto 500 300';
      this.executeCommand();
    },

    clearGraphics() {
      this.currentCommand = 'clearwin';
      this.executeCommand();
    },

    onCanvasMouseMove(event) {
      // Get the canvas element from the GraphicsCanvas component
      const canvasElement = this.$refs.graphicsCanvas?.$el.querySelector('canvas');
      if (canvasElement) {
        const rect = canvasElement.getBoundingClientRect();
        this.mousePos = {
          x: event.clientX - rect.left,
          y: event.clientY - rect.top
        };
      }
    },

    onCanvasClick(event) {
      if (this.mousePos) {
        this.currentCommand = `circle ${this.mousePos.x} ${this.mousePos.y} 20 1`;
        this.executeCommand();
      }
    }
  },

  mounted() {
    console.log('🎨 Minimal Graphics Tester mounted');
    
    if (this.$dserv) {
      const connectionUnsub = this.$dserv.on('connection', (data) => {
        console.log('🔌 Connection changed:', data.connected);
        this.isConnected = data.connected;
      }, 'MinimalGraphicsTester');
      
      this.unsubscribeFunctions.push(connectionUnsub);
      this.isConnected = this.$dserv.state.connected;
    }
    
    this.$nextTick(() => {
      this.$refs.commandInput?.focus();
    });
  },

  beforeUnmount() {
    this.unsubscribeFunctions.forEach(fn => fn());
  }
}
</script>

<style scoped>
.graphics-tester {
  max-width: 1200px;
  margin: 0 auto;
  background: white;
  border-radius: 8px;
  box-shadow: 0 2px 12px rgba(0,0,0,0.1);
  overflow: hidden;
  font-family: -apple-system, BlinkMacSystemFont, sans-serif;
}

.status-bar {
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  color: white;
  padding: 15px 20px;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.status-bar h2 {
  margin: 0;
  font-size: 1.3rem;
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 8px;
}

.status-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: #ef4444;
  transition: background 0.3s;
}

.connection-status.connected .status-dot {
  background: #22c55e;
}

.main-layout {
  display: flex;
  min-height: 600px;
}

.terminal-panel {
  width: 350px;
  padding: 20px;
  background: #f8fafc;
  border-right: 1px solid #e2e8f0;
}

.canvas-panel {
  flex: 1;
  padding: 20px;
}

.terminal-panel h3, .canvas-panel h3 {
  margin: 0 0 15px 0;
  color: #374151;
  font-size: 1rem;
}

.quick-tests {
  margin-bottom: 20px;
}

.test-btn {
  margin: 4px;
  padding: 8px 12px;
  background: #3b82f6;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  font-size: 0.85rem;
}

.test-btn:hover {
  background: #2563eb;
}

.test-btn.clear-btn {
  background: #ef4444;
}

.test-btn.clear-btn:hover {
  background: #dc2626;
}

.command-input {
  display: flex;
  gap: 8px;
  margin-bottom: 20px;
}

.command-input input {
  flex: 1;
  padding: 10px;
  border: 1px solid #d1d5db;
  border-radius: 4px;
  font-family: monospace;
}

.execute-btn {
  padding: 10px 16px;
  background: #059669;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
}

.execute-btn:hover {
  background: #047857;
}

.debug-info {
  background: #f1f5f9;
  padding: 15px;
  border-radius: 6px;
  border: 1px solid #e2e8f0;
}

.debug-info h4 {
  margin: 0 0 10px 0;
  color: #374151;
  font-size: 0.9rem;
}

.debug-item {
  margin: 6px 0;
  font-size: 0.85rem;
  color: #6b7280;
}

.canvas-container {
  position: relative;
  display: inline-block;
}

.canvas-overlay {
  position: absolute;
  top: 8px;
  left: 8px;
  background: rgba(0,0,0,0.7);
  color: white;
  padding: 4px 8px;
  border-radius: 3px;
  font-family: monospace;
  font-size: 0.75rem;
  pointer-events: none;
}
</style>