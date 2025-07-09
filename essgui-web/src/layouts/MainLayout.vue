<template>
  <a-layout style="height: 100vh; font-size: 13px;">
    <!-- Application Menu Bar -->
    <a-layout-header :style="{ background: '#fff', padding: '0 16px', borderBottom: '1px solid #d9d9d9', height: '32px', lineHeight: '32px' }">
      <menu-bar @toggle-terminal="toggleTerminal" />
    </a-layout-header>

    <a-layout>
      <!-- Control Panel - Left Sidebar -->
      <a-layout-sider
        width="280"
        theme="light"
        style="border-right: 1px solid #d9d9d9;"
      >
        <experiment-control ref="experimentControlRef" @status-update="handleStatusUpdate" />
      </a-layout-sider>

      <!-- Main Content Area with Terminal -->
      <a-layout>
        <!-- Main Content -->
        <a-layout-content style="background: white; display: flex; flex-direction: column;">
          <!-- Tabs Area -->
          <div style="flex: 1; overflow: hidden;">
            <a-tabs
              size="small"
              style="height: 100%; overflow: hidden;"
              tab-position="top"
            >
              <a-tab-pane key="behavior" tab="Behavior">
                <div style="height: 100%; display: flex; flex-direction: column;">
                  <div style="flex: 1; display: flex; gap: 16px;">
                    <!-- Behavior Monitor Placeholder -->
                    <div style="flex: 1; border: 1px solid #d9d9d9; padding: 8px;">
                      <div style="font-weight: 500; margin-bottom: 8px;">Behavior Monitor</div>
                      <div style="height: 200px; background: #f5f5f5; display: flex; align-items: center; justify-content: center;">
                        EyeTouch Window Placeholder
                      </div>
                    </div>

                    <!-- Input Controls Placeholder -->
                    <div style="width: 200px; border: 1px solid #d9d9d9; padding: 8px;">
                      <div style="font-weight: 500; margin-bottom: 8px;">Input</div>
                      <div style="height: 200px; background: #f5f5f5; display: flex; align-items: center; justify-content: center;">
                        Virtual Controls
                      </div>
                    </div>
                  </div>

                  <!-- Performance Table -->
                  <div style="margin-top: 16px; border: 1px solid #d9d9d9; padding: 8px;">
                    <div style="font-weight: 500; margin-bottom: 8px;">Performance</div>
                    <a-table
                      :columns="performanceColumns"
                      :data-source="performanceData"
                      size="small"
                      :pagination="false"
                      :scroll="{ y: 100 }"
                    />
                  </div>
                </div>
              </a-tab-pane>

	      <a-tab-pane key="stim" tab="Stim" style="height: 100%; overflow: hidden;">
	        <stim-info />
	      </a-tab-pane>

              <a-tab-pane key="scripts" tab="Scripts">
                <div>Script editor will go here</div>
              </a-tab-pane>

              <a-tab-pane key="system" tab="System">
                <div>System state diagram will go here</div>
              </a-tab-pane>

              <a-tab-pane key="data" tab="Data">
                <div>Data file management will go here</div>
              </a-tab-pane>

            </a-tabs>
          </div>

          <!-- Terminal at Bottom -->
          <template v-if="isTerminalVisible">
            <div class="resizer" @mousedown="startResize"></div>
            <div v-if="isTerminalVisible" :style="{ height: `${terminalHeight}px` }" class="terminal-wrapper">
              <ess-terminal ref="bottomTerminalRef" />
            </div>
          </template>
        </a-layout-content>
      </a-layout>
    </a-layout>
    <status-bar :status-message="statusMessage" />
  </a-layout>
</template>

<script setup>
import { ref } from 'vue'
import MenuBar from '../components/MenuBar.vue'
import ExperimentControl from '../components/ExperimentControl.vue'
import EssTerminal from '../components/EssTerminal.vue'
import StatusBar from '../components/StatusBar.vue'
import StimInfo from '../components/StimInfo.vue'

// Status message
const statusMessage = ref('Ready')

// Terminal refs
const bottomTerminalRef = ref(null)
const experimentControlRef = ref(null)
const isTerminalVisible = ref(true)
const terminalHeight = ref(180) // Initial height in px

// Performance table data (could be moved to a store)
const performanceColumns = [
  { title: 'Condition', dataIndex: 'condition', key: 'condition', width: 100 },
  { title: '% Correct', dataIndex: 'correct', key: 'correct', width: 80 },
  { title: 'RT (ms)', dataIndex: 'rt', key: 'rt', width: 80 },
  { title: 'N', dataIndex: 'n', key: 'n', width: 60 }
]

const performanceData = ref([
  { key: '1', condition: 'Easy', correct: 95, rt: 450, n: 50 },
  { key: '2', condition: 'Hard', correct: 78, rt: 650, n: 50 }
])

function startResize(event) {
  event.preventDefault();
  window.addEventListener('mousemove', doResize);
  window.addEventListener('mouseup', stopResize);
}

function doResize(event) {
  const newHeight = window.innerHeight - event.clientY - 24; // 24px for status bar
  // Set constraints for resizing
  if (newHeight > 40 && newHeight < window.innerHeight * 0.9) {
    terminalHeight.value = newHeight;
  }
}

function stopResize() {
  window.removeEventListener('mousemove', doResize);
  window.removeEventListener('mouseup', stopResize);
}

function toggleTerminal() {
  isTerminalVisible.value = !isTerminalVisible.value
}

// Handle terminal logging from experiment control
function handleStatusUpdate({ message, type }) {
  statusMessage.value = message
  // Log to the bottom terminal
  if (bottomTerminalRef.value) {
    bottomTerminalRef.value.addLine(message, type)
  }
}
</script>

<style scoped>
.resizer {
  height: 5px;
  background: #f0f0f0;
  cursor: ns-resize;
  margin: 0 16px;
}

.terminal-wrapper {
  overflow: hidden;
}

:deep(.ant-tabs-nav) {
  margin-left: 14px;
}

</style>
