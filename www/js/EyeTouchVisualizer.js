/**
 * EyeTouchVisualizer.js
 * Canvas-based visualization of eye position and touch events
 * 
 * Displays:
 * - Eye position marker with optional trail
 * - Eye tracking regions (fixation windows)
 * - Touch position and regions
 * - Virtual eye and touch inputs for testing
 * - Window status indicators
 */

class EyeTouchVisualizer {
    constructor(canvas, dpManager, options = {}) {
        this.canvas = typeof canvas === 'string' 
            ? document.getElementById(canvas) 
            : canvas;
        this.ctx = this.canvas.getContext('2d');
        this.dpManager = dpManager;
        
        // Get connection from options or from dpManager
        this.connection = options.connection || (dpManager && dpManager.connection) || null;
        
        this.options = {
            backgroundColor: '#000000',
            gridColor: '#333333',
            centerCrossColor: '#666666',
            eyeColor: '#ffffff',
            eyeStrokeColor: '#ff0000',
            eyeTrailColor: '#ff0000',
            touchColor: '#00ffff',
            touchStrokeColor: '#0088aa',
            virtualEyeColor: '#ff8c00',
            virtualTouchColor: '#ff8c00',
            eyeRegionColor: '#ff0000',
            eyeRegionFill: 'rgb(100, 50, 50)',
            touchRegionColor: '#00ffff',
            touchRegionFill: 'rgb(50, 100, 100)',
            essSubprocess: 'ess',  // subprocess for ESS commands
            ...options
        };
        
        // Visual range in degrees (matching FLTK's xextent/yextent = 16*2 = 32)
        this.xExtent = 32.0;
        this.yExtent = 32.0;
        
        // State
        this.state = {
            // Eye tracking
            eyePos: { x: 0, y: 0 },
            eyeTrail: [],
            maxTrailLength: 50,
            showTrails: false,
            
            // Touch
            touchPos: { x: 0, y: 0 },
            showTouch: false,
            
            // Regions
            eyeRegions: Array(8).fill(null).map((_, i) => ({
                id: i,
                active: false,
                state: 0,
                type: 'rectangle',
                center: { x: 0, y: 0 },
                size: { width: 2, height: 2 },
                inRegion: false
            })),
            touchRegions: Array(8).fill(null).map((_, i) => ({
                id: i,
                active: false,
                state: 0,
                type: 'rectangle',
                centerRaw: { x: 400, y: 320 },
                sizeRaw: { width: 100, height: 100 },
                center: { x: 0, y: 0 },
                size: { width: 2, height: 2 },
                inRegion: false
            })),
            
            // Screen dimensions
            screenW: 1920,
            screenH: 1080,
            screenHalfX: 15.0,  // degrees
            screenHalfY: 10.0,  // degrees
            
            // Region status masks
            eyeWindowStatusMask: 0,
            touchWindowStatusMask: 0,
            
            // Virtual inputs
            virtualEyeEnabled: false,
            virtualTouchEnabled: false,
            
            virtualEye: {
                x: 0,
                y: 0,
                active: false,
                isDragging: false,
                dragOffset: { x: 0, y: 0 }
            },
            
            virtualTouch: {
                x: 0,
                y: 0,
                active: false,
                isDragging: false,
                dragStartPos: { x: 0, y: 0 },
                lastDragPos: { x: 0, y: 0 }
            }
        };
        
        // UI elements
        this.setupUI();
        
        // Animation
        this.animationId = null;
        
        // Debug: expose state for console inspection
        if (typeof window !== 'undefined') {
            window.eyeTouchVizDebug = this;
        }
        
        // Setup
        this.setupCanvas();
        this.setupSubscriptions();
        this.setupConnectionListeners();
        this.startAnimation();
        
        // Request initial region data
        this.refreshWindows();
    }
    
    setupUI() {
        // Create UI container if it doesn't exist
        const parent = this.canvas.parentElement;
        
        // Look for or create header
        let header = parent.querySelector('.eyetouch-header');
        if (!header) {
            header = document.createElement('div');
            header.className = 'eyetouch-header';
            parent.insertBefore(header, this.canvas);
        }
        
        // Wrap canvas in a centering container if not already wrapped
        if (!this.canvas.parentElement.classList.contains('eyetouch-canvas-wrapper')) {
            const wrapper = document.createElement('div');
            wrapper.className = 'eyetouch-canvas-wrapper';
            this.canvas.parentNode.insertBefore(wrapper, this.canvas);
            wrapper.appendChild(this.canvas);
            this.canvasWrapper = wrapper;
        } else {
            this.canvasWrapper = this.canvas.parentElement;
        }
        
        // Create window indicators and controls - inline layout
        header.innerHTML = `
            <div style="display: flex; flex-direction: column; gap: 3px; width: 100%;">
                <div style="display: flex; align-items: center; justify-content: space-between;">
                    <label class="eyetouch-checkbox">
                        <input type="checkbox" id="eyetouch-virtual-eye">
                        <span>Virtual Eye</span>
                    </label>
                    <div style="display: flex; align-items: center; gap: 6px;">
                        <span style="font-size: 10px;">Eye:</span>
                        <div class="window-indicators">
                            ${Array(8).fill(0).map((_, i) => 
                                `<span class="window-dot" data-type="eye" data-index="${i}">${i}</span>`
                            ).join('')}
                        </div>
                    </div>
                </div>
                <div style="display: flex; align-items: center; justify-content: space-between;">
                    <label class="eyetouch-checkbox">
                        <input type="checkbox" id="eyetouch-virtual-touch">
                        <span>Virtual Touch</span>
                    </label>
                    <div style="display: flex; align-items: center; gap: 6px;">
                        <span style="font-size: 10px;">Touch:</span>
                        <div class="window-indicators">
                            ${Array(8).fill(0).map((_, i) => 
                                `<span class="window-dot touch-dot" data-type="touch" data-index="${i}">${i}</span>`
                            ).join('')}
                        </div>
                    </div>
                </div>
            </div>
        `;
        
        // No toolbar needed - save the space!
        // Users can disable checkboxes to "reset"
        
        // Explicitly ensure checkboxes start unchecked (don't trust browser state)
        setTimeout(() => {
            const virtualEyeCheck = document.getElementById('eyetouch-virtual-eye');
            const virtualTouchCheck = document.getElementById('eyetouch-virtual-touch');
            
            if (virtualEyeCheck) virtualEyeCheck.checked = false;
            if (virtualTouchCheck) virtualTouchCheck.checked = false;
        }, 0);
        
        // Create info overlay
        let overlay = parent.querySelector('.eyetouch-info');
        if (!overlay) {
            overlay = document.createElement('div');
            overlay.className = 'eyetouch-info';
            overlay.innerHTML = `
                <div class="coordinate-info">
                    Eye: <span id="eyetouch-eye-coords">0.00°, 0.00°</span><br>
                    <span id="eyetouch-touch-coords" style="display: none;">Touch: 0, 0</span><br>
                    <span id="eyetouch-virtual-eye-coords" style="display: none;">Virtual Eye: 0.00°, 0.00°</span><br>
                    <span id="eyetouch-virtual-touch-coords" style="display: none;">Virtual Touch: 0, 0</span>
                </div>
            `;
            parent.appendChild(overlay);
        }
        
        // Bind event handlers
        this.bindUIEvents();
    }
    
    bindUIEvents() {
        // Virtual eye checkbox (now in header)
        const virtualEyeCheck = document.getElementById('eyetouch-virtual-eye');
        if (virtualEyeCheck) {
            virtualEyeCheck.addEventListener('change', (e) => {
                this.setVirtualEyeEnabled(e.target.checked);
            });
        }
        
        // Virtual touch checkbox
        const virtualTouchCheck = document.getElementById('eyetouch-virtual-touch');
        if (virtualTouchCheck) {
            virtualTouchCheck.addEventListener('change', (e) => {
                this.setVirtualTouchEnabled(e.target.checked);
            });
        }
        
        // Canvas mouse events
        this.canvas.addEventListener('mousedown', (e) => this.handleMouseDown(e));
        this.canvas.addEventListener('mousemove', (e) => this.handleMouseMove(e));
        this.canvas.addEventListener('mouseup', (e) => this.handleMouseUp(e));
        this.canvas.addEventListener('mouseleave', (e) => this.handleMouseLeave(e));
        
        // Touch events for mobile/Safari compatibility
        this.canvas.addEventListener('touchstart', (e) => this.handleTouchStart(e), { passive: false });
        this.canvas.addEventListener('touchmove', (e) => this.handleTouchMove(e), { passive: false });
        this.canvas.addEventListener('touchend', (e) => this.handleTouchEnd(e));
        this.canvas.addEventListener('touchcancel', (e) => this.handleTouchEnd(e));
        
        // Prevent default drag behavior on canvas (helps Safari)
        this.canvas.style.touchAction = 'none';
        this.canvas.style.webkitUserSelect = 'none';
        this.canvas.style.userSelect = 'none';
    }
    
    setupCanvas() {
        // Handle resize - observe the wrapper, not the parent
        this.resizeObserver = new ResizeObserver(() => this.resize());
        const observeTarget = this.canvasWrapper || this.canvas.parentElement;
        this.resizeObserver.observe(observeTarget);
        this.resize();
    }
    
    resize() {
        // Get available space from wrapper
        const wrapper = this.canvasWrapper || this.canvas.parentElement;
        const rect = wrapper.getBoundingClientRect();
        
        const availW = rect.width;
        const availH = rect.height;
        
        // Use square aspect ratio (1:1) - take the smaller dimension
        const size = Math.max(Math.floor(Math.min(availW, availH)), 100);
        
        this.canvas.width = size;
        this.canvas.height = size;
        
        this.draw();
    }
    
    setupSubscriptions() {
        // Eye position
        this.dpManager.subscribe('ess/em_pos', (data) => {
            // Don't update if virtual eye is enabled
            if (this.state.virtualEyeEnabled) return;
            
            const valueStr = String(data.value);
            const [x, y] = valueStr.split(' ').map(parseFloat);
            
            if (!isNaN(x) && !isNaN(y)) {
                this.state.eyePos = { x, y };
                
                // Update info display
                this.updateInfoDisplay();
                
                // Add to trail
                if (this.state.showTrails) {
                    this.state.eyeTrail.push({ x, y, time: Date.now() });
                    
                    // Limit trail length
                    if (this.state.eyeTrail.length > this.state.maxTrailLength) {
                        this.state.eyeTrail.shift();
                    }
                }
            }
        });
        
        // Touch events - handle binary format
        this.dpManager.subscribe('mtouch/event', (data) => {
            // Don't update if virtual touch is active
            if (this.state.virtualTouchEnabled && this.state.virtualTouch.active) return;
            
            try {
                const value = data.value || data.data;
                let x, y, eventType;
                
                if (Array.isArray(value)) {
                    if (value.length < 3) return;
                    [x, y, eventType] = value.map(Number);
                } else if (typeof value === 'string') {
                    const parts = value.split(' ');
                    if (parts.length < 3) return;
                    [x, y, eventType] = parts.map(Number);
                } else {
                    return;
                }
                
                if (isNaN(x) || isNaN(y) || isNaN(eventType)) return;
                
                switch (eventType) {
                    case 0: // Press
                        this.state.touchPos = { x, y };
                        this.state.showTouch = true;
                        break;
                    case 1: // Drag
                        if (this.state.showTouch) {
                            this.state.touchPos = { x, y };
                        }
                        break;
                    case 2: // Release
                        this.state.showTouch = false;
                        break;
                }
                
                this.updateInfoDisplay();
            } catch (error) {
                console.error('Error handling touch event:', error);
            }
        });
        
        // Eye region settings
        this.dpManager.subscribe('ess/em_region_setting', (data) => {
            // Split on whitespace (handles multiple spaces) and filter empty strings
            const parts = String(data.value).trim().split(/\s+/).map(parseFloat);
            if (parts.length >= 8) {
                const [win, active, state, type, cx, cy, pmx, pmy] = parts;
                if (win >= 0 && win < 8 && !isNaN(cx) && !isNaN(cy)) {
                    this.state.eyeRegions[win] = {
                        id: win,
                        active: active === 1,
                        state: state,
                        type: type === 1 ? 'ellipse' : 'rectangle',
                        center: { x: cx, y: cy },
                        size: { width: pmx, height: pmy },
                        inRegion: this.state.eyeRegions[win]?.inRegion || false
                    };
                    this.updateWindowIndicators();
                }
            }
        });
        
        // Eye region status
        this.dpManager.subscribe('ess/em_region_status', (data) => {
            // Split on whitespace (handles multiple spaces)
            const parts = String(data.value).trim().split(/\s+/).map(parseFloat);
            if (parts.length >= 4) {
                const [changes, states, x, y] = parts;
                this.state.eyeWindowStatusMask = states;
                
                // Update region states based on bitmask
                this.state.eyeRegions.forEach((region, i) => {
                    if (region) {
                        region.inRegion = (states & (1 << i)) !== 0;
                    }
                });
                
                this.updateWindowIndicators();
            }
        });
        
        // Touch region settings
        this.dpManager.subscribe('ess/touch_region_setting', (data) => {
            // Split on whitespace (handles multiple spaces)
            const parts = String(data.value).trim().split(/\s+/).map(parseFloat);
            if (parts.length >= 8) {
                const [reg, active, state, type, cx, cy, dx, dy] = parts;
                if (reg >= 0 && reg < 8 && !isNaN(cx) && !isNaN(cy)) {
                    // Convert touch pixels to degrees
                    const centerDeg = this.touchPixelsToDegrees(cx, cy);
                    const sizeDeg = {
                        width: Math.abs(dx / this.state.screenW * (2 * this.state.screenHalfX)),
                        height: Math.abs(dy / this.state.screenH * (2 * this.state.screenHalfY))
                    };
                    
                    this.state.touchRegions[reg] = {
                        id: reg,
                        active: active === 1,
                        state: state,
                        type: type === 1 ? 'ellipse' : 'rectangle',
                        centerRaw: { x: cx, y: cy },
                        sizeRaw: { width: dx, height: dy },
                        center: centerDeg,
                        size: sizeDeg,
                        inRegion: this.state.touchRegions[reg]?.inRegion || false
                    };
                    this.updateWindowIndicators();
                }
            }
        });
        
        // Touch region status
        this.dpManager.subscribe('ess/touch_region_status', (data) => {
            // Split on whitespace (handles multiple spaces)
            const parts = String(data.value).trim().split(/\s+/).map(parseFloat);
            if (parts.length >= 4) {
                const [changes, states, touch_x, touch_y] = parts;
                this.state.touchWindowStatusMask = states;
                
                // Update region states based on bitmask
                this.state.touchRegions.forEach((region, i) => {
                    if (region) {
                        region.inRegion = (states & (1 << i)) !== 0;
                    }
                });
                
                this.updateWindowIndicators();
            }
        });
        
        // Screen dimensions
        this.dpManager.subscribe('ess/screen_w', (data) => {
            this.state.screenW = parseInt(data.value) || 1920;
        });
        
        this.dpManager.subscribe('ess/screen_h', (data) => {
            this.state.screenH = parseInt(data.value) || 1080;
        });
        
        this.dpManager.subscribe('ess/screen_halfx', (data) => {
            this.state.screenHalfX = parseFloat(data.value) || 15.0;
        });
        
        this.dpManager.subscribe('ess/screen_halfy', (data) => {
            this.state.screenHalfY = parseFloat(data.value) || 10.0;
        });
    }
    
    /**
     * Setup connection event listeners for reconnection handling
     */
    setupConnectionListeners() {
        if (this.connection) {
            // Refresh windows when connection is established/re-established
            this.connection.on('connected', () => {
                // Small delay to ensure connection is fully ready
                setTimeout(() => this.refreshWindows(), 100);
            });
        }
    }
    
    /**
     * Request current region settings from server
     * This triggers the datapoint updates that populate our region state
     */
    async refreshWindows() {
        if (!this.connection) {
            return;
        }
        
        if (!this.connection.connected) {
            return;
        }
        
        const subprocess = this.options.essSubprocess;
        
        try {
            // Request all eye region settings at once (matches FLTK approach)
            await this.connection.send(
                'for {set i 0} {$i < 8} {incr i} {ainGetRegionInfo $i}',
                subprocess
            );
        } catch (e) {
            // Silently ignore - regions may not be available
        }
        
        try {
            // Request all touch region settings at once
            await this.connection.send(
                'for {set i 0} {$i < 8} {incr i} {touchGetRegionInfo $i}',
                subprocess
            );
        } catch (e) {
            // Silently ignore - regions may not be available
        }
    }
    
    // Coordinate transformation functions
    get degPerPixel() {
        const w = this.canvas.width;
        const h = this.canvas.height;
        
        // Calculate scale to fit the extent in the canvas
        // while maintaining circular aspect ratio
        const scaleX = this.xExtent / w;
        const scaleY = this.yExtent / h;
        
        // Use the same scale for both dimensions (ensures circles stay circular)
        const scale = Math.max(scaleX, scaleY);
        
        return { x: scale, y: scale };
    }
    
    get canvasCenter() {
        return {
            x: this.canvas.width / 2,
            y: this.canvas.height / 2
        };
    }
    
    /**
     * Convert degrees to canvas coordinates
     */
    degreesToCanvas(degX, degY) {
        const center = this.canvasCenter;
        const degPP = this.degPerPixel;
        return {
            x: center.x + (degX / degPP.x),
            y: center.y - (degY / degPP.y)  // Negative because canvas Y increases downward
        };
    }
    
    /**
     * Convert canvas coordinates to degrees
     */
    canvasToDegrees(canvasX, canvasY) {
        const center = this.canvasCenter;
        const degPP = this.degPerPixel;
        return {
            x: (canvasX - center.x) * degPP.x,
            y: -(canvasY - center.y) * degPP.y
        };
    }
    
    /**
     * Convert touch screen pixels to degrees
     */
    touchPixelsToDegrees(pixX, pixY) {
        const screen = this.state;
        const screenPixPerDegX = screen.screenW / (2 * screen.screenHalfX);
        const screenPixPerDegY = screen.screenH / (2 * screen.screenHalfY);
        // Note: No Y flip here - touch screen coords have Y increasing downward
        // The flip happens in degreesToCanvas when drawing
        return {
            x: (pixX - screen.screenW / 2) / screenPixPerDegX,
            y: (pixY - screen.screenH / 2) / screenPixPerDegY
        };
    }
    
    /**
     * Convert degrees to touch screen pixels
     */
    degreesToTouchPixels(degX, degY) {
        const screen = this.state;
        const screenPixPerDegX = screen.screenW / (2 * screen.screenHalfX);
        const screenPixPerDegY = screen.screenH / (2 * screen.screenHalfY);
        return {
            x: Math.round(degX * screenPixPerDegX + screen.screenW / 2),
            y: Math.round(-degY * screenPixPerDegY + screen.screenH / 2)
        };
    }
    
    // Mouse event handlers
    handleMouseDown(event) {
        const rect = this.canvas.getBoundingClientRect();
        
        // Account for canvas scaling (display size vs internal resolution)
        const scaleX = this.canvas.width / rect.width;
        const scaleY = this.canvas.height / rect.height;
        const mouseX = (event.clientX - rect.left) * scaleX;
        const mouseY = (event.clientY - rect.top) * scaleY;
        
        // Check if clicking on virtual eye marker
        if (this.state.virtualEyeEnabled && this.state.virtualEye.active) {
            const eyePos = this.degreesToCanvas(this.state.virtualEye.x, this.state.virtualEye.y);
            const eyeDistance = Math.sqrt(
                Math.pow(mouseX - eyePos.x, 2) + 
                Math.pow(mouseY - eyePos.y, 2)
            );
            
            if (eyeDistance <= 13) {
                this.state.virtualEye.isDragging = true;
                this.state.virtualEye.dragOffset = {
                    x: eyePos.x - mouseX,
                    y: eyePos.y - mouseY
                };
                event.preventDefault();
                return;
            }
        }
        
        // Handle virtual touch start
        if (this.state.virtualTouchEnabled && !this.state.virtualEye.isDragging) {
            const degPos = this.canvasToDegrees(mouseX, mouseY);
            const touchPixels = this.degreesToTouchPixels(degPos.x, degPos.y);
            
            this.state.virtualTouch.x = touchPixels.x;
            this.state.virtualTouch.y = touchPixels.y;
            this.state.virtualTouch.active = true;
            this.state.virtualTouch.isDragging = true;
            this.state.virtualTouch.dragStartPos = { x: touchPixels.x, y: touchPixels.y };
            this.state.virtualTouch.lastDragPos = { x: touchPixels.x, y: touchPixels.y };
            
            this.sendVirtualTouchData(touchPixels.x, touchPixels.y, 0); // PRESS
            this.updateInfoDisplay();
            event.preventDefault();
        }
    }
    
    handleMouseMove(event) {
        const rect = this.canvas.getBoundingClientRect();
        
        // Account for canvas scaling (display size vs internal resolution)
        const scaleX = this.canvas.width / rect.width;
        const scaleY = this.canvas.height / rect.height;
        const mouseX = (event.clientX - rect.left) * scaleX;
        const mouseY = (event.clientY - rect.top) * scaleY;
        
        if (this.state.virtualEyeEnabled && this.state.virtualEye.isDragging) {
            const newCanvasX = mouseX + this.state.virtualEye.dragOffset.x;
            const newCanvasY = mouseY + this.state.virtualEye.dragOffset.y;
            const degPos = this.canvasToDegrees(newCanvasX, newCanvasY);
            
            // Clamp to visible range
            const maxX = this.xExtent / 2;
            const maxY = this.yExtent / 2;
            degPos.x = Math.max(-maxX, Math.min(maxX, degPos.x));
            degPos.y = Math.max(-maxY, Math.min(maxY, degPos.y));
            
            this.updateVirtualEyePosition(degPos.x, degPos.y);
        }
        
        if (this.state.virtualTouchEnabled && this.state.virtualTouch.isDragging) {
            const degPos = this.canvasToDegrees(mouseX, mouseY);
            const touchPixels = this.degreesToTouchPixels(degPos.x, degPos.y);
            
            const dragThreshold = 2;
            if (Math.abs(touchPixels.x - this.state.virtualTouch.lastDragPos.x) > dragThreshold ||
                Math.abs(touchPixels.y - this.state.virtualTouch.lastDragPos.y) > dragThreshold) {
                
                this.state.virtualTouch.x = touchPixels.x;
                this.state.virtualTouch.y = touchPixels.y;
                this.state.virtualTouch.lastDragPos = { x: touchPixels.x, y: touchPixels.y };
                
                this.sendVirtualTouchData(touchPixels.x, touchPixels.y, 1); // DRAG
                this.updateInfoDisplay();
            }
        }
    }
    
    handleMouseUp(event) {
        if (this.state.virtualEye.isDragging) {
            this.state.virtualEye.isDragging = false;
        }
        
        if (this.state.virtualTouch.isDragging) {
            this.sendVirtualTouchData(
                this.state.virtualTouch.x, 
                this.state.virtualTouch.y, 
                2  // RELEASE
            );
            this.state.virtualTouch.isDragging = false;
            
            // Keep visible briefly after release
            setTimeout(() => {
                if (!this.state.virtualTouch.isDragging) {
                    this.state.virtualTouch.active = false;
                }
            }, 500);
        }
    }
    
    handleMouseLeave(event) {
        if (this.state.virtualEye.isDragging) {
            this.state.virtualEye.isDragging = false;
        }
        
        if (this.state.virtualTouch.isDragging) {
            this.sendVirtualTouchData(
                this.state.virtualTouch.x,
                this.state.virtualTouch.y,
                2  // RELEASE
            );
            this.state.virtualTouch.isDragging = false;
            this.state.virtualTouch.active = false;
        }
    }
    
    // Touch event handlers (for Safari/mobile compatibility)
    handleTouchStart(event) {
        if (event.touches.length === 1) {
            const touch = event.touches[0];
            // Create a mouse-like event object
            const mouseEvent = {
                clientX: touch.clientX,
                clientY: touch.clientY,
                preventDefault: () => event.preventDefault()
            };
            this.handleMouseDown(mouseEvent);
            event.preventDefault();
        }
    }
    
    handleTouchMove(event) {
        if (event.touches.length === 1) {
            const touch = event.touches[0];
            const mouseEvent = {
                clientX: touch.clientX,
                clientY: touch.clientY
            };
            this.handleMouseMove(mouseEvent);
            event.preventDefault();
        }
    }
    
    handleTouchEnd(event) {
        this.handleMouseUp({});
    }
    
    // Virtual input control
    updateVirtualEyePosition(degX, degY) {
        this.state.virtualEye.x = degX;
        this.state.virtualEye.y = degY;
        this.state.virtualEye.active = true;
        this.sendVirtualEyeTarget(degX, degY);
        this.updateInfoDisplay();
    }
    
    sendVirtualEyeTarget(x, y) {
        // Fire-and-forget - don't wait for response (critical for responsiveness)
        try {
            const conn = this.connection || (this.dpManager && this.dpManager.connection);
            if (conn && conn.connected && conn.ws) {
                // Send directly to virtual_eye subprocess (like FLTK does)
                conn.ws.send(`send virtual_eye {set_eye ${x} ${y}}`);
            }
        } catch (error) {
            // Silently ignore errors during drag
        }
    }
    
    async setVirtualEyeControl(enabled) {
        try {
            const cmd = enabled ? 'start' : 'stop';
            const conn = this.connection || (this.dpManager && this.dpManager.connection);
            if (conn && conn.connected && conn.ws) {
                // Send directly to virtual_eye subprocess
                conn.ws.send(`send virtual_eye ${cmd}`);
            }
        } catch (error) {
            console.error('Failed to set virtual eye control:', error);
        }
    }
    
    sendVirtualTouchData(x, y, eventType = 0) {
        // Fire-and-forget for responsiveness during drag
        try {
            const conn = this.connection || (this.dpManager && this.dpManager.connection);
            if (conn && conn.connected && conn.ws) {
                // Send dservSetData command directly (like FLTK does)
                const cmd = `set d [binary format s3 {${x} ${y} ${eventType}}]; dservSetData mtouch/event 0 4 $d; unset d`;
                conn.ws.send(cmd);
            }
        } catch (error) {
            console.error('Failed to send virtual touch data:', error);
        }
    }
    
    setVirtualEyeEnabled(enabled) {
        this.state.virtualEyeEnabled = enabled;
        this.setVirtualEyeControl(enabled);
        
        if (enabled) {
            // Initialize virtual eye at current eye position or center
            if (this.state.eyePos.x !== 0 || this.state.eyePos.y !== 0) {
                this.updateVirtualEyePosition(this.state.eyePos.x, this.state.eyePos.y);
            } else {
                this.updateVirtualEyePosition(0, 0);
            }
            
            // Update cursor
            this.canvas.style.cursor = 'crosshair';
        } else {
            this.state.virtualEye.active = false;
            this.state.virtualEye.isDragging = false;
            this.canvas.style.cursor = this.state.virtualTouchEnabled ? 'pointer' : 'default';
        }
        
        this.updateInfoDisplay();
    }
    
    setVirtualTouchEnabled(enabled) {
        this.state.virtualTouchEnabled = enabled;
        
        if (!enabled) {
            if (this.state.virtualTouch.isDragging) {
                this.sendVirtualTouchData(
                    this.state.virtualTouch.x,
                    this.state.virtualTouch.y,
                    2  // RELEASE
                );
            }
            this.state.virtualTouch.active = false;
            this.state.virtualTouch.isDragging = false;
            this.canvas.style.cursor = this.state.virtualEyeEnabled ? 'crosshair' : 'default';
        } else {
            this.canvas.style.cursor = this.state.virtualEyeEnabled ? 'crosshair' : 'pointer';
        }
        
        this.updateInfoDisplay();
    }
    
    resetVirtualEye() {
        this.updateVirtualEyePosition(0, 0);
    }
    
    resetVirtualTouch() {
        this.state.virtualTouch.active = false;
        this.state.virtualTouch.isDragging = false;
        this.state.virtualTouch.x = 0;
        this.state.virtualTouch.y = 0;
        this.updateInfoDisplay();
    }
    
    /**
     * Enable/disable eye trails programmatically
     * (UI toggle removed to save space, but feature still available)
     */
    setTrailsEnabled(enabled) {
        this.state.showTrails = enabled;
        if (!enabled) {
            this.state.eyeTrail = [];
        }
    }
    
    // UI updates
    updateWindowIndicators() {
        // Update eye window indicators
        this.state.eyeRegions.forEach((region, i) => {
            const dot = document.querySelector(`.window-dot[data-type="eye"][data-index="${i}"]`);
            if (dot) {
                dot.classList.toggle('active', region.active);
                dot.classList.toggle('inside', region.inRegion);
            }
        });
        
        // Update touch window indicators
        this.state.touchRegions.forEach((region, i) => {
            const dot = document.querySelector(`.window-dot[data-type="touch"][data-index="${i}"]`);
            if (dot) {
                dot.classList.toggle('active', region.active);
                dot.classList.toggle('inside', region.inRegion);
            }
        });
    }
    
    updateInfoDisplay() {
        // Eye coordinates
        const eyeCoords = document.getElementById('eyetouch-eye-coords');
        if (eyeCoords) {
            eyeCoords.textContent = `${this.state.eyePos.x.toFixed(2)}°, ${this.state.eyePos.y.toFixed(2)}°`;
        }
        
        // Touch coordinates
        const touchCoords = document.getElementById('eyetouch-touch-coords');
        if (touchCoords) {
            if (this.state.showTouch && (this.state.touchPos.x !== 0 || this.state.touchPos.y !== 0)) {
                touchCoords.textContent = `Touch: ${this.state.touchPos.x}, ${this.state.touchPos.y}`;
                touchCoords.style.display = 'block';
            } else {
                touchCoords.style.display = 'none';
            }
        }
        
        // Virtual eye coordinates
        const virtualEyeCoords = document.getElementById('eyetouch-virtual-eye-coords');
        if (virtualEyeCoords) {
            if (this.state.virtualEyeEnabled && this.state.virtualEye.active) {
                virtualEyeCoords.textContent = `Virtual Eye: ${this.state.virtualEye.x.toFixed(2)}°, ${this.state.virtualEye.y.toFixed(2)}°`;
                virtualEyeCoords.style.display = 'block';
            } else {
                virtualEyeCoords.style.display = 'none';
            }
        }
        
        // Virtual touch coordinates
        const virtualTouchCoords = document.getElementById('eyetouch-virtual-touch-coords');
        if (virtualTouchCoords) {
            if (this.state.virtualTouchEnabled && this.state.virtualTouch.active) {
                virtualTouchCoords.textContent = `Virtual Touch: ${this.state.virtualTouch.x}, ${this.state.virtualTouch.y}`;
                virtualTouchCoords.style.display = 'block';
            } else {
                virtualTouchCoords.style.display = 'none';
            }
        }
    }
    
    // Animation
    startAnimation() {
        const animate = () => {
            this.draw();
            this.animationId = requestAnimationFrame(animate);
        };
        animate();
    }
    
    stopAnimation() {
        if (this.animationId) {
            cancelAnimationFrame(this.animationId);
            this.animationId = null;
        }
    }
    
    // Drawing functions
    draw() {
        const { ctx, canvas, options } = this;
        const { width, height } = canvas;
        
        // Clear
        ctx.fillStyle = options.backgroundColor;
        ctx.fillRect(0, 0, width, height);
        
        // Draw grid
        this.drawGrid();
        
        // Draw trails
        if (this.state.showTrails) {
            this.drawTrails();
        }
        
        // Draw eye regions
        this.state.eyeRegions.forEach(region => {
            if (region.active) {
                this.drawEyeRegion(region);
            }
        });
        
        // Draw touch regions
        this.state.touchRegions.forEach(region => {
            if (region && region.active) {
                this.drawTouchRegion(region);
            }
        });
        
        // Draw markers
        if (this.state.virtualEyeEnabled && this.state.virtualEye.active) {
            this.drawVirtualEye();
        } else if (!this.state.virtualEyeEnabled) {
            this.drawEyePosition();
        }
        
        if (this.state.virtualTouchEnabled && this.state.virtualTouch.active) {
            this.drawVirtualTouch();
        } else if (this.state.showTouch) {
            this.drawTouchPosition();
        }
    }
    
    drawGrid() {
        const { ctx, canvas, options } = this;
        
        // Draw degree lines
        ctx.strokeStyle = options.gridColor;
        ctx.lineWidth = 1;
        
        for (let deg = -15; deg <= 15; deg += 5) {
            const pos = this.degreesToCanvas(deg, 0);
            ctx.beginPath();
            ctx.moveTo(pos.x, 0);
            ctx.lineTo(pos.x, canvas.height);
            ctx.stroke();
        }
        
        for (let deg = -15; deg <= 15; deg += 5) {
            const pos = this.degreesToCanvas(0, deg);
            ctx.beginPath();
            ctx.moveTo(0, pos.y);
            ctx.lineTo(canvas.width, pos.y);
            ctx.stroke();
        }
        
        // Draw center crosshair
        ctx.strokeStyle = options.centerCrossColor;
        ctx.lineWidth = 2;
        
        const center = this.canvasCenter;
        ctx.beginPath();
        ctx.moveTo(center.x - 10, center.y);
        ctx.lineTo(center.x + 10, center.y);
        ctx.moveTo(center.x, center.y - 10);
        ctx.lineTo(center.x, center.y + 10);
        ctx.stroke();
    }
    
    drawTrails() {
        const { ctx } = this;
        const trail = this.state.eyeTrail;
        
        if (trail.length < 2) return;
        
        ctx.strokeStyle = this.options.eyeTrailColor;
        ctx.lineWidth = 2;
        ctx.globalAlpha = 0.5;
        
        ctx.beginPath();
        trail.forEach((point, i) => {
            const pos = this.degreesToCanvas(point.x, point.y);
            if (i === 0) {
                ctx.moveTo(pos.x, pos.y);
            } else {
                ctx.lineTo(pos.x, pos.y);
            }
        });
        ctx.stroke();
        
        ctx.globalAlpha = 1.0;
    }
    
    drawEyeRegion(region) {
        const { ctx, options } = this;
        const pos = this.degreesToCanvas(region.center.x, region.center.y);
        const degPP = this.degPerPixel;
        const halfW = region.size.width / degPP.x;
        const halfH = region.size.height / degPP.y;
        
        // Draw fill when inside
        if (region.inRegion) {
            ctx.fillStyle = options.eyeRegionFill;
            if (region.type === 'ellipse') {
                ctx.beginPath();
                ctx.ellipse(pos.x, pos.y, halfW, halfH, 0, 0, 2 * Math.PI);
                ctx.fill();
            } else {
                ctx.fillRect(pos.x - halfW, pos.y - halfH, 2 * halfW, 2 * halfH);
            }
        }
        
        // Draw outline
        ctx.strokeStyle = options.eyeRegionColor;
        ctx.lineWidth = region.inRegion ? 2 : 1;
        
        if (region.type === 'ellipse') {
            ctx.beginPath();
            ctx.ellipse(pos.x, pos.y, halfW, halfH, 0, 0, 2 * Math.PI);
            ctx.stroke();
        } else {
            ctx.strokeRect(pos.x - halfW, pos.y - halfH, 2 * halfW, 2 * halfH);
        }
        
        // Draw center dot
        ctx.fillStyle = options.eyeRegionColor;
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 2, 0, 2 * Math.PI);
        ctx.fill();
        
        // Draw label
        ctx.fillStyle = options.eyeRegionColor;
        ctx.font = '12px monospace';
        ctx.fillText(`E${region.id}`, pos.x - halfW + 2, pos.y - halfH - 5);
    }
    
    drawTouchRegion(region) {
        const { ctx, options } = this;
        // Touch regions: Y is NOT flipped (unlike eye which uses standard math coords)
        // Touch screen Y=0 is top, increases downward - same as canvas
        const center = this.canvasCenter;
        const degPP = this.degPerPixel;
        const pos = {
            x: center.x + (region.center.x / degPP.x),
            y: center.y + (region.center.y / degPP.y)  // Note: ADD, not subtract
        };
        const halfW = region.size.width / degPP.x;
        const halfH = region.size.height / degPP.y;
        
        // Draw fill when inside
        if (region.inRegion) {
            ctx.fillStyle = options.touchRegionFill;
            if (region.type === 'ellipse') {
                ctx.beginPath();
                ctx.ellipse(pos.x, pos.y, halfW, halfH, 0, 0, 2 * Math.PI);
                ctx.fill();
            } else {
                ctx.fillRect(pos.x - halfW, pos.y - halfH, 2 * halfW, 2 * halfH);
            }
        }
        
        // Draw outline
        ctx.strokeStyle = options.touchRegionColor;
        ctx.lineWidth = region.inRegion ? 2 : 1;
        
        if (region.type === 'ellipse') {
            ctx.beginPath();
            ctx.ellipse(pos.x, pos.y, halfW, halfH, 0, 0, 2 * Math.PI);
            ctx.stroke();
        } else {
            ctx.strokeRect(pos.x - halfW, pos.y - halfH, 2 * halfW, 2 * halfH);
        }
        
        // Draw center dot
        ctx.fillStyle = options.touchRegionColor;
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 2, 0, 2 * Math.PI);
        ctx.fill();
        
        // Draw label
        ctx.fillStyle = options.touchRegionColor;
        ctx.font = '12px monospace';
        ctx.fillText(`T${region.id}`, pos.x + halfW - 20, pos.y + halfH + 15);
    }
    
    drawEyePosition() {
        const { ctx, options } = this;
        const pos = this.degreesToCanvas(this.state.eyePos.x, this.state.eyePos.y);
        
        // Draw filled circle
        ctx.fillStyle = options.eyeColor;
        ctx.strokeStyle = options.eyeStrokeColor;
        ctx.lineWidth = 2;
        
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 5, 0, 2 * Math.PI);
        ctx.fill();
        ctx.stroke();
        
        // Draw crosshair
        ctx.strokeStyle = options.eyeColor;
        ctx.lineWidth = 1;
        
        ctx.beginPath();
        ctx.moveTo(pos.x - 10, pos.y);
        ctx.lineTo(pos.x + 10, pos.y);
        ctx.moveTo(pos.x, pos.y - 10);
        ctx.lineTo(pos.x, pos.y + 10);
        ctx.stroke();
    }
    
    drawVirtualEye() {
        const { ctx, options } = this;
        const pos = this.degreesToCanvas(this.state.virtualEye.x, this.state.virtualEye.y);
        
        // Draw circle
        ctx.fillStyle = options.eyeColor;
        ctx.strokeStyle = this.state.virtualEye.isDragging ? '#00ff00' : options.virtualEyeColor;
        ctx.lineWidth = 2;
        
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 8, 0, 2 * Math.PI);
        ctx.fill();
        ctx.stroke();
        
        // Draw crosshair
        ctx.strokeStyle = '#000000';
        ctx.lineWidth = 1;
        
        ctx.beginPath();
        ctx.moveTo(pos.x - 6, pos.y);
        ctx.lineTo(pos.x + 6, pos.y);
        ctx.moveTo(pos.x, pos.y - 6);
        ctx.lineTo(pos.x, pos.y + 6);
        ctx.stroke();
        
        // Draw "V" indicator
        ctx.fillStyle = options.virtualEyeColor;
        ctx.font = 'bold 10px monospace';
        ctx.fillText('V', pos.x - 3, pos.y - 12);
    }
    
    drawTouchPosition() {
        const { ctx, options } = this;
        const touchDegrees = this.touchPixelsToDegrees(
            this.state.touchPos.x,
            this.state.touchPos.y
        );
        const pos = this.degreesToCanvas(touchDegrees.x, touchDegrees.y);
        
        // Draw diamond shape
        ctx.fillStyle = options.touchColor;
        ctx.strokeStyle = options.touchStrokeColor;
        ctx.lineWidth = 2;
        
        const size = 6;
        ctx.beginPath();
        ctx.moveTo(pos.x, pos.y - size);
        ctx.lineTo(pos.x + size, pos.y);
        ctx.lineTo(pos.x, pos.y + size);
        ctx.lineTo(pos.x - size, pos.y);
        ctx.closePath();
        ctx.fill();
        ctx.stroke();
    }
    
    drawVirtualTouch() {
        const { ctx, options } = this;
        const touchDegrees = this.touchPixelsToDegrees(
            this.state.virtualTouch.x,
            this.state.virtualTouch.y
        );
        // Touch uses same Y convention as touch regions (ADD, not subtract)
        const center = this.canvasCenter;
        const degPP = this.degPerPixel;
        const pos = {
            x: center.x + (touchDegrees.x / degPP.x),
            y: center.y + (touchDegrees.y / degPP.y)
        };
        
        // Draw diamond shape
        ctx.fillStyle = options.virtualTouchColor;
        ctx.strokeStyle = this.state.virtualTouch.isDragging ? '#ffffff' : '#cc6600';
        ctx.lineWidth = this.state.virtualTouch.isDragging ? 3 : 2;
        
        const size = this.state.virtualTouch.isDragging ? 11 : 9;
        ctx.beginPath();
        ctx.moveTo(pos.x, pos.y - size);
        ctx.lineTo(pos.x + size, pos.y);
        ctx.lineTo(pos.x, pos.y + size);
        ctx.lineTo(pos.x - size, pos.y);
        ctx.closePath();
        ctx.fill();
        ctx.stroke();
        
        // Draw "V" indicator
        ctx.fillStyle = '#ffffff';
        ctx.font = 'bold 8px monospace';
        ctx.fillText('V', pos.x - 2, pos.y + 2);
        
        // Draw drag line
        if (this.state.virtualTouch.isDragging) {
            const startDegrees = this.touchPixelsToDegrees(
                this.state.virtualTouch.dragStartPos.x,
                this.state.virtualTouch.dragStartPos.y
            );
            const startPos = {
                x: center.x + (startDegrees.x / degPP.x),
                y: center.y + (startDegrees.y / degPP.y)
            };
            
            ctx.strokeStyle = 'rgba(255, 140, 0, 0.3)';
            ctx.lineWidth = 1;
            ctx.setLineDash([5, 5]);
            
            ctx.beginPath();
            ctx.moveTo(startPos.x, startPos.y);
            ctx.lineTo(pos.x, pos.y);
            ctx.stroke();
            
            ctx.setLineDash([]);
        }
    }
    
    dispose() {
        this.stopAnimation();
        this.resizeObserver.disconnect();
    }
    
    /**
     * Debug helper - call from console to check state
     */
    debug() {
        return {
            connected: this.connection ? this.connection.connected : false,
            virtualEyeEnabled: this.state.virtualEyeEnabled,
            virtualTouchEnabled: this.state.virtualTouchEnabled,
            eyePos: this.state.eyePos,
            touchPos: this.state.touchPos,
            showTouch: this.state.showTouch,
            eyeRegionsActive: this.state.eyeRegions.filter(r => r.active).length,
            eyeRegions: this.state.eyeRegions,
            touchRegionsActive: this.state.touchRegions.filter(r => r && r.active).length,
            touchRegions: this.state.touchRegions,
            eyeWindowStatusMask: this.state.eyeWindowStatusMask,
            touchWindowStatusMask: this.state.touchWindowStatusMask,
            trailLength: this.state.eyeTrail.length,
            canvasSize: { w: this.canvas.width, h: this.canvas.height },
            degPerPixel: this.degPerPixel,
            // Methods for manual testing
            refresh: () => this.refreshWindows()
        };
    }
}

// Export
if (typeof window !== 'undefined') {
    window.EyeTouchVisualizer = EyeTouchVisualizer;
}