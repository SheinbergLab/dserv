<template>
  <div style="height: 100%; display: flex; flex-direction: column; overflow: hidden;">
    <!-- Header with controls -->
    <div style="flex-shrink: 0; padding: 8px; border-bottom: 1px solid #e8e8e8; display: flex; justify-content: space-between; align-items: center;">
      <div style="font-size: 12px; color: #666;">
        <template v-if="isLoading">
          <a-spin size="small" style="margin-right: 8px;" />
          Loading state diagram...
        </template>
        <template v-else-if="nodes.length > 0">
          <strong>{{ nodes.length }}</strong> states
          <span v-if="currentState" style="margin-left: 12px;">
            <strong>Current:</strong> {{ currentState }}
          </span>
          <span v-if="totalVisits > 0" style="margin-left: 12px;">
            <strong>Total visits:</strong> {{ totalVisits }}
          </span>
          <span v-if="totalTransitions > 0" style="margin-left: 12px;">
            <strong>Transitions:</strong> {{ totalTransitions }}
          </span>
        </template>
        <template v-else>
          No state system loaded
        </template>
      </div>
      
      <div style="display: flex; align-items: center; gap: 8px;">
        <a-select
          v-model:value="layoutMode"
          size="small"
          style="width: 120px;"
          @change="applyLayout"
        >
          <a-select-option value="grid">Grid</a-select-option>
          <a-select-option value="hierarchical">Hierarchical</a-select-option>
          <a-select-option value="circular">Circular</a-select-option>
        </a-select>
        
        <a-button 
          size="small" 
          @click="resetCounters"
          :icon="h(ClearOutlined)"
          title="Reset visit counters and timing data"
        >
          Reset
        </a-button>
        
        <a-button 
          size="small" 
          @click="centerView"
          :icon="h(ZoomInOutlined)"
        >
          Fit
        </a-button>
      </div>
    </div>
    
    <!-- Main diagram area -->
    <div ref="diagramContainer" style="flex: 1; position: relative; overflow: hidden;">
      <svg
        ref="svgRef"
        style="width: 100%; height: 100%; cursor: grab;"
        @mousedown="startPan"
        @mousemove="doPan"
        @mouseup="endPan"
        @wheel="handleZoom"
      >
        <!-- Background grid -->
        <defs>
          <pattern id="grid" width="20" height="20" patternUnits="userSpaceOnUse">
            <path d="M 20 0 L 0 0 0 20" fill="none" stroke="#f0f0f0" stroke-width="1"/>
          </pattern>
          <!-- Arrow marker -->
          <marker
            id="arrowhead"
            markerWidth="10"
            markerHeight="7"
            refX="9"
            refY="3.5"
            orient="auto"
          >
            <polygon
              points="0 0, 10 3.5, 0 7"
              fill="#666"
            />
          </marker>
        </defs>
        <rect width="100%" height="100%" fill="url(#grid)" />
        
        <!-- Transform group for zoom/pan -->
        <g ref="transformGroup" :transform="`translate(${transform.x}, ${transform.y}) scale(${transform.scale})`">
          
          <!-- Edges (connections) -->
          <g class="edges">
            <g v-for="edge in edges" :key="edge.id">
              <path
                :d="getEdgePath(edge)"
                :stroke="edge.isActive ? '#1890ff' : getEdgeColor(edge)"
                :stroke-width="edge.isActive ? 3 : getEdgeWidth(edge)"
                fill="none"
                :stroke-dasharray="edge.isActive ? '5,5' : 'none'"
                marker-end="url(#arrowhead)"
              />
              <!-- Timing label on edge -->
              <text
                v-if="getEdgeTimingLabel(edge)"
                :x="edge.labelX"
                :y="edge.labelY"
                text-anchor="middle"
                style="font-size: 9px; fill: #666; user-select: none; font-family: monospace;"
                :style="{ fill: edge.isActive ? '#1890ff' : '#999' }"
              >
                {{ getEdgeTimingLabel(edge) }}
              </text>
            </g>
          </g>
          
          <!-- Nodes (states) -->
          <g class="nodes">
            <g
              v-for="node in nodes"
              :key="node.id"
              :transform="`translate(${node.x}, ${node.y})`"
              @click="selectNode(node)"
              style="cursor: pointer;"
            >
              <!-- Node background -->
              <rect
                :width="node.width"
                :height="node.height"
                :rx="8"
                :ry="8"
                :fill="getNodeColor(node)"
                :stroke="getNodeBorderColor(node)"
                :stroke-width="node.isSelected ? 3 : node.isCurrent ? 3 : 1"
                :stroke-dasharray="node.isCurrent ? '3,3' : 'none'"
              />
              
              <!-- Node label -->
              <text
                :x="node.width / 2"
                :y="node.height / 2 - 2"
                text-anchor="middle"
                dominant-baseline="middle"
                style="font-size: 12px; font-weight: 500; fill: #333; user-select: none;"
              >
                {{ node.label }}
              </text>
              
              <!-- Visit counter -->
              <text
                v-if="getVisitCount(node.id) > 0"
                :x="node.width / 2"
                :y="node.height / 2 + 12"
                text-anchor="middle"
                dominant-baseline="middle"
                style="font-size: 10px; font-weight: 400; fill: #666; user-select: none;"
              >
                ({{ getVisitCount(node.id) }})
              </text>
              
              <!-- Current state indicator -->
              <circle
                v-if="node.isCurrent"
                :cx="node.width - 8"
                :cy="8"
                r="4"
                fill="#52c41a"
                stroke="white"
                stroke-width="1"
              />
              
              <!-- Visit count badge for high counts -->
              <g v-if="getVisitCount(node.id) >= 10">
                <circle
                  :cx="8"
                  :cy="8"
                  r="8"
                  fill="#ff4d4f"
                  stroke="white"
                  stroke-width="1"
                />
                <text
                  x="8"
                  y="12"
                  text-anchor="middle"
                  style="font-size: 8px; font-weight: bold; fill: white; user-select: none;"
                >
                  {{ getVisitCount(node.id) }}
                </text>
              </g>
            </g>
          </g>
        </g>
      </svg>
      
      <!-- Enhanced node details panel with timing -->
      <div
        v-if="selectedNode"
        style="position: absolute; top: 10px; right: 10px; width: 320px; background: white; border: 1px solid #d9d9d9; border-radius: 6px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); padding: 12px; z-index: 10; max-height: 80vh; overflow-y: auto;"
      >
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
          <h4 style="margin: 0; font-size: 14px;">{{ selectedNode.label }}</h4>
          <a-button
            size="small"
            type="text"
            @click="selectedNode = null"
            :icon="h(CloseOutlined)"
          />
        </div>
        
        <div style="font-size: 12px; color: #666;">
          <div v-if="selectedNode.isCurrent" style="color: #52c41a; font-weight: 500; margin-bottom: 4px;">
            ● Currently Active
          </div>
          
          <div v-if="getVisitCount(selectedNode.id) > 0" style="margin-bottom: 4px;">
            <strong>Visits:</strong> {{ getVisitCount(selectedNode.id) }}
          </div>
          
          <div v-if="getStateDwellTime(selectedNode.id)" style="margin-bottom: 8px;">
            <strong>Avg Dwell Time:</strong> 
            <span style="color: #1890ff; font-family: monospace;">
              {{ getStateDwellTime(selectedNode.id).toFixed(1) }}ms
            </span>
          </div>
          
          <!-- Incoming Transitions with Timing -->
          <div v-if="getIncomingTransitionAverage(selectedNode.id).length > 0" style="margin-bottom: 8px;">
            <strong>Incoming Transitions:</strong>
            <div style="margin-left: 8px;">
              <div 
                v-for="incoming in getIncomingTransitionAverage(selectedNode.id)" 
                :key="incoming.from"
                style="display: flex; justify-content: space-between; margin: 2px 0; font-size: 11px;"
              >
                <span>{{ incoming.from }}</span>
                <span style="color: #1890ff; font-family: monospace;">
                  {{ incoming.average.toFixed(1) }}ms (n={{ incoming.count }})
                </span>
              </div>
            </div>
          </div>
          
          <!-- Outgoing Transitions with Timing -->
          <div style="margin-bottom: 8px;">
            <strong>Outgoing Transitions:</strong>
            <div v-if="selectedNode.transitions.length === 0" style="color: #999; font-style: italic; margin-left: 8px;">
              No transitions (end state)
            </div>
            <div v-else style="margin-left: 8px;">
              <div 
                v-for="transition in selectedNode.transitions"
                :key="transition"
                style="margin: 2px 0;"
              >
                <div style="display: flex; justify-content: space-between; font-size: 11px;">
                  <span>{{ transition }}</span>
                  <span 
                    v-if="getOutgoingTransitionAverage(selectedNode.id).find(t => t.to === transition)"
                    style="color: #52c41a; font-family: monospace;"
                  >
                    {{ getOutgoingTransitionAverage(selectedNode.id).find(t => t.to === transition).average.toFixed(1) }}ms 
                    (n={{ getOutgoingTransitionAverage(selectedNode.id).find(t => t.to === transition).count }})
                  </span>
                  <span v-else style="color: #999; font-style: italic; font-size: 10px;">
                    not visited
                  </span>
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick, h } from 'vue'
import { ZoomInOutlined, CloseOutlined, ClearOutlined } from '@ant-design/icons-vue'
import { dserv } from '../services/dserv.js'

// Reactive state
const nodes = ref([])
const edges = ref([])
const selectedNode = ref(null)
const currentState = ref('')
const actionState = ref('')
const transitionState = ref('')
const isLoading = ref(false)
const layoutMode = ref('grid')
const visitCounts = ref(new Map()) // Track visit counts for each state

// Timing tracking state
const transitionTimings = ref(new Map()) // Track timing data per transition
const stateEnterTimes = ref(new Map()) // Track when states were entered
const lastActionState = ref('')
const lastActionTime = ref(0)

// SVG refs and transform state
const svgRef = ref(null)
const transformGroup = ref(null)
const diagramContainer = ref(null)
const transform = ref({ x: 0, y: 0, scale: 1 })
const isPanning = ref(false)
const lastPanPoint = ref({ x: 0, y: 0 })

// Computed properties
const totalVisits = computed(() => {
  let total = 0
  for (const count of visitCounts.value.values()) {
    total += count
  }
  return total
})

const totalTransitions = computed(() => {
  let total = 0
  for (const timing of transitionTimings.value.values()) {
    total += timing.count
  }
  return total
})

// Component registration and cleanup
onMounted(() => {
  console.log('StateSystemDiagram component mounted')
  
  const cleanup = dserv.registerComponent('StateSystemDiagram', {
    subscriptions: [
      { pattern: 'ess/state_table', every: 1 },
      { pattern: 'ess/action_state', every: 1 },
      { pattern: 'ess/transition_state', every: 1 },
      { pattern: 'ess/state', every: 1 },
      { pattern: 'ess/reset', every: 1 }
    ]
  })
  
  // Listen for state table updates
  dserv.on('datapoint:ess/state_table', (data) => {
    console.log('Received state table:', data.data)
    parseStateTable(data.data)
  })
  
  // Listen for current state updates
  dserv.on('datapoint:ess/action_state', (data) => {
    actionState.value = data.data
    updateCurrentState()
  })
  
  dserv.on('datapoint:ess/transition_state', (data) => {
    transitionState.value = data.data
    updateCurrentState()
  })
  
  // Listen for system state changes (running/stopped)
  dserv.on('datapoint:ess/state', (data) => {
    console.log('System state changed to:', data.data)
    if (data.data === 'Stopped') {
      // Clear active state when system stops
      actionState.value = ''
      transitionState.value = ''
      currentState.value = ''
      updateCurrentState()
    }
  })
  
  // Listen for reset events
  dserv.on('datapoint:ess/reset', () => {
    console.log('Reset received, clearing visit counters and timing data')
    resetCounters()
  })
  
  // Listen for system changes that clear the diagram
  dserv.on('systemState', ({ loading }) => {
    if (loading) {
      console.log('System loading, clearing state diagram')
      nodes.value = []
      edges.value = []
      selectedNode.value = null
      currentState.value = ''
      actionState.value = ''
      transitionState.value = ''
      resetCounters()
    }
  })
  
  // Try to load existing state table if already available
  nextTick(() => {
    console.log('Checking for existing state table...')
    // Check if state table already exists in dserv
    if (dserv.state.connected) {
      try {
        // Touch the datapoints to get current values
        dserv.essCommand('catch {dservTouch ess/state_table}')
        dserv.essCommand('catch {dservTouch ess/action_state}') 
        dserv.essCommand('catch {dservTouch ess/transition_state}')
        dserv.essCommand('catch {dservTouch ess/state}')
      } catch (error) {
        console.log('Could not touch datapoints (system may not be loaded):', error)
      }
    }
  })
  
  onUnmounted(cleanup)
})

// Visit counter functions
function getVisitCount(nodeId) {
  return visitCounts.value.get(nodeId) || 0
}

// Timing helper functions
function getIncomingTransitionAverage(stateName) {
  const incomingTimings = []
  for (const [transitionKey, timing] of transitionTimings.value.entries()) {
    if (transitionKey.endsWith(`->${stateName}`)) {
      incomingTimings.push({
        from: transitionKey.split('->')[0],
        average: timing.average / 1000, // Convert to milliseconds
        count: timing.count
      })
    }
  }
  return incomingTimings.sort((a, b) => a.from.localeCompare(b.from))
}

function getOutgoingTransitionAverage(stateName) {
  const outgoingTimings = []
  for (const [transitionKey, timing] of transitionTimings.value.entries()) {
    if (transitionKey.startsWith(`${stateName}->`)) {
      outgoingTimings.push({
        to: transitionKey.split('->')[1],
        average: timing.average / 1000, // Convert to milliseconds
        count: timing.count
      })
    }
  }
  return outgoingTimings.sort((a, b) => a.to.localeCompare(b.to))
}

function getStateDwellTime(stateName) {
  // Calculate average time spent in this state
  const outgoingTimings = getOutgoingTransitionAverage(stateName)
  if (outgoingTimings.length === 0) return null
  
  const totalTime = outgoingTimings.reduce((sum, timing) => sum + (timing.average * timing.count), 0)
  const totalCount = outgoingTimings.reduce((sum, timing) => sum + timing.count, 0)
  
  return totalCount > 0 ? totalTime / totalCount : null
}

function getTransitionTiming(fromState, toState) {
  const transitionKey = `${fromState}->${toState}`
  return transitionTimings.value.get(transitionKey)
}

function getEdgeTimingLabel(edge) {
  const timing = getTransitionTiming(edge.source, edge.target)
  if (!timing || timing.count === 0) return null
  return `${(timing.average / 1000).toFixed(0)}ms`
}

function getEdgeColor(edge) {
  const timing = getTransitionTiming(edge.source, edge.target)
  if (!timing || timing.count === 0) return '#d9d9d9'
  
  // Color based on frequency of use
  if (timing.count >= 10) return '#52c41a' // Green for frequent
  if (timing.count >= 5) return '#faad14'  // Orange for moderate
  return '#1890ff' // Blue for infrequent
}

function getEdgeWidth(edge) {
  const timing = getTransitionTiming(edge.source, edge.target)
  if (!timing || timing.count === 0) return 1
  
  // Width based on frequency
  if (timing.count >= 10) return 3
  if (timing.count >= 5) return 2
  return 1
}

function resetCounters() {
  visitCounts.value.clear()
  visitCounts.value = new Map(visitCounts.value)
  
  transitionTimings.value.clear()
  transitionTimings.value = new Map(transitionTimings.value)
  
  stateEnterTimes.value.clear()
  lastActionState.value = ''
  lastActionTime.value = 0
  
  console.log('Visit counters and timing data reset')
}

// Parse TCL dictionary format state table
function parseStateTable(stateTableStr) {
  if (!stateTableStr || stateTableStr.trim() === '') {
    nodes.value = []
    edges.value = []
    return
  }
  
  try {
    // Parse TCL dictionary format
    const stateDict = parseTclDict(stateTableStr)
    console.log('Parsed state dictionary:', stateDict)
    
    // Create nodes and edges
    createNodesAndEdges(stateDict)
    
    // Apply initial layout
    nextTick(() => {
      applyLayout()
      // Immediate fit to screen
      setTimeout(() => {
        centerView()
      }, 50) // Small delay to ensure layout is complete
    })
  } catch (error) {
    console.error('Failed to parse state table:', error)
  }
}

// Simple TCL dictionary parser
function parseTclDict(dictStr) {
  const result = {}
  const tokens = []
  let current = ''
  let inBraces = 0
  
  // Tokenize the string
  for (let i = 0; i < dictStr.length; i++) {
    const char = dictStr[i]
    
    if (char === '{') {
      inBraces++
      current += char
    } else if (char === '}') {
      inBraces--
      current += char
    } else if (char === ' ' && inBraces === 0) {
      if (current.trim()) {
        tokens.push(current.trim())
        current = ''
      }
    } else {
      current += char
    }
  }
  
  if (current.trim()) {
    tokens.push(current.trim())
  }
  
  // Parse key-value pairs
  for (let i = 0; i < tokens.length; i += 2) {
    const key = tokens[i]
    let value = tokens[i + 1] || ''
    
    // Handle braced lists
    if (value.startsWith('{') && value.endsWith('}')) {
      value = value.slice(1, -1)
      result[key] = value ? value.split(' ') : []
    } else {
      result[key] = value ? [value] : []
    }
  }
  
  return result
}

// Create nodes and edges from state dictionary
function createNodesAndEdges(stateDict) {
  const nodeMap = new Map()
  const newNodes = []
  const newEdges = []
  
  // Clear visit counts and timing when new state system is loaded
  resetCounters()
  
  // Create nodes
  Object.keys(stateDict).forEach((stateName, index) => {
    const node = {
      id: stateName,
      label: stateName,
      x: 0,
      y: 0,
      width: Math.max(80, stateName.length * 8 + 20),
      height: 40,
      transitions: stateDict[stateName] || [],
      incomingStates: [],
      isCurrent: false,
      isSelected: false,
      isStartState: stateName === 'start',
      isEndState: stateName === 'end' || (stateDict[stateName] || []).length === 0
    }
    newNodes.push(node)
    nodeMap.set(stateName, node)
  })
  
  // Calculate incoming states and create edges
  Object.entries(stateDict).forEach(([fromState, toStates]) => {
    toStates.forEach(toState => {
      if (nodeMap.has(toState)) {
        nodeMap.get(toState).incomingStates.push(fromState)
        
        const edge = {
          id: `${fromState}-${toState}`,
          source: fromState,
          target: toState,
          isActive: false
        }
        
        newEdges.push(edge)
      }
    })
  })
  
  nodes.value = newNodes
  edges.value = newEdges
  
  updateCurrentState()
}

// Update current state highlighting with timing tracking
function updateCurrentState() {
  // Extract state name from action/transition state (remove _a/_t suffix)
  let current = ''
  let previousCurrent = currentState.value
  const currentTime = Date.now() * 1000 // Convert to microseconds for consistency
  
  if (actionState.value) {
    current = actionState.value.replace(/_a$/, '')
  } else if (transitionState.value) {
    current = transitionState.value.replace(/_t$/, '')
  }
  
  // Track state visits and timing when we enter a new action state
  if (current && current !== previousCurrent && actionState.value) {
    // Update visit counter
    const currentCount = visitCounts.value.get(current) || 0
    visitCounts.value.set(current, currentCount + 1)
    
    // Track transition timing if we have a previous state
    if (lastActionState.value && lastActionTime.value > 0) {
      const transitionKey = `${lastActionState.value}->${current}`
      const duration = currentTime - lastActionTime.value
      
      if (!transitionTimings.value.has(transitionKey)) {
        transitionTimings.value.set(transitionKey, {
          durations: [],
          totalTime: 0,
          count: 0,
          average: 0
        })
      }
      
      const timing = transitionTimings.value.get(transitionKey)
      timing.durations.push(duration)
      timing.totalTime += duration
      timing.count += 1
      timing.average = timing.totalTime / timing.count
      
      console.log(`Transition ${transitionKey}: ${(duration/1000).toFixed(1)}ms (avg: ${(timing.average/1000).toFixed(1)}ms)`)
    }
    
    // Update tracking variables
    lastActionState.value = current
    lastActionTime.value = currentTime
    stateEnterTimes.value.set(current, currentTime)
    
    // Force reactivity update
    visitCounts.value = new Map(visitCounts.value)
    transitionTimings.value = new Map(transitionTimings.value)
  }
  
  currentState.value = current
  
  // Update node highlighting
  nodes.value.forEach(node => {
    node.isCurrent = node.id === current
  })
  
  // Update edge highlighting
  edges.value.forEach(edge => {
    edge.isActive = edge.source === current
  })
}

// Node selection
function selectNode(node) {
  nodes.value.forEach(n => n.isSelected = false)
  node.isSelected = true
  selectedNode.value = node
}

// Layout algorithms
function applyLayout() {
  if (nodes.value.length === 0) return
  
  switch (layoutMode.value) {
    case 'hierarchical':
      applyHierarchicalLayout()
      break
    case 'circular':
      applyCircularLayout()
      break
    case 'grid':
      applyGridLayout()
      break
  }
  
  updateEdgePaths()
  
  // Auto-fit after layout change
  setTimeout(() => {
    centerView()
  }, 50)
}

function applyHierarchicalLayout() {
  const layers = []
  const visited = new Set()
  const nodeMap = new Map(nodes.value.map(n => [n.id, n]))
  
  // Find start node and build layers
  const startNode = nodes.value.find(n => n.isStartState) || nodes.value[0]
  
  function buildLayers(nodeId, layer = 0) {
    if (visited.has(nodeId)) return
    visited.add(nodeId)
    
    if (!layers[layer]) layers[layer] = []
    layers[layer].push(nodeId)
    
    const node = nodeMap.get(nodeId)
    if (node) {
      node.transitions.forEach(targetId => {
        if (!visited.has(targetId)) {
          buildLayers(targetId, layer + 1)
        }
      })
    }
  }
  
  buildLayers(startNode.id)
  
  // Position nodes
  const layerHeight = 100
  const nodeSpacing = 120
  
  layers.forEach((layer, layerIndex) => {
    const y = layerIndex * layerHeight
    const totalWidth = (layer.length - 1) * nodeSpacing
    const startX = -totalWidth / 2
    
    layer.forEach((nodeId, nodeIndex) => {
      const node = nodeMap.get(nodeId)
      if (node) {
        node.x = startX + nodeIndex * nodeSpacing
        node.y = y
      }
    })
  })
}

function applyCircularLayout() {
  const centerX = 0
  const centerY = 0
  const radius = Math.max(150, nodes.value.length * 15)
  
  nodes.value.forEach((node, index) => {
    const angle = (index / nodes.value.length) * 2 * Math.PI
    node.x = centerX + radius * Math.cos(angle)
    node.y = centerY + radius * Math.sin(angle)
  })
}

function applyGridLayout() {
  const cols = Math.ceil(Math.sqrt(nodes.value.length))
  const cellWidth = 120
  const cellHeight = 80
  
  nodes.value.forEach((node, index) => {
    const row = Math.floor(index / cols)
    const col = index % cols
    node.x = col * cellWidth
    node.y = row * cellHeight
  })
}

// Edge path calculation
function updateEdgePaths() {
  edges.value.forEach(edge => {
    const sourceNode = nodes.value.find(n => n.id === edge.source)
    const targetNode = nodes.value.find(n => n.id === edge.target)
    
    if (sourceNode && targetNode) {
      const path = calculateEdgePath(sourceNode, targetNode)
      edge.path = path
      edge.labelX = (sourceNode.x + sourceNode.width/2 + targetNode.x + targetNode.width/2) / 2
      edge.labelY = (sourceNode.y + sourceNode.height/2 + targetNode.y + targetNode.height/2) / 2
    }
  })
}

function calculateEdgePath(source, target) {
  const sx = source.x + source.width / 2
  const sy = source.y + source.height / 2
  const tx = target.x + target.width / 2
  const ty = target.y + target.height / 2
  
  return `M ${sx} ${sy} L ${tx} ${ty}`
}

function getEdgePath(edge) {
  return edge.path || ''
}

// Node styling with visit-based coloring
function getNodeColor(node) {
  const visits = getVisitCount(node.id)
  
  if (node.isCurrent) return '#e6f7ff'
  if (node.isStartState) return '#f6ffed'
  if (node.isEndState) return '#fff2e8'
  
  // Color intensity based on visit count
  if (visits >= 20) return '#fff1f0' // Light red for heavily visited
  if (visits >= 10) return '#fff7e6' // Light orange for moderately visited
  if (visits >= 5) return '#fcffe6'  // Light yellow for lightly visited
  if (visits > 0) return '#f6ffed'   // Light green for visited
  
  return '#fafafa' // Default for unvisited
}

function getNodeBorderColor(node) {
  if (node.isCurrent) return '#1890ff'
  if (node.isSelected) return '#722ed1'
  if (node.isStartState) return '#52c41a'
  if (node.isEndState) return '#fa8c16'
  return '#d9d9d9'
}

// Pan and zoom functionality
function startPan(event) {
  if (event.target === svgRef.value) {
    isPanning.value = true
    lastPanPoint.value = { x: event.clientX, y: event.clientY }
    svgRef.value.style.cursor = 'grabbing'
  }
}

function doPan(event) {
  if (isPanning.value) {
    const dx = event.clientX - lastPanPoint.value.x
    const dy = event.clientY - lastPanPoint.value.y
    
    transform.value.x += dx
    transform.value.y += dy
    
    lastPanPoint.value = { x: event.clientX, y: event.clientY }
  }
}

function endPan() {
  isPanning.value = false
  svgRef.value.style.cursor = 'grab'
}

function handleZoom(event) {
  event.preventDefault()
  const delta = event.deltaY > 0 ? 0.9 : 1.1
  const newScale = Math.max(0.1, Math.min(3, transform.value.scale * delta))
  
  if (newScale !== transform.value.scale) {
    const rect = svgRef.value.getBoundingClientRect()
    const centerX = rect.width / 2
    const centerY = rect.height / 2
    
    const dx = (centerX - transform.value.x) * (newScale / transform.value.scale - 1)
    const dy = (centerY - transform.value.y) * (newScale / transform.value.scale - 1)
    
    transform.value.scale = newScale
    transform.value.x -= dx
    transform.value.y -= dy
  }
}

function centerView() {
  if (nodes.value.length === 0) return
  
  const bounds = calculateBounds()
  const containerRect = diagramContainer.value.getBoundingClientRect()
  
  const scaleX = (containerRect.width - 100) / bounds.width
  const scaleY = (containerRect.height - 100) / bounds.height
  const scale = Math.min(scaleX, scaleY, 1)
  
  transform.value.scale = scale
  transform.value.x = containerRect.width / 2 - (bounds.centerX * scale)
  transform.value.y = containerRect.height / 2 - (bounds.centerY * scale)
}

function calculateBounds() {
  if (nodes.value.length === 0) return { width: 0, height: 0, centerX: 0, centerY: 0 }
  
  const xs = nodes.value.map(n => n.x)
  const ys = nodes.value.map(n => n.y)
  const widths = nodes.value.map(n => n.width)
  const heights = nodes.value.map(n => n.height)
  
  const minX = Math.min(...xs)
  const maxX = Math.max(...xs.map((x, i) => x + widths[i]))
  const minY = Math.min(...ys)
  const maxY = Math.max(...ys.map((y, i) => y + heights[i]))
  
  return {
    width: maxX - minX,
    height: maxY - minY,
    centerX: (minX + maxX) / 2,
    centerY: (minY + maxY) / 2
  }
}
</script>

<style scoped>
.edges path {
  transition: stroke 0.3s ease, stroke-width 0.3s ease;
}

.nodes rect {
  transition: fill 0.3s ease, stroke 0.3s ease, stroke-width 0.3s ease;
}

.nodes g:hover rect {
  filter: brightness(1.05);
}

/* Enhanced edge styling */
.edges text {
  pointer-events: none;
  font-size: 9px;
  font-family: monospace;
}

/* Node details panel scrollbar */
div[style*="max-height: 80vh"]::-webkit-scrollbar {
  width: 6px;
}

div[style*="max-height: 80vh"]::-webkit-scrollbar-track {
  background: #f1f1f1;
  border-radius: 3px;
}

div[style*="max-height: 80vh"]::-webkit-scrollbar-thumb {
  background: #c1c1c1;
  border-radius: 3px;
}

div[style*="max-height: 80vh"]::-webkit-scrollbar-thumb:hover {
  background: #a8a8a8;
}
</style>