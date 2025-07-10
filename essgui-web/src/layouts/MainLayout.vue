<template>
  <a-layout style="height: 100vh; font-size: 13px;">
    <!-- Application Menu Bar -->
    <a-layout-header :style="{ background: '#fff', padding: '0 16px', borderBottom: '1px solid #d9d9d9', height: '32px', lineHeight: '32px' }">
      <menu-bar @toggle-terminal="toggleTerminal" />
    </a-layout-header>

    <a-layout style="height: calc(100vh - 32px);">
      <!-- Control Panel - Left Sidebar -->
      <a-layout-sider
        width="290"
        theme="light"
        style="border-right: 1px solid #d9d9d9;"
      >
        <experiment-control ref="experimentControlRef" @status-update="handleStatusUpdate" />
      </a-layout-sider>

      <!-- Main Content Area with Terminal -->
      <a-layout style="display: flex; flex-direction: column; height: 100%; overflow: hidden;">
        <!-- Main Content -->
        <a-layout-content style="background: white; flex: 1; display: flex; flex-direction: column; overflow: hidden;">
          <!-- Tabs Area -->
          <div style="flex: 1; display: flex; flex-direction: column; overflow: hidden;">
            <a-tabs
              size="small"
              style="height: 100%; display: flex; flex-direction: column;"
              tab-position="top"
            >
              <template #tabBarExtraContent>
                <div style="height: 40px;"></div>
              </template>
              
              <a-tab-pane key="behavior" tab="Behavior" style="height: 100%; overflow: hidden;">
                <div style="height: 100%; display: flex; flex-direction: column; overflow: auto;">
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
                <div style="height: 100%; overflow: hidden;">
                  <stim-info />
                </div>
              </a-tab-pane>

              <a-tab-pane key="scripts" tab="Scripts" style="height: 100%; overflow: hidden;">
	        <div style="height: 100%; overflow: hidden;">
                  <scripts />
  	        </div>
              </a-tab-pane>

              <a-tab-pane key="system" tab="System" style="height: 100%; overflow: hidden;">
	        <div style="height: 100%; overflow: hidden;">
                  <state-system-diagram />
  	        </div>

              </a-tab-pane>

              <a-tab-pane key="data" tab="Data" style="height: 100%; overflow: hidden;">
                <div>Data file management will go here</div>
              </a-tab-pane>

            </a-tabs>
          </div>
        </a-layout-content>

        <!-- Terminal at Bottom -->
        <template v-if="isTerminalVisible">
          <div class="resizer" @mousedown="startResize"></div>
          <div :style="{ height: `${terminalHeight}px`, flexShrink: 0 }" class="terminal-wrapper">
            <ess-terminal ref="bottomTerminalRef" />
          </div>
        </template>
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
import Scripts from '../components/Scripts.vue'
import StateSystemDiagram from '../components/StateSystemDiagram.vue'

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
  flex-shrink: 0;
}

.terminal-wrapper {
  overflow: hidden;
  background: white;
  border-top: 1px solid #d9d9d9;
}

/* Fix tab content height */
:deep(.ant-tabs) {
  display: flex;
  flex-direction: column;
  height: 100%;
}

:deep(.ant-tabs-nav) {
  margin-left: 14px;
  flex-shrink: 0;
}

:deep(.ant-tabs-content) {
  height: 100%;
  overflow: hidden;
}

:deep(.ant-tabs-content-holder) {
  height: 100%;
}

:deep(.ant-tabs-tabpane) {
  height: 100%;
}
</style>