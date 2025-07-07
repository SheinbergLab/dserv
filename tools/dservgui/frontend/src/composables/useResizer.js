import { ref } from 'vue'

// Default sizes
const leftSidebarWidth = ref(275)
const rightSidebarWidth = ref(180)
const terminalHeight = ref(250)

// Resize state
const isResizing = ref(false)
const resizeType = ref(null) // 'left', 'right', 'terminal'

export function useResizer() {

  const startLeftResize = (event) => {
    startResize(event, 'left')
  }

  const startRightResize = (event) => {
    startResize(event, 'right')
  }

  const startTerminalResize = (event) => {
    startResize(event, 'terminal')
  }

  const startResize = (event, type) => {
    event.preventDefault()
    isResizing.value = true
    resizeType.value = type

    const startX = event.clientX
    const startY = event.clientY
    const startWidth = type === 'left' ? leftSidebarWidth.value : 
                      type === 'right' ? rightSidebarWidth.value : null
    const startHeight = type === 'terminal' ? terminalHeight.value : null

    const onMouseMove = (e) => {
      e.preventDefault()
      if (!isResizing.value) return

      switch (type) {
        case 'left':
          const leftDelta = e.clientX - startX
          leftSidebarWidth.value = Math.max(200, Math.min(500, startWidth + leftDelta))
          break
          
        case 'right':
          const rightDelta = startX - e.clientX
          rightSidebarWidth.value = Math.max(150, Math.min(400, startWidth + rightDelta))
          break
          
        case 'terminal':
          const terminalDelta = startY - e.clientY
          terminalHeight.value = Math.max(150, Math.min(600, startHeight + terminalDelta))
          break
      }
    }

    const onMouseUp = (e) => {
      e.preventDefault()
      isResizing.value = false
      resizeType.value = null
      
      document.removeEventListener('mousemove', onMouseMove)
      document.removeEventListener('mouseup', onMouseUp)
      
      // Reset cursor and selection
      document.body.style.cursor = ''
      document.body.style.userSelect = ''
      document.documentElement.style.pointerEvents = ''
      
      // Save sizes to localStorage
      saveSizes()
    }

    document.addEventListener('mousemove', onMouseMove)
    document.addEventListener('mouseup', onMouseUp)
    
    // Prevent text selection during resize
    document.body.style.cursor = type === 'terminal' ? 'ns-resize' : 'ew-resize'
    document.body.style.userSelect = 'none'
    document.documentElement.style.pointerEvents = 'none'
  }

  const saveSizes = () => {
    const sizes = {
      leftSidebarWidth: leftSidebarWidth.value,
      rightSidebarWidth: rightSidebarWidth.value,
      terminalHeight: terminalHeight.value
    }
    
    try {
      localStorage.setItem('dserv-layout-sizes', JSON.stringify(sizes))
    } catch (error) {
      console.warn('Failed to save layout sizes:', error)
    }
  }

  const loadSizes = () => {
    try {
      const saved = localStorage.getItem('dserv-layout-sizes')
      if (saved) {
        const sizes = JSON.parse(saved)
        leftSidebarWidth.value = sizes.leftSidebarWidth || 275
        rightSidebarWidth.value = sizes.rightSidebarWidth || 180
        terminalHeight.value = sizes.terminalHeight || 250
      }
    } catch (error) {
      console.warn('Failed to load layout sizes:', error)
    }
  }

  const resetSizes = () => {
    leftSidebarWidth.value = 275
    rightSidebarWidth.value = 180
    terminalHeight.value = 250
    saveSizes()
  }

  const toggleLeftSidebar = () => {
    leftSidebarWidth.value = leftSidebarWidth.value > 0 ? 0 : 275
    saveSizes()
  }

  const toggleRightSidebar = () => {
    rightSidebarWidth.value = rightSidebarWidth.value > 0 ? 0 : 180
    saveSizes()
  }

  const minimizeTerminal = () => {
    terminalHeight.value = 30 // Just show header
    saveSizes()
  }

  const maximizeTerminal = () => {
    terminalHeight.value = 400
    saveSizes()
  }

  const initialize = () => {
    loadSizes()
  }

  return {
    // State
    leftSidebarWidth,
    rightSidebarWidth,
    terminalHeight,
    isResizing,
    resizeType,
    
    // Methods
    startLeftResize,
    startRightResize,
    startTerminalResize,
    saveSizes,
    loadSizes,
    resetSizes,
    toggleLeftSidebar,
    toggleRightSidebar,
    minimizeTerminal,
    maximizeTerminal,
    initialize
  }
}
