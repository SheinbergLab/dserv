<template>
  <div class="panel welcome-panel">
    <div class="panel-header">
      <span>Welcome to ESS DservGUI</span>
    </div>
    
    <div class="panel-content">
      <div class="welcome-content">
        <!-- Main Welcome Section -->
        <div class="welcome-section">
          <div class="welcome-icon">📡</div>
          <h2>ESS DservGUI</h2>
          <p class="welcome-description">
            Desktop-style interface for dserv variable monitoring and ESS system control.
            Connect to your dserv instance to begin monitoring experiment variables in real-time.
          </p>
        </div>

        <!-- Connection Status -->
        <div class="status-section">
          <div class="status-card" :class="connectionStatus">
            <div class="status-icon">
              {{ connectionStatus === 'connected' ? '✅' : '🔌' }}
            </div>
            <div class="status-content">
              <h3>{{ connectionStatus === 'connected' ? 'Connected' : 'Not Connected' }}</h3>
              <p>{{ connectionMessage }}</p>
              <button 
                v-if="connectionStatus !== 'connected'"
                @click="$emit('connect')"
                class="connect-btn"
              >
                Connect to dserv
              </button>
            </div>
          </div>
        </div>

        <!-- Quick Start Guide -->
        <div class="guide-section">
          <h3>Quick Start Guide</h3>
          <div class="guide-steps">
            <div class="guide-step" :class="{ completed: connectionStatus === 'connected' }">
              <div class="step-number">1</div>
              <div class="step-content">
                <h4>Connect to dserv</h4>
                <p>Establish connection to your dserv instance (localhost:4620)</p>
              </div>
            </div>
            
            <div class="guide-step" :class="{ completed: connectionStatus === 'connected' }">
              <div class="step-number">2</div>
              <div class="step-content">
                <h4>Configure ESS System</h4>
                <p>Use the System Control panel to select your experimental system</p>
              </div>
            </div>
            
            <div class="guide-step">
              <div class="step-number">3</div>
              <div class="step-content">
                <h4>Monitor Variables</h4>
                <p>Query variables, set up subscriptions, and monitor real-time data</p>
              </div>
            </div>
            
            <div class="guide-step">
              <div class="step-number">4</div>
              <div class="step-content">
                <h4>Use Terminal</h4>
                <p>Execute ESS commands directly through the integrated terminal</p>
              </div>
            </div>
          </div>
        </div>

        <!-- Available Panels -->
        <div class="panels-section">
          <h3>Available Panels</h3>
          <div class="panel-grid">
            <div class="panel-card" @click="selectPanel('query')">
              <div class="panel-icon">🔍</div>
              <h4>Query</h4>
              <p>Query individual variables and view their current values</p>
            </div>
            
            <div class="panel-card" @click="selectPanel('subscribe')">
              <div class="panel-icon">📡</div>
              <h4>Subscribe</h4>
              <p>Set up subscriptions for real-time variable monitoring</p>
            </div>
            
            <div class="panel-card" @click="selectPanel('dataframe')">
              <div class="panel-icon">📊</div>
              <h4>Data</h4>
              <p>View and analyze dataframes and experimental data</p>
            </div>
            
            <div class="panel-card" @click="selectPanel('monitor')">
              <div class="panel-icon">📈</div>
              <h4>Monitor</h4>
              <p>Real-time position monitoring and visualization</p>
            </div>
            
            <div class="panel-card" @click="selectPanel('log')">
              <div class="panel-icon">📄</div>
              <h4>Log</h4>
              <p>View live data streams and system logs</p>
            </div>
          </div>
        </div>

        <!-- System Information -->
        <div class="info-section">
          <h3>System Information</h3>
          <div class="info-grid">
            <div class="info-item">
              <span class="info-label">Dserv Server:</span>
              <span class="info-value">localhost:4620</span>
            </div>
            <div class="info-item">
              <span class="info-label">ESS Server:</span>
              <span class="info-value">localhost:2570</span>
            </div>
            <div class="info-item">
              <span class="info-label">Web Interface:</span>
              <span class="info-value">localhost:8080</span>
            </div>
            <div class="info-item">
              <span class="info-label">Data Format:</span>
              <span class="info-value">JSON Messages</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

const props = defineProps({
  connectionStatus: {
    type: String,
    default: 'disconnected'
  }
})

const emit = defineEmits(['connect'])

const connectionMessage = computed(() => {
  switch (props.connectionStatus) {
    case 'connected':
      return 'Successfully connected to dserv server. You can now monitor variables and control your ESS system.'
    case 'connecting':
      return 'Attempting to connect to dserv server...'
    case 'error':
      return 'Failed to connect to dserv server. Please check that dserv is running on localhost:4620.'
    default:
      return 'Click the connect button to establish a connection to your dserv instance.'
  }
})

const selectPanel = (panelName) => {
  // This would typically emit an event to change the active panel
  console.log('Selecting panel:', panelName)
  window.dispatchEvent(new CustomEvent('change-panel', { detail: panelName }))
}
</script>

<style scoped>
.welcome-panel .panel-content {
  padding: 20px;
  max-width: 800px;
  margin: 0 auto;
}

.welcome-content {
  display: flex;
  flex-direction: column;
  gap: 30px;
}

.welcome-section {
  text-align: center;
  padding: 20px;
}

.welcome-icon {
  font-size: 48px;
  margin-bottom: 16px;
}

.welcome-section h2 {
  margin: 0 0 16px 0;
  font-size: 28px;
  color: #333;
}

.welcome-description {
  font-size: 16px;
  color: #666;
  line-height: 1.5;
  max-width: 600px;
  margin: 0 auto;
}

.status-section {
  display: flex;
  justify-content: center;
}

.status-card {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 20px;
  border: 2px solid #ddd;
  border-radius: 8px;
  background: white;
  min-width: 300px;
}

.status-card.connected {
  border-color: #4CAF50;
  background: #f8fff8;
}

.status-card.disconnected {
  border-color: #f44336;
  background: #fff8f8;
}

.status-icon {
  font-size: 24px;
}

.status-content h3 {
  margin: 0 0 8px 0;
  font-size: 18px;
}

.status-content p {
  margin: 0 0 12px 0;
  color: #666;
  font-size: 14px;
}

.connect-btn {
  padding: 8px 16px;
  background: #4CAF50;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  font-size: 14px;
  font-weight: 500;
}

.connect-btn:hover {
  background: #45a049;
}

.guide-section h3,
.panels-section h3,
.info-section h3 {
  margin: 0 0 16px 0;
  font-size: 20px;
  color: #333;
  border-bottom: 2px solid #eee;
  padding-bottom: 8px;
}

.guide-steps {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.guide-step {
  display: flex;
  align-items: flex-start;
  gap: 12px;
  padding: 16px;
  border: 1px solid #ddd;
  border-radius: 6px;
  background: white;
  transition: all 0.2s ease;
}

.guide-step.completed {
  border-color: #4CAF50;
  background: #f8fff8;
}

.step-number {
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background: #ddd;
  color: white;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 12px;
  font-weight: bold;
  flex-shrink: 0;
}

.guide-step.completed .step-number {
  background: #4CAF50;
}

.step-content h4 {
  margin: 0 0 4px 0;
  font-size: 14px;
  color: #333;
}

.step-content p {
  margin: 0;
  font-size: 12px;
  color: #666;
  line-height: 1.4;
}

.panel-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 16px;
}

.panel-card {
  padding: 16px;
  border: 1px solid #ddd;
  border-radius: 6px;
  background: white;
  cursor: pointer;
  transition: all 0.2s ease;
  text-align: center;
}

.panel-card:hover {
  border-color: #0078d4;
  transform: translateY(-2px);
  box-shadow: 0 4px 8px rgba(0,0,0,0.1);
}

.panel-icon {
  font-size: 24px;
  margin-bottom: 8px;
}

.panel-card h4 {
  margin: 0 0 8px 0;
  font-size: 16px;
  color: #333;
}

.panel-card p {
  margin: 0;
  font-size: 12px;
  color: #666;
  line-height: 1.4;
}

.info-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 12px;
}

.info-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 8px 12px;
  background: #f8f9fa;
  border-radius: 4px;
  font-size: 12px;
}

.info-label {
  color: #666;
  font-weight: 500;
}

.info-value {
  color: #333;
  font-family: monospace;
}
</style>