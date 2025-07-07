<template>
  <div class="modal-overlay" @click="$emit('close')">
    <div class="modal-dialog" @click.stop>
      <div class="modal-header">
        <span>Application Settings</span>
        <button @click="$emit('close')" class="modal-close">×</button>
      </div>
      
      <div class="modal-content">
        <div class="settings-content">
          <!-- General Settings -->
          <div class="settings-section">
            <h3>General</h3>
            <div class="setting-item">
              <label for="auto-connect">Auto-connect on startup:</label>
              <input 
                id="auto-connect"
                type="checkbox" 
                v-model="localSettings.autoConnect"
              />
            </div>
            <div class="setting-item">
              <label for="theme">Theme:</label>
              <select id="theme" v-model="localSettings.theme">
                <option value="light">Light</option>
                <option value="dark">Dark</option>
                <option value="auto">Auto</option>
              </select>
            </div>
            <div class="setting-item">
              <label for="font-size">Font Size:</label>
              <input 
                id="font-size"
                type="number" 
                min="10" 
                max="20" 
                v-model.number="localSettings.fontSize"
              />
            </div>
            <div class="setting-item">
              <label for="compact-mode">Compact mode:</label>
              <input 
                id="compact-mode"
                type="checkbox" 
                v-model="localSettings.compactMode"
              />
            </div>
          </div>

          <!-- Logging Settings -->
          <div class="settings-section">
            <h3>Logging</h3>
            <div class="setting-item">
              <label for="log-level">Log Level:</label>
              <select id="log-level" v-model="localSettings.logLevel">
                <option value="debug">Debug</option>
                <option value="info">Info</option>
                <option value="warn">Warning</option>
                <option value="error">Error</option>
              </select>
            </div>
            <div class="setting-item">
              <label for="max-log-entries">Max Log Entries:</label>
              <input 
                id="max-log-entries"
                type="number" 
                min="100" 
                max="10000" 
                step="100"
                v-model.number="localSettings.maxLogEntries"
              />
            </div>
            <div class="setting-item">
              <label for="auto-scroll">Auto-scroll logs:</label>
              <input 
                id="auto-scroll"
                type="checkbox" 
                v-model="localSettings.autoScrollLogs"
              />
            </div>
          </div>

          <!-- Performance Settings -->
          <div class="settings-section">
            <h3>Performance</h3>
            <div class="setting-item">
              <label for="refresh-interval">Refresh Interval (ms):</label>
              <input 
                id="refresh-interval"
                type="number" 
                min="1000" 
                max="30000" 
                step="1000"
                v-model.number="localSettings.refreshInterval"
              />
            </div>
            <div class="setting-item">
              <label for="buffer-size">Update Buffer Size:</label>
              <input 
                id="buffer-size"
                type="number" 
                min="100" 
                max="10000" 
                step="100"
                v-model.number="localSettings.bufferSize"
              />
            </div>
            <div class="setting-item">
              <label for="enable-animations">Enable animations:</label>
              <input 
                id="enable-animations"
                type="checkbox" 
                v-model="localSettings.enableAnimations"
              />
            </div>
          </div>

          <!-- Connection Settings -->
          <div class="settings-section">
            <h3>Connection</h3>
            <div class="setting-item">
              <label for="dserv-address">Dserv Address:</label>
              <input 
                id="dserv-address"
                type="text" 
                v-model="localSettings.dservAddress"
                placeholder="localhost:4620"
              />
            </div>
            <div class="setting-item">
              <label for="ess-address">ESS Address:</label>
              <input 
                id="ess-address"
                type="text" 
                v-model="localSettings.essAddress"
                placeholder="localhost:2570"
              />
            </div>
            <div class="setting-item">
              <label for="timeout">Connection Timeout (s):</label>
              <input 
                id="timeout"
                type="number" 
                min="5" 
                max="60" 
                v-model.number="localSettings.connectionTimeout"
              />
            </div>
            <div class="setting-item">
              <label for="auto-reconnect">Auto-reconnect:</label>
              <input 
                id="auto-reconnect"
                type="checkbox" 
                v-model="localSettings.autoReconnect"
              />
            </div>
          </div>

          <!-- Monitor Settings -->
          <div class="settings-section">
            <h3>Monitor</h3>
            <div class="setting-item">
              <label for="show-grid">Show grid by default:</label>
              <input 
                id="show-grid"
                type="checkbox" 
                v-model="localSettings.showGrid"
              />
            </div>
            <div class="setting-item">
              <label for="show-trail">Show trail by default:</label>
              <input 
                id="show-trail"
                type="checkbox" 
                v-model="localSettings.showTrail"
              />
            </div>
            <div class="setting-item">
              <label for="trail-length">Default trail length:</label>
              <input 
                id="trail-length"
                type="number" 
                min="10" 
                max="500" 
                step="10"
                v-model.number="localSettings.trailLength"
              />
            </div>
          </div>
        </div>
      </div>
      
      <div class="modal-actions">
        <button @click="resetToDefaults" class="warning-btn">Reset to Defaults</button>
        <div class="action-spacer"></div>
        <button @click="$emit('close')" class="secondary-btn">Cancel</button>
        <button @click="saveSettings" class="primary-btn">Save</button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'

const props = defineProps({
  settings: {
    type: Object,
    required: true
  }
})

const emit = defineEmits(['close', 'save'])

// Local copy of settings for editing
const localSettings = ref({})

// Default settings
const defaultSettings = {
  theme: 'light',
  autoConnect: true,
  logLevel: 'info',
  maxLogEntries: 1000,
  refreshInterval: 5000,
  fontSize: 13,
  compactMode: false,
  autoScrollLogs: true,
  bufferSize: 1000,
  enableAnimations: true,
  dservAddress: 'localhost:4620',
  essAddress: 'localhost:2570',
  connectionTimeout: 10,
  autoReconnect: true,
  showGrid: true,
  showTrail: true,
  trailLength: 50
}

const saveSettings = () => {
  emit('save', localSettings.value)
}

const resetToDefaults = () => {
  if (confirm('Are you sure you want to reset all settings to defaults?')) {
    localSettings.value = { ...defaultSettings }
  }
}

onMounted(() => {
  // Initialize local settings with current values
  localSettings.value = { ...defaultSettings, ...props.settings }
})
</script>

<style scoped>
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.modal-dialog {
  background: white;
  border: 2px solid #999;
  border-radius: 4px;
  width: 90%;
  max-width: 700px;
  max-height: 85vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 4px 12px rgba(0,0,0,0.3);
}

.modal-header {
  padding: 8px 12px;
  background: linear-gradient(to bottom, #f0f0f0, #e0e0e0);
  border-bottom: 1px solid #ccc;
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-weight: 500;
  font-size: 12px;
  flex-shrink: 0;
}

.modal-close {
  width: 20px;
  height: 20px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  cursor: pointer;
  font-size: 14px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 2px;
}

.modal-close:hover {
  background: #ff6b6b;
  color: white;
}

.modal-content {
  flex: 1;
  overflow-y: auto;
  padding: 16px;
}

.settings-content {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.settings-section {
  border: 1px solid #ddd;
  border-radius: 4px;
  padding: 16px;
  background: #fafafa;
}

.settings-section h3 {
  margin: 0 0 16px 0;
  font-size: 16px;
  color: #333;
  border-bottom: 1px solid #ddd;
  padding-bottom: 8px;
}

.setting-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
  font-size: 12px;
}

.setting-item:last-child {
  margin-bottom: 0;
}

.setting-item label {
  flex: 1;
  font-weight: 500;
  color: #555;
}

.setting-item input,
.setting-item select {
  width: 120px;
  padding: 4px 6px;
  border: 1px solid #999;
  border-radius: 2px;
  font-size: 11px;
}

.setting-item input[type="checkbox"] {
  width: auto;
}

.setting-item input[type="text"] {
  width: 150px;
  font-family: monospace;
}

.modal-actions {
  padding: 12px 16px;
  border-top: 1px solid #ccc;
  display: flex;
  gap: 8px;
  align-items: center;
  flex-shrink: 0;
}

.action-spacer {
  flex: 1;
}

.primary-btn,
.secondary-btn,
.warning-btn {
  padding: 6px 16px;
  font-size: 12px;
  border: 1px solid #999;
  cursor: pointer;
  border-radius: 2px;
}

.primary-btn {
  background: linear-gradient(to bottom, #4CAF50, #45a049);
  color: white;
}

.secondary-btn {
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  color: #333;
}

.warning-btn {
  background: linear-gradient(to bottom, #ff9800, #e68900);
  color: white;
}

.primary-btn:hover {
  background: linear-gradient(to bottom, #45a049, #3e8e41);
}

.secondary-btn:hover {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.warning-btn:hover {
  background: linear-gradient(to bottom, #e68900, #cc7a00);
}
</style>