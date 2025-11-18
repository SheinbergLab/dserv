<template>
  <div class="mesh-dashboard">
    <!-- Pure two-column layout: Systems | GUI -->
    <div class="main-content">
      <!-- Left Column - Systems List -->
      <div class="systems-column">
        <!-- Active Systems Section -->
        <div class="active-systems">
          <div class="section-header">
            <h2>Active Systems</h2>
            <div class="header-controls">
              <div class="connection-status" :class="{ connected: isConnected }">
                <div class="status-dot" :class="{ connected: isConnected }"></div>
                <span>{{ isConnected ? 'Connected' : 'Disconnected' }}</span>
              </div>
              <button @click="refreshPeers" class="btn btn-small">Refresh</button>
            </div>
          </div>

          <!-- Error Message -->
          <div v-if="errorMessage" class="error-banner">
            {{ errorMessage }}
          </div>

          <!-- Systems List -->
          <div class="systems-list">
            <div
              v-for="appliance in sortedAppliances"
              :key="appliance.applianceId"
              class="system-row"
              :class="{ 
                'local': appliance.isLocal, 
                'selected': selectedAppliance?.applianceId === appliance.applianceId 
              }"
              @click="selectAppliance(appliance)"
            >
              <div class="system-identity">
                <span class="system-name">{{ appliance.name || 'Unknown' }}</span>
				<div class="system-details">
				  <code class="system-id">{{ appliance.applianceId }}</code>
				  <span v-if="appliance.isLocal" class="badge local-badge">{{ appliance.ipAddress }}</span>
				  <span v-else class="badge ip-badge">{{ appliance.ipAddress }}</span>
				  <span v-if="appliance.ssl" class="badge ssl-badge" title="SSL Enabled">🔒</span>
				</div>
				<div class="system-status-line">
				  <span class="status-text" :class="`status-${appliance.status || 'unknown'}`">
					{{ appliance.status || 'Unknown' }}
				  </span>
				  <span v-if="appliance.subject" class="subject-text">{{ appliance.subject }}</span>
				</div>
              </div>
              <div class="system-actions">
                <span class="status-dot" :class="`status-${appliance.status || 'unknown'}`"></span>
                <!-- Always show globe icon for all systems -->
                <button
                  @click.stop="jumpToMesh(appliance)"
                  class="btn btn-mini mesh-btn"
                  title="Jump to this system's mesh view"
                >
                  🌐
                </button>
                <!-- Always show open dashboard button -->
                <button
                  @click.stop="openDashboard(appliance)"
                  class="btn btn-mini"
                  title="Open in New Tab"
                >
                  ↗️
                </button>
              </div>
            </div>
          </div>

          <!-- No Systems Message -->
          <div v-if="appliances.length === 0" class="no-systems">
            <div class="no-systems-icon">🔍</div>
            <p>Discovering systems...</p>
          </div>
        </div>

        <!-- Lost Systems Section -->
        <div v-if="lostPeers.length > 0" class="lost-systems">
          <div class="section-header">
            <h3>Lost Systems</h3>
            <button @click="clearLostPeers" class="btn btn-mini">Clear</button>
          </div>
          <div class="lost-list">
            <div
              v-for="(lost, index) in lostPeers"
              :key="`lost-${lost.applianceId}`"
              class="lost-row"
            >
              <div class="lost-info">
                <div class="lost-left">
                  <span class="lost-name">{{ lost.name || 'Unknown' }}</span>
                  <code class="lost-id">{{ lost.applianceId }}</code>
                </div>
                <div class="lost-right">
                  <span class="lost-time">{{ lost.timeAgo || 'unknown' }}</span>
                  <span class="lost-ip">{{ lost.lastIpAddress || 'unknown' }}</span>
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- Right Column - GUI -->
      <div class="gui-column">
        <div class="gui-header">
          <span class="gui-title">
            <span v-if="selectedAppliance">{{ selectedAppliance.name }}</span>
            <span v-else>Lab Mesh Network</span>
          </span>
          <div class="gui-status" :class="{ connected: selectedAppliance }">
            {{ selectedAppliance ? 'Connected' : 'No Selection' }}
          </div>
        </div>
        
        <div class="gui-content">
          <!-- System Interface Iframe -->
          <iframe 
            v-if="selectedAppliance"
            :src="getApplianceUrl(selectedAppliance)"
            class="gui-iframe"
            title="System Interface"
          />
          
          <!-- No Selection Message -->
          <div v-else class="no-selection">
            <div class="selection-message">
              <h3>🌐 Lab Mesh Network</h3>
              <p>Welcome to the mesh network dashboard.</p>
              <p>This interface allows you to monitor and control multiple systems across our network.</p>
              
              
              <div class="selection-hint">
                <span>👈 Select a system from the list to begin</span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Debug Panel -->
    <div v-if="showDebug" class="debug-panel">
      <h4>Debug Info</h4>
      <div>WebSocket Status: {{ wsStatus }}</div>
      <div>Selected: {{ selectedAppliance?.name || 'None' }}</div>
      <div v-if="selectedAppliance">
        SSL: {{ selectedAppliance.ssl ? 'Enabled' : 'Disabled' }} | 
        Protocol: {{ selectedAppliance.ssl ? 'https' : 'http' }} | 
        Port: {{ selectedAppliance.webPort || 2565 }}
      </div>
      <div>Lost Peers: {{ lostPeers.length }}</div>
      <div>Total Appliances: {{ appliances.length }}</div>
    </div>
  </div>
</template>

<script>
export default {
  name: 'MeshDashboard',
  
  data() {
    return {
      appliances: [],
      lostPeers: [],
      selectedAppliance: null,
      isConnected: false,
      wsStatus: 'disconnected',
      errorMessage: '',
      lastUpdate: null,
      lastMessage: null,
      showDebug: false,
      lastActivePeerCount: undefined,
      
      // WebSocket connection for mesh updates
      ws: null,
      reconnectAttempts: 0,
      maxReconnectAttempts: 5,
      reconnectDelay: 2000
    };
  },

  computed: {
    sortedAppliances() {
      return [...this.appliances].sort((a, b) => {
        if (a.isLocal && !b.isLocal) return -1;
        if (!a.isLocal && b.isLocal) return 1;
        return (a.name || '').localeCompare(b.name || '');
      });
    },

    totalAppliances() {
      return this.appliances.length;
    },

    activePeers() {
      return this.appliances.filter(a => !a.isLocal).length;
    },

    runningExperiments() {
      return this.appliances.filter(a => 
        a.system || a.protocol || a.variant
      ).length;
    }
  },

  mounted() {
    this.connectWebSocket();
    this.fetchLostPeers();
    
    // Don't auto-select anything - start with overview
    
    // Debug mode toggle
    document.addEventListener('keydown', (e) => {
      if (e.ctrlKey && e.key === 'd') {
        this.showDebug = !this.showDebug;
        e.preventDefault();
      }
    });
  },

  beforeUnmount() {
    if (this.ws) this.ws.close();
  },

  methods: {
    // WebSocket methods
    connectWebSocket() {
      try {
        // Use wss:// if page is loaded over https://, otherwise use ws://
        const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${wsProtocol}//${window.location.host}/ws`;
        console.log(`Connecting to WebSocket: ${wsUrl}`);
        this.ws = new WebSocket(wsUrl);
        this.wsStatus = 'connecting';

        this.ws.onopen = () => {
          this.isConnected = true;
          this.wsStatus = 'connected';
          this.reconnectAttempts = 0;
          this.errorMessage = '';
          this.sendMessage({ cmd: 'mesh_subscribe' });
          setTimeout(() => this.fetchInitialData(), 100);
        };

        this.ws.onmessage = (event) => {
          try {
            const data = JSON.parse(event.data);
            this.handleMessage(data);
          } catch (e) {
            console.warn('Failed to parse WebSocket message:', event.data);
          }
        };

        this.ws.onclose = () => {
          this.isConnected = false;
          this.wsStatus = 'disconnected';
          this.scheduleReconnect();
        };

        this.ws.onerror = () => {
          this.errorMessage = 'WebSocket connection error';
        };

      } catch (error) {
        this.errorMessage = 'Failed to connect to server';
        this.scheduleReconnect();
      }
    },

    sendMessage(message) {
      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(JSON.stringify(message));
      }
    },

    handleMessage(data) {
      this.lastMessage = data;
      
      if (data.type === 'mesh_update' && data.data) {
        if (data.data.appliances) {
          this.appliances = data.data.appliances;
          this.updateLastUpdate();
          this.errorMessage = '';
          
          // Always log appliance data to catch any issues
          console.log('Received appliances:', this.appliances.map(a => ({
            name: a.name,
            ipAddress: a.ipAddress,
            ssl: a.ssl,
            webPort: a.webPort,
            applianceId: a.applianceId
          })));
          
          // Log SSL status for debugging
          if (this.showDebug) {
            console.log('Appliances detailed:', this.appliances.map(a => ({
              name: a.name,
              ssl: a.ssl,
              webPort: a.webPort,
              computedProtocol: a.ssl ? 'https' : 'http'
            })));
          }
          
          // Auto-select local appliance if none selected - REMOVED
          // Now we don't auto-reconnect, user must manually select
          
          // Only fetch lost peers when peer count changes
          const currentActivePeerCount = this.appliances.filter(a => !a.isLocal).length;
          if (this.lastActivePeerCount !== undefined && currentActivePeerCount !== this.lastActivePeerCount) {
            setTimeout(() => {
              this.fetchLostPeers();
            }, 500);
          }
          this.lastActivePeerCount = currentActivePeerCount;
        }
        
        if (data.data.recentlyLost) {
          this.updateLostPeers(data.data.recentlyLost);
        }
      } 
      else if (data.type === 'mesh_subscribed') {
        console.log('Successfully subscribed to mesh updates');
      }
      else if (data.error) {
        this.errorMessage = `Server error: ${data.error}`;
      }
    },

    async fetchInitialData() {
      try {
        // Use the same protocol as the current page
        const protocol = window.location.protocol; // 'http:' or 'https:'
        const apiBase = window.location.port === '2569' 
          ? `${protocol}//${window.location.hostname}:12348/api`
          : '/api';
        
        const response = await fetch(`${apiBase}/mesh/peers`);
        if (response.ok) {
          const data = await response.json();
          if (data.appliances) {
            this.appliances = data.appliances;
            this.updateLastUpdate();
            
            const localAppliance = this.appliances.find(a => a.isLocal);
            // Don't auto-select anymore - let user choose
          }
          
          if (data.recentlyLost) {
            this.updateLostPeers(data.recentlyLost);
          }
        }
      } catch (error) {
        console.warn('Failed to fetch initial mesh data:', error);
      }
    },

    async fetchLostPeers() {
      try {
        const protocol = window.location.protocol; // 'http:' or 'https:'
        const response = await fetch(`${protocol}//${window.location.hostname}:2569/api/lost-peers`);
        if (response.ok) {
          const data = await response.json();
          if (data.lostPeers) {
            this.lostPeers = data.lostPeers;
          }
        }
      } catch (error) {
        console.warn('Failed to fetch lost peers:', error);
      }
    },

    updateLostPeers(lostPeersList) {
      if (Array.isArray(lostPeersList)) {
        this.lostPeers = lostPeersList;
      }
    },

    clearLostPeers() {
		this.sendMessage({ cmd: 'clear_lost_peers' });
		this.lostPeers = []; // and clear immediately
	},

    scheduleReconnect() {
      if (this.reconnectAttempts < this.maxReconnectAttempts) {
        this.reconnectAttempts++;
        setTimeout(() => this.connectWebSocket(), this.reconnectDelay);
        this.reconnectDelay *= 1.5;
      } else {
        this.errorMessage = 'Unable to connect after multiple attempts.';
      }
    },

    // Selection and actions
    selectAppliance(appliance) {
      // If clicking on the already selected appliance, deselect it
      if (this.selectedAppliance?.applianceId === appliance.applianceId) {
        this.selectedAppliance = null;
      } else {
        this.selectedAppliance = appliance;
      }
    },

    disconnectSystem() {
      this.selectedAppliance = null;
    },

    getApplianceUrl(appliance) {
      // Compute protocol from ssl field (true = https, false = http)
      const protocol = appliance.ssl ? 'https' : 'http';
      const port = 2565;  // Hardcoded for testing - will use webPort later
      // Always use IP address for consistency in mesh network
      console.log(`Connecting to ${appliance.name}: ${protocol}://${appliance.ipAddress}:${port} (ssl=${appliance.ssl})`);
      return `${protocol}://${appliance.ipAddress}:${port}`;
    },

    refreshPeers() {
		if (this.isConnected) {
			// Use the new WebSocket force refresh command
			this.sendMessage({ cmd: 'force_refresh' });
		} else {
			// Fallback to HTTP when WebSocket is disconnected
			this.fetchInitialData();
			this.fetchLostPeers();
		}
    },

    openDashboard(appliance) {
      // Compute protocol from ssl field
      const protocol = appliance.ssl ? 'https' : 'http';
      const port = 2565;  // Hardcoded for testing
      const url = appliance.isLocal 
        ? `${protocol}://localhost:${port}`
        : `${protocol}://${appliance.ipAddress}:${port}`;
      console.log(`Opening dashboard: ${url} (ssl=${appliance.ssl})`);
      window.open(url, '_blank');
    },

    jumpToMesh(appliance) {
      // Mesh typically runs on different port than main web interface
      // For now, assume mesh port is webPort + 4 (e.g., 2565 -> 2569)
      // You may want to add meshPort to the heartbeat data if it differs
      const protocol = appliance.ssl ? 'https' : 'http';
      const meshPort = (appliance.webPort || 2565) + 4;
      const meshUrl = `${protocol}://${appliance.ipAddress}:${meshPort}`;
      window.open(meshUrl, '_blank');
    },

    updateLastUpdate() {
      this.lastUpdate = new Date().toLocaleTimeString();
    }
  }
};
</script>

<style scoped>
.mesh-dashboard {
  height: 100vh;
  display: flex;
  flex-direction: column;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  background: #f8fafc;
}

/* Main Content - Two columns taking full height */
.main-content {
  flex: 1;
  display: grid;
  grid-template-columns: 350px 1fr;
  gap: 1px;
  background: #e2e8f0;
  overflow: hidden;
}

/* Left Column - Systems */
.systems-column {
  background: white;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.active-systems {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.section-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 9px 15px;
  border-bottom: 1px solid #e2e8f0;
  background: #f8fafc;
  min-height: 16px;   
}

.section-header h2 {
  margin: 0;
  font-size: 1rem;
  color: #2d3748;
}

.section-header h3 {
  margin: 0;
  font-size: 0.9rem;
  color: #4a5568;
}

.header-controls {
  display: flex;
  align-items: center;
  gap: 12px;
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 8px;
  border-radius: 12px;
  background: #fed7d7;
  color: #742a2a;
  font-size: 0.75rem;
  font-weight: 500;
}

.connection-status.connected {
  background: #c6f6d5;
  color: #22543d;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #e53e3e;
}

.status-dot.connected {
  background: #38a169;
}

/* Systems List */
.systems-list {
  flex: 1;
  overflow-y: auto;
  padding: 8px;
}

.system-row {
  padding: 8px 12px;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  margin-bottom: 6px;
  cursor: pointer;
  transition: all 0.2s ease;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.system-row:hover {
  border-color: #cbd5e0;
  background: #f7fafc;
}

.system-row.selected {
  border-color: #4299e1;
  background: #ebf8ff;
}

.system-row.local {
  border-left: 4px solid #ed8936;
}

.system-identity {
  flex: 1;
  min-width: 0;
}

.system-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  margin-bottom: 4px;
}

.system-name {
  font-weight: 600;
  color: #2d3748;
  font-size: 0.9rem;
  flex: 1;
  min-width: 0;
}

.disconnect-btn-header {
  color: #e53e3e;
  background: #fed7d7;
  border: 1px solid #feb2b2;
  margin-left: 8px;
  flex-shrink: 0;
}

.disconnect-btn-header:hover {
  background: #feb2b2;
  color: #742a2a;
}

.system-details {
  display: flex;
  align-items: center;
  gap: 8px;
}

.system-id {
  background: #edf2f7;
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 0.7rem;
  color: #4a5568;
}

.system-actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

/* Base badge styles */
.badge {
  color: white;
  padding: 2px 6px;
  border-radius: 8px;
  font-size: 0.6rem;
  font-weight: normal;
}

.local-badge {
  background: #ed8936;
}

.ip-badge {
  background: #4299e1;
}

.ssl-badge {
  background: #48bb78;
  font-size: 0.55rem;
  padding: 2px 4px;
}

/* System status line */
.system-status-line {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 2px;
}

.status-text {
  font-size: 0.7rem;
  font-weight: 500;
  text-transform: capitalize;
}

.status-text.status-running { color: #2d7a2d; } /* Darker green for better contrast */
.status-text.status-stopped { color: #e53e3e; }
.status-text.status-loading { color: #ed8936; }
.status-text.status-idle { color: #718096; }
.status-text.status-busy { color: #ed8936; }
.status-text.status-error { color: #e53e3e; }
.status-text.status-unknown { color: #a0aec0; }

.subject-text {
  font-size: 0.65rem;
  color: #718096;
  font-style: italic;
}

/* Status dots for system rows */
.system-actions .status-dot {
  width: 12px;
  height: 12px;
}

.system-actions .status-dot.status-idle { background: #718096; }
.system-actions .status-dot.status-running { background: #38a169; }
.system-actions .status-dot.status-busy { background: #ed8936; }
.system-actions .status-dot.status-error { background: #e53e3e; }
.system-actions .status-dot.status-unknown { background: #a0aec0; }

/* Lost Systems */
.lost-systems {
  border-top: 2px solid #e2e8f0;
  background: #f7fafc;
}

.lost-list {
  max-height: 120px;
  overflow-y: auto;
  padding: 6px;
}

.lost-row {
  padding: 6px 8px;
  border: 1px solid #cbd5e0;
  border-radius: 6px;
  margin-bottom: 4px;
  background: white;
  opacity: 0.7;
}

.lost-info {
  display: flex;
  justify-content: space-between;
  align-items: center;
  width: 100%;
  gap: 12px;
}

.lost-left {
  display: flex;
  align-items: center;
  gap: 8px;
  flex: 1;
  min-width: 0;
}

.lost-right {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-shrink: 0;
}

.lost-name {
  font-weight: 500;
  color: #718096;
  font-size: 0.8rem;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.lost-id {
  background: #edf2f7;
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 0.65rem;
  color: #718096;
  flex-shrink: 0;
}

.lost-time {
  color: #a0aec0;
  font-size: 0.7rem;
  white-space: nowrap;
  min-width: 90px;
  text-align: right;
}

.lost-ip {
  color: #a0aec0;
  font-size: 0.7rem;
  white-space: nowrap;
  min-width: 110px;
  text-align: right;
}

/* Right Column - GUI */
.gui-column {
  background: white;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.gui-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px 15px;
  background: #2d3748;
  color: white;
  font-size: 0.9rem;
  border-bottom: 1px solid #4a5568;
  min-height: 20px
}

.gui-title {
  font-weight: 500;
}

.gui-status {
  font-size: 0.8rem;
  color: #fed7d7;
}

.gui-status.connected {
  color: #c6f6d5;
}

.gui-content {
  flex: 1;
  display: flex;
  overflow: hidden;
}

.gui-iframe {
  width: 100%;
  height: 100%;
  border: none;
  background: white;
}

/* No selection message */
.no-selection {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  background: #f8fafc;
}

.selection-message {
  text-align: center;
  padding: 40px;
  max-width: 400px;
}

.selection-message h3 {
  margin: 0 0 16px 0;
  color: #2d3748;
  font-size: 1.2rem;
}

.selection-message p {
  margin: 0 0 8px 0;
  color: #718096;
  line-height: 1.5;
}

.selection-hint {
  margin-top: 24px;
  padding: 12px;
  background: #e6fffa;
  border: 1px solid #81e6d9;
  border-radius: 8px;
  color: #234e52;
  font-size: 0.9rem;
}

/* No systems message */
.no-systems {
  text-align: center;
  padding: 40px 20px;
  color: #718096;
}

.no-systems-icon {
  font-size: 2rem;
  margin-bottom: 8px;
}

/* Buttons */
.btn {
  padding: 6px 12px;
  border: none;
  border-radius: 4px;
  font-size: 0.8rem;
  cursor: pointer;
  transition: all 0.2s ease;
  background: #edf2f7;
  color: #4a5568;
}

.btn:hover {
  background: #e2e8f0;
  transform: translateY(-1px);
}

.btn-small {
  padding: 4px 8px;
  font-size: 0.75rem;
}

.btn-mini {
  padding: 4px 6px;
  font-size: 0.7rem;
  min-width: 28px;
}

.mesh-btn {
  background: #bee3f8;
  color: #2a69ac;
}

.mesh-btn:hover {
  background: #90cdf4;
}

/* Error Banner */
.error-banner {
  background: #fed7d7;
  border: 1px solid #feb2b2;
  color: #742a2a;
  padding: 8px 12px;
  margin: 8px 12px;
  border-radius: 4px;
  font-size: 0.8rem;
}

/* Debug Panel */
.debug-panel {
  background: #1a202c;
  color: #e2e8f0;
  padding: 12px;
  font-family: 'Courier New', monospace;
  font-size: 0.7rem;
  border-top: 1px solid #4a5568;
}

.debug-panel h4 {
  margin: 0 0 8px 0;
  color: #81e6d9;
}

/* Scrollbars */
::-webkit-scrollbar {
  width: 6px;
  height: 6px;
}

::-webkit-scrollbar-track {
  background: #edf2f7;
}

::-webkit-scrollbar-thumb {
  background: #cbd5e0;
  border-radius: 3px;
}

::-webkit-scrollbar-thumb:hover {
  background: #a0aec0;
}
</style>