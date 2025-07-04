<template>
  <div class="modal-overlay" @click="$emit('close')">
    <div class="modal-dialog" @click.stop>
      <div class="modal-header">
        <span>About ESS DservGUI</span>
        <button @click="$emit('close')" class="modal-close">×</button>
      </div>
      
      <div class="modal-content">
        <div class="about-content">
          <!-- App Icon and Title -->
          <div class="app-info">
            <div class="app-icon">📡</div>
            <h2>ESS DservGUI</h2>
            <p class="version">Version 2.0.0</p>
          </div>

          <!-- Description -->
          <div class="description">
            <p>
              Desktop-style interface for dserv variable monitoring and ESS system control.
              Built with Vue.js and designed for real-time experimental data visualization.
            </p>
          </div>

          <!-- Features -->
          <div class="features">
            <h3>Features</h3>
            <ul>
              <li>Real-time variable monitoring</li>
              <li>ESS system configuration</li>
              <li>Interactive terminal interface</li>
              <li>Position tracking and visualization</li>
              <li>Live data logging and filtering</li>
              <li>Modular panel-based architecture</li>
            </ul>
          </div>

          <!-- Technical Info -->
          <div class="tech-info">
            <h3>Technical Information</h3>
            <div class="tech-grid">
              <div class="tech-item">
                <span class="tech-label">Frontend:</span>
                <span class="tech-value">Vue.js 3 + Composition API</span>
              </div>
              <div class="tech-item">
                <span class="tech-label">Backend:</span>
                <span class="tech-value">Go with dserv integration</span>
              </div>
              <div class="tech-item">
                <span class="tech-label">Communication:</span>
                <span class="tech-value">Server-Sent Events (SSE)</span>
              </div>
              <div class="tech-item">
                <span class="tech-label">Data Format:</span>
                <span class="tech-value">JSON Messages</span>
              </div>
            </div>
          </div>

          <!-- System Requirements -->
          <div class="requirements">
            <h3>System Requirements</h3>
            <div class="req-grid">
              <div class="req-item">
                <span class="req-label">Dserv Server:</span>
                <span class="req-value">localhost:4620</span>
              </div>
              <div class="req-item">
                <span class="req-label">ESS Server:</span>
                <span class="req-value">localhost:2570</span>
              </div>
              <div class="req-item">
                <span class="req-label">Web Interface:</span>
                <span class="req-value">localhost:8080</span>
              </div>
              <div class="req-item">
                <span class="req-label">Browser:</span>
                <span class="req-value">Modern browser with ES6+ support</span>
              </div>
            </div>
          </div>

          <!-- Copyright -->
          <div class="copyright">
            <p>&copy; 2024 ESS DservGUI. Built for experimental data monitoring.</p>
          </div>
        </div>
      </div>
      
      <div class="modal-actions">
        <button @click="$emit('close')" class="primary-btn">OK</button>
        <button @click="openGitHub" class="secondary-btn">View on GitHub</button>
      </div>
    </div>
  </div>
</template>

<script setup>
defineEmits(['close'])

const openGitHub = () => {
  // Open GitHub repository (placeholder)
  console.log('Opening GitHub repository...')
  // window.open('https://github.com/your-repo/ess-dservgui', '_blank')
}
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
  max-width: 600px;
  max-height: 80vh;
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
  padding: 20px;
}

.about-content {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.app-info {
  text-align: center;
  border-bottom: 1px solid #eee;
  padding-bottom: 20px;
}

.app-icon {
  font-size: 48px;
  margin-bottom: 12px;
}

.app-info h2 {
  margin: 0 0 8px 0;
  font-size: 24px;
  color: #333;
}

.version {
  margin: 0;
  color: #666;
  font-size: 14px;
}

.description p {
  margin: 0;
  color: #555;
  line-height: 1.5;
  font-size: 14px;
}

.features h3,
.tech-info h3,
.requirements h3 {
  margin: 0 0 12px 0;
  font-size: 16px;
  color: #333;
  border-bottom: 1px solid #eee;
  padding-bottom: 4px;
}

.features ul {
  margin: 0;
  padding-left: 20px;
  list-style-type: disc;
}

.features li {
  margin: 6px 0;
  font-size: 13px;
  color: #555;
}

.tech-grid,
.req-grid {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.tech-item,
.req-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 6px 8px;
  background: #f8f9fa;
  border-radius: 3px;
  font-size: 12px;
}

.tech-label,
.req-label {
  font-weight: 500;
  color: #666;
}

.tech-value,
.req-value {
  color: #333;
  font-family: monospace;
}

.copyright {
  text-align: center;
  border-top: 1px solid #eee;
  padding-top: 16px;
}

.copyright p {
  margin: 0;
  color: #999;
  font-size: 12px;
}

.modal-actions {
  padding: 12px 16px;
  border-top: 1px solid #ccc;
  display: flex;
  gap: 8px;
  justify-content: flex-end;
}

.primary-btn,
.secondary-btn {
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

.primary-btn:hover {
  background: linear-gradient(to bottom, #45a049, #3e8e41);
}

.secondary-btn:hover {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}
</style>