/**
 * EyeTouchVisualizer.js
 * Canvas-based visualization of eye position and touch events
 * 
 * Displays:
 * - Eye position marker
 * - Eye tracking regions (fixation windows)
 * - Touch position and regions
 * - Screen coordinate mapping
 */

class EyeTouchVisualizer {
    constructor(canvas, dpManager, options = {}) {
        this.canvas = typeof canvas === 'string' 
            ? document.getElementById(canvas) 
            : canvas;
        this.ctx = this.canvas.getContext('2d');
        this.dpManager = dpManager;
        
        this.options = {
            backgroundColor: '#1a1a2a',
            eyeColor: '#4ade80',
            eyeTrailColor: 'rgba(74, 222, 128, 0.3)',
            touchColor: '#f87171',
            regionActiveColor: 'rgba(74, 222, 128, 0.3)',
            regionInactiveColor: 'rgba(100, 100, 120, 0.2)',
            regionInColor: 'rgba(74, 222, 128, 0.5)',
            gridColor: 'rgba(255, 255, 255, 0.1)',
            ...options
        };
        
        // State
        this.state = {
            eyePos: { x: 0, y: 0 },
            eyeTrail: [],
            touchPos: { x: 0, y: 0 },
            showTouch: false,
            eyeRegions: [],    // Array of region settings
            touchRegions: [],  // Array of touch region settings
            screenW: 1920,
            screenH: 1080,
            screenHalfX: 15.0,  // degrees
            screenHalfY: 10.0,  // degrees
            pointsPerDegH: 1.0,
            pointsPerDegV: 1.0
        };
        
        // Animation
        this.animationId = null;
        
        // Setup
        this.setupCanvas();
        this.setupSubscriptions();
        this.startAnimation();
    }
    
    setupCanvas() {
        // Handle resize
        this.resizeObserver = new ResizeObserver(() => this.resize());
        this.resizeObserver.observe(this.canvas.parentElement);
        this.resize();
    }
    
    resize() {
        const parent = this.canvas.parentElement;
        const rect = parent.getBoundingClientRect();
        
        // Account for padding
        const style = getComputedStyle(parent);
        const paddingX = parseFloat(style.paddingLeft) + parseFloat(style.paddingRight);
        const paddingY = parseFloat(style.paddingTop) + parseFloat(style.paddingBottom);
        
        this.canvas.width = rect.width - paddingX;
        this.canvas.height = rect.height - paddingY;
        
        this.draw();
    }
    
    setupSubscriptions() {
        // Eye position
        this.dpManager.subscribe('ess/em_pos', (data) => {
            const [x, y] = String(data.value).split(' ').map(parseFloat);
            if (!isNaN(x) && !isNaN(y)) {
                this.state.eyePos = { x, y };
                
                // Add to trail
                this.state.eyeTrail.push({ x, y, time: Date.now() });
                
                // Limit trail length
                const trailDuration = 500; // ms
                const now = Date.now();
                this.state.eyeTrail = this.state.eyeTrail.filter(
                    p => now - p.time < trailDuration
                );
            }
        });
        
        // Touch events
        this.dpManager.subscribe('ess/touch_press', (data) => {
            const [x, y] = String(data.value).split(' ').map(parseInt);
            if (!isNaN(x) && !isNaN(y)) {
                this.state.touchPos = { x, y };
                this.state.showTouch = true;
            }
        });
        
        this.dpManager.subscribe('ess/touch_release', () => {
            this.state.showTouch = false;
        });
        
        this.dpManager.subscribe('ess/touch_drag', (data) => {
            const [x, y] = String(data.value).split(' ').map(parseInt);
            if (!isNaN(x) && !isNaN(y)) {
                this.state.touchPos = { x, y };
            }
        });
        
        // Eye region settings
        this.dpManager.subscribe('ess/em_region_setting', (data) => {
            const parts = String(data.value).split(' ').map(parseFloat);
            if (parts.length >= 8) {
                const [win, active, state, type, cx, cy, pmx, pmy] = parts;
                this.state.eyeRegions[win] = { active, state, type, cx, cy, pmx, pmy };
            }
        });
        
        // Eye region status
        this.dpManager.subscribe('ess/em_region_status', (data) => {
            const parts = String(data.value).split(' ').map(parseFloat);
            if (parts.length >= 4) {
                const [changes, states, x, y] = parts;
                // Update region states based on bitmask
                this.state.eyeRegions.forEach((region, i) => {
                    if (region) {
                        region.inRegion = (states & (1 << i)) !== 0;
                    }
                });
            }
        });
        
        // Touch region settings
        this.dpManager.subscribe('ess/touch_region_setting', (data) => {
            const parts = String(data.value).split(' ').map(parseInt);
            if (parts.length >= 8) {
                const [win, active, state, type, cx, cy, w, h] = parts;
                this.state.touchRegions[win] = { active, state, type, cx, cy, w, h };
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
    
    /**
     * Convert degrees to canvas coordinates
     */
    degToCanvas(degX, degY) {
        const { screenHalfX, screenHalfY } = this.state;
        const cx = this.canvas.width / 2;
        const cy = this.canvas.height / 2;
        
        // Scale factor: canvas pixels per degree
        const scaleX = (this.canvas.width / 2) / screenHalfX;
        const scaleY = (this.canvas.height / 2) / screenHalfY;
        
        return {
            x: cx + degX * scaleX,
            y: cy - degY * scaleY  // Flip Y (screen coords are top-down)
        };
    }
    
    /**
     * Convert screen pixels to canvas coordinates
     */
    screenToCanvas(screenX, screenY) {
        const { screenW, screenH } = this.state;
        
        const scaleX = this.canvas.width / screenW;
        const scaleY = this.canvas.height / screenH;
        
        return {
            x: screenX * scaleX,
            y: screenY * scaleY
        };
    }
    
    draw() {
        const { ctx, canvas } = this;
        const { width, height } = canvas;
        
        // Clear
        ctx.fillStyle = this.options.backgroundColor;
        ctx.fillRect(0, 0, width, height);
        
        // Draw grid
        this.drawGrid();
        
        // Draw eye regions
        this.drawEyeRegions();
        
        // Draw touch regions
        this.drawTouchRegions();
        
        // Draw eye trail
        this.drawEyeTrail();
        
        // Draw eye position
        this.drawEyePosition();
        
        // Draw touch position
        if (this.state.showTouch) {
            this.drawTouchPosition();
        }
        
        // Draw crosshair at center
        this.drawCrosshair();
    }
    
    drawGrid() {
        const { ctx, canvas, options } = this;
        const { screenHalfX, screenHalfY } = this.state;
        
        ctx.strokeStyle = options.gridColor;
        ctx.lineWidth = 1;
        
        // Draw degree lines
        for (let deg = -Math.floor(screenHalfX); deg <= Math.floor(screenHalfX); deg += 5) {
            const { x } = this.degToCanvas(deg, 0);
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, canvas.height);
            ctx.stroke();
        }
        
        for (let deg = -Math.floor(screenHalfY); deg <= Math.floor(screenHalfY); deg += 5) {
            const { y } = this.degToCanvas(0, deg);
            ctx.beginPath();
            ctx.moveTo(0, y);
            ctx.lineTo(canvas.width, y);
            ctx.stroke();
        }
    }
    
    drawCrosshair() {
        const { ctx, canvas, options } = this;
        const cx = canvas.width / 2;
        const cy = canvas.height / 2;
        const size = 10;
        
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.3)';
        ctx.lineWidth = 1;
        
        ctx.beginPath();
        ctx.moveTo(cx - size, cy);
        ctx.lineTo(cx + size, cy);
        ctx.moveTo(cx, cy - size);
        ctx.lineTo(cx, cy + size);
        ctx.stroke();
    }
    
    drawEyeRegions() {
        const { ctx, options } = this;
        
        this.state.eyeRegions.forEach((region, i) => {
            if (!region || !region.active) return;
            
            const center = this.degToCanvas(region.cx, region.cy);
            const halfW = region.pmx * (this.canvas.width / 2) / this.state.screenHalfX;
            const halfH = region.pmy * (this.canvas.height / 2) / this.state.screenHalfY;
            
            // Choose color based on state
            if (region.inRegion) {
                ctx.fillStyle = options.regionInColor;
                ctx.strokeStyle = options.eyeColor;
            } else {
                ctx.fillStyle = region.state ? options.regionActiveColor : options.regionInactiveColor;
                ctx.strokeStyle = region.state ? 'rgba(74, 222, 128, 0.5)' : 'rgba(100, 100, 120, 0.4)';
            }
            
            ctx.lineWidth = 2;
            
            if (region.type === 0) {
                // Rectangle
                ctx.fillRect(center.x - halfW, center.y - halfH, halfW * 2, halfH * 2);
                ctx.strokeRect(center.x - halfW, center.y - halfH, halfW * 2, halfH * 2);
            } else {
                // Ellipse
                ctx.beginPath();
                ctx.ellipse(center.x, center.y, halfW, halfH, 0, 0, Math.PI * 2);
                ctx.fill();
                ctx.stroke();
            }
        });
    }
    
    drawTouchRegions() {
        const { ctx, options } = this;
        
        this.state.touchRegions.forEach((region, i) => {
            if (!region || !region.active) return;
            
            const center = this.screenToCanvas(region.cx, region.cy);
            const halfW = (region.w / 2) * (this.canvas.width / this.state.screenW);
            const halfH = (region.h / 2) * (this.canvas.height / this.state.screenH);
            
            ctx.fillStyle = region.state ? 'rgba(248, 113, 113, 0.2)' : 'rgba(100, 100, 120, 0.1)';
            ctx.strokeStyle = region.state ? 'rgba(248, 113, 113, 0.5)' : 'rgba(100, 100, 120, 0.3)';
            ctx.lineWidth = 1;
            
            ctx.fillRect(center.x - halfW, center.y - halfH, halfW * 2, halfH * 2);
            ctx.strokeRect(center.x - halfW, center.y - halfH, halfW * 2, halfH * 2);
        });
    }
    
    drawEyeTrail() {
        const { ctx, options } = this;
        const trail = this.state.eyeTrail;
        
        if (trail.length < 2) return;
        
        const now = Date.now();
        
        ctx.beginPath();
        trail.forEach((point, i) => {
            const pos = this.degToCanvas(point.x, point.y);
            const age = (now - point.time) / 500; // 0 to 1
            
            if (i === 0) {
                ctx.moveTo(pos.x, pos.y);
            } else {
                ctx.lineTo(pos.x, pos.y);
            }
        });
        
        ctx.strokeStyle = options.eyeTrailColor;
        ctx.lineWidth = 2;
        ctx.stroke();
    }
    
    drawEyePosition() {
        const { ctx, options } = this;
        const pos = this.degToCanvas(this.state.eyePos.x, this.state.eyePos.y);
        
        // Outer circle
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 8, 0, Math.PI * 2);
        ctx.fillStyle = options.eyeColor;
        ctx.fill();
        
        // Inner circle
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 4, 0, Math.PI * 2);
        ctx.fillStyle = '#000';
        ctx.fill();
    }
    
    drawTouchPosition() {
        const { ctx, options } = this;
        const pos = this.screenToCanvas(this.state.touchPos.x, this.state.touchPos.y);
        
        // Touch indicator (crosshair style)
        ctx.strokeStyle = options.touchColor;
        ctx.lineWidth = 2;
        
        const size = 12;
        ctx.beginPath();
        ctx.moveTo(pos.x - size, pos.y);
        ctx.lineTo(pos.x + size, pos.y);
        ctx.moveTo(pos.x, pos.y - size);
        ctx.lineTo(pos.x, pos.y + size);
        ctx.stroke();
        
        // Circle
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 6, 0, Math.PI * 2);
        ctx.fillStyle = options.touchColor;
        ctx.fill();
    }
    
    /**
     * Set points per degree scaling
     */
    setPointsPerDeg(h, v) {
        this.state.pointsPerDegH = h;
        this.state.pointsPerDegV = v;
    }
    
    dispose() {
        this.stopAnimation();
        this.resizeObserver.disconnect();
    }
}

// Export
if (typeof window !== 'undefined') {
    window.EyeTouchVisualizer = EyeTouchVisualizer;
}
