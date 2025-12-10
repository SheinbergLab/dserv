/**
 * GraphicsRenderer.js
 * Canvas-based graphics renderer for dserv graphics commands
 * 
 * Usage:
 *   const renderer = new GraphicsRenderer('canvas-id', options);
 *   renderer.renderCommands(commandsJSON);
 * 
 * Or with DatapointManager:
 *   const renderer = new GraphicsRenderer('canvas-id', { 
 *       datapointManager: dpManager,
 *       streamId: 'graphics/main'
 *   });
 */

class GraphicsRenderer {
    constructor(canvasId, options = {}) {
        // Get canvas element
        if (typeof canvasId === 'string') {
            this.canvas = document.getElementById(canvasId);
        } else {
            this.canvas = canvasId; // Already an element
        }
        
        if (!this.canvas) {
            throw new Error(`Canvas element "${canvasId}" not found`);
        }
        
        // Configuration
        this.options = {
            width: options.width || 640,
            height: options.height || 480,
            autoScale: options.autoScale !== false,
            backgroundColor: options.backgroundColor || '#ffffff',
            datapointManager: options.datapointManager || null,
            streamId: options.streamId || null,
            onStats: options.onStats || null, // Callback for render stats
            ...options
        };
        
        // Set canvas size
        this.canvas.width = this.options.width;
        this.canvas.height = this.options.height;
        
        // Get 2D context
        this.ctx = this.canvas.getContext('2d');
        
        // Graphics state
        this.currentColor = '#000000';
        this.currentPos = { x: 0, y: 0 };
        this.currentFont = 'Helvetica';
        this.currentFontSize = 10;
        this.currentJustification = 0; // 0=left, 1=center, 2=right
        this.currentOrientation = 0; // degrees
        this.windowBounds = null;
        this.scaleX = 1;
        this.scaleY = 1;
        this.currentBackgroundColor = this.options.backgroundColor;
        
        // Last data for redrawing
        this.lastCommandData = null;
        
        // Image cache (stores HTMLImageElement objects by ID)
        this.imageCache = new Map();
        
        // Stats
        this.stats = {
            lastRenderTime: null,
            commandCount: 0,
            textCommandCount: 0
        };
        
        // Initialize
        this.clear();
        
        // Auto-subscribe to datapoint if specified
        if (this.options.datapointManager && this.options.streamId) {
            this.subscribeToDatapoint();
        }
    }
    
    /**
     * Subscribe to datapoint for automatic updates
     */
    subscribeToDatapoint() {
        if (!this.options.datapointManager || !this.options.streamId) {
            console.warn('Cannot subscribe: missing datapointManager or streamId');
            return;
        }
        
        this.unsubscribe = this.options.datapointManager.subscribe(
            this.options.streamId,
            (data) => {
                this.handleDatapointUpdate(data);
            }
        );
        
        console.log(`GraphicsRenderer subscribed to: ${this.options.streamId}`);
    }
    
    /**
     * Handle datapoint update
     */
    handleDatapointUpdate(data) {
        try {
            let commandData;
            
            // Parse if string
            if (typeof data.value === 'string') {
                commandData = JSON.parse(data.value);
            } else if (typeof data.data === 'string') {
                commandData = JSON.parse(data.data);
            } else {
                commandData = data.value || data.data;
            }
            
            // Render commands
            if (commandData && commandData.commands) {
                this.renderCommands(commandData);
            }
        } catch (e) {
            console.error('Failed to process graphics data:', e);
        }
    }
    
    /**
     * Render a set of graphics commands
     */
    renderCommands(commandData) {
        if (!commandData || !commandData.commands) {
            console.warn('No commands to render');
            return;
        }
        
        this.lastCommandData = commandData;
        
        // Clear canvas with background color
        this.ctx.fillStyle = this.currentBackgroundColor;
        this.ctx.fillRect(0, 0, this.options.width, this.options.height);
        
        // Reset transform and state
        this.ctx.setTransform(1, 0, 0, 1, 0, 0);
        this.resetGraphicsState();
        
        // Clear image cache to ensure fresh images
        // (images with same ID but different data should reload)
        this.imageCache.clear();
        
        // Execute commands
        const commands = commandData.commands;
        let commandCount = 0;
        let textCommandCount = 0;
        
        for (const command of commands) {
            if (command.cmd && command.args) {
                if (command.cmd === 'drawtext') {
                    textCommandCount++;
                }
                this.executeCommand(command);
                commandCount++;
            }
        }
        
        // Update stats
        this.stats.lastRenderTime = new Date();
        this.stats.commandCount = commandCount;
        this.stats.textCommandCount = textCommandCount;
        
        // Callback
        if (this.options.onStats) {
            this.options.onStats({
                time: this.stats.lastRenderTime.toLocaleTimeString(),
                commandCount: commandCount,
                textCommandCount: textCommandCount
            });
        }
    }
    
    /**
     * Reset graphics state
     */
    resetGraphicsState() {
        this.ctx.strokeStyle = '#000000';
        this.ctx.fillStyle = '#000000';
        this.ctx.lineWidth = 1;
        this.ctx.font = '10px Helvetica';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        
        this.currentColor = '#000000';
        this.currentPos = { x: 0, y: 0 };
        this.currentFont = 'Helvetica';
        this.currentFontSize = 10;
        this.currentJustification = 0;
        this.currentOrientation = 0;
        this.windowBounds = null;
        this.scaleX = 1;
        this.scaleY = 1;
    }
    
    /**
     * Execute a single graphics command
     */
    executeCommand(commandObj) {
        const { cmd, args } = commandObj;
        
        try {
            switch (cmd) {
                case 'setwindow':
                    this.cmdSetWindow(args);
                    break;
                    
                case 'setcolor':
                    this.cmdSetColor(args);
                    break;
                    
                case 'setbackground':
                    this.cmdSetBackground(args);
                    break;
                    
                case 'setjust':
                    this.cmdSetJustification(args);
                    break;
                    
                case 'setorientation':
                    this.cmdSetOrientation(args);
                    break;
                    
                case 'setfont':
                    this.cmdSetFont(args);
                    break;
                    
                case 'setlwidth':
                    this.cmdSetLineWidth(args);
                    break;
                    
                case 'gsave':
                    this.ctx.save();
                    break;
                    
                case 'grestore':
                    this.ctx.restore();
                    this.ctx.strokeStyle = this.currentColor;
                    this.ctx.fillStyle = this.currentColor;
                    this.ctx.font = `${this.currentFontSize}px ${this.currentFont}`;
                    break;
                    
                case 'circle':
                case 'fcircle':
                    this.cmdCircle(args, cmd === 'fcircle');
                    break;
                    
                case 'line':
                    this.cmdLine(args);
                    break;
                    
                case 'moveto':
                    this.cmdMoveTo(args);
                    break;
                    
                case 'lineto':
                    this.cmdLineTo(args);
                    break;
                    
                case 'frect':
                case 'filledrect':
                    this.cmdFilledRect(args);
                    break;
                    
                case 'poly':
                case 'fpoly':
                    this.cmdPolygon(args, cmd === 'fpoly');
                    break;
                    
                case 'drawtext':
                    this.cmdDrawText(args);
                    break;
                    
                case 'drawimage':
                    this.cmdDrawImage(commandObj);  // Pass full object for image_data
                    break;
                    
                case 'setclipregion':
                    // Clipping disabled for stability
                    break;
                    
                default:
                    // Silently ignore unknown commands
                    break;
            }
        } catch (error) {
            console.error('Error executing graphics command:', cmd, args, error);
        }
    }
    
    // ============================================
    // Command implementations
    // ============================================
    
    cmdSetWindow(args) {
        this.windowBounds = {
            llx: args[0], lly: args[1],
            urx: args[2], ury: args[3]
        };
        
        if (this.options.autoScale && this.windowBounds) {
            const sourceWidth = this.windowBounds.urx - this.windowBounds.llx;
            const sourceHeight = this.windowBounds.ury - this.windowBounds.lly;
            this.scaleX = this.options.width / sourceWidth;
            this.scaleY = this.options.height / sourceHeight;
        }
    }
    
    cmdSetColor(args) {
        this.currentColor = this.colorToHex(args[0]);
        this.ctx.strokeStyle = this.currentColor;
        this.ctx.fillStyle = this.currentColor;
    }
    
    cmdSetBackground(args) {
        const bgColor = this.colorToHex(args[0]);
        this.currentBackgroundColor = bgColor;
        this.ctx.fillStyle = bgColor;
        this.ctx.fillRect(0, 0, this.options.width, this.options.height);
        // Restore current drawing color
        this.ctx.fillStyle = this.currentColor;
    }
    
    cmdSetJustification(args) {
        const justValue = args[0];
        if (justValue === -1) {
            this.currentJustification = 0; // left
        } else if (justValue === 0) {
            this.currentJustification = 1; // center
        } else if (justValue === 1) {
            this.currentJustification = 2; // right
        } else {
            this.currentJustification = 1;
        }
    }
    
    cmdSetOrientation(args) {
        const orientValue = args[0];
        const orientationMap = { 0: 0, 1: 270, 2: 180, 3: 90 };
        this.currentOrientation = orientationMap[orientValue] !== undefined 
            ? orientationMap[orientValue] 
            : 0;
    }
    
    cmdSetFont(args) {
        this.currentFont = args[0] || 'Helvetica';
        this.currentFontSize = (args[1] || 10) * Math.min(this.scaleX, this.scaleY);
        this.ctx.font = `${this.currentFontSize}px ${this.currentFont}`;
    }
    
    cmdSetLineWidth(args) {
        this.ctx.lineWidth = Math.max(1, (args[0] / 100) * Math.min(this.scaleX, this.scaleY));
    }
    
    cmdCircle(args, filled) {
        this.ctx.beginPath();
        this.ctx.arc(
            this.transformX(args[0]),
            this.transformY(args[1]),
            this.transformWidth(args[2]),
            0, 2 * Math.PI
        );
        
        const shouldFill = filled || args[3];
        if (shouldFill) {
            this.ctx.fill();
        } else {
            this.ctx.stroke();
        }
    }
    
    cmdLine(args) {
        this.ctx.beginPath();
        this.ctx.moveTo(this.transformX(args[0]), this.transformY(args[1]));
        this.ctx.lineTo(this.transformX(args[2]), this.transformY(args[3]));
        this.ctx.stroke();
    }
    
    cmdMoveTo(args) {
        this.currentPos = {
            x: this.transformX(args[0]),
            y: this.transformY(args[1])
        };
    }
    
    cmdLineTo(args) {
        this.ctx.beginPath();
        this.ctx.moveTo(this.currentPos.x, this.currentPos.y);
        const newX = this.transformX(args[0]);
        const newY = this.transformY(args[1]);
        this.ctx.lineTo(newX, newY);
        this.ctx.stroke();
        this.currentPos = { x: newX, y: newY };
    }
    
    cmdFilledRect(args) {
        const x1 = this.transformX(args[0]);
        const y1 = this.transformY(args[1]);
        const x2 = this.transformX(args[2]);
        const y2 = this.transformY(args[3]);
        const width = Math.abs(x2 - x1);
        const height = Math.abs(y2 - y1);
        this.ctx.fillRect(
            Math.min(x1, x2),
            Math.min(y1, y2),
            width,
            height
        );
    }
    
    cmdPolygon(args, filled) {
        if (args.length >= 6 && args.length % 2 === 0) {
            this.ctx.beginPath();
            
            // Move to first point
            this.ctx.moveTo(this.transformX(args[0]), this.transformY(args[1]));
            
            // Draw lines to subsequent points
            for (let i = 2; i < args.length; i += 2) {
                this.ctx.lineTo(this.transformX(args[i]), this.transformY(args[i + 1]));
            }
            
            // Close the polygon
            this.ctx.closePath();
            
            // Fill or stroke
            if (filled) {
                this.ctx.fill();
            } else {
                this.ctx.stroke();
            }
        }
    }
    
    cmdDrawText(args) {
        if (this.currentPos && this.currentPos.x !== undefined && args[0]) {
            const text = String(args[0]);
            
            this.ctx.save();
            this.ctx.font = `${this.currentFontSize}px ${this.currentFont}`;
            this.ctx.fillStyle = this.currentColor;
            this.ctx.textAlign = ['left', 'center', 'right'][this.currentJustification] || 'left';
            this.ctx.textBaseline = 'middle';
            
            const x = this.currentPos.x;
            const y = this.currentPos.y;
            
            if (this.currentOrientation !== 0) {
                this.ctx.translate(x, y);
                this.ctx.rotate((this.currentOrientation * Math.PI) / 180);
                this.ctx.fillText(text, 0, 0);
            } else {
                this.ctx.fillText(text, x, y);
            }
            
            this.ctx.restore();
        }
    }
    
    cmdDrawImage(commandObj) {
        // Command format: { cmd: 'drawimage', args: [x0, y0, x1, y1, image_id], image_data: {...} }
        const args = commandObj.args;
        const imageData = commandObj.image_data;
        
        if (!args || args.length < 5) {
            console.warn('drawimage: insufficient args');
            return;
        }
        
        const x0 = this.transformX(args[0]);
        const y0 = this.transformY(args[1]);
        const x1 = this.transformX(args[2]);
        const y1 = this.transformY(args[3]);
        const imageId = String(args[4]);
        
        // Calculate destination rectangle (handle flipped Y coords)
        const left = Math.min(x0, x1);
        const top = Math.min(y0, y1);
        const width = Math.abs(x1 - x0);
        const height = Math.abs(y1 - y0);
        
        // If image_data provided, cache it
        if (imageData && imageData.width && imageData.height && imageData.data) {
            const imgWidth = parseInt(imageData.width);
            const imgHeight = parseInt(imageData.height);
            const depth = parseInt(imageData.depth) || 3; // Default to RGB
            const base64Data = imageData.data;
            
            // Cache image if not already cached
            const cacheKey = `${imageId}`;
            if (!this.imageCache.has(cacheKey)) {
                this.cacheImage(cacheKey, imgWidth, imgHeight, depth, base64Data);
            }
        }
        
        // Draw the image if cached
        const cacheKey = `${imageId}`;
        const img = this.imageCache.get(cacheKey);
        if (img && img.complete) {
            this.ctx.drawImage(img, left, top, width, height);
        } else if (img) {
            // Image still loading, draw when ready
            img.onload = () => {
                this.ctx.drawImage(img, left, top, width, height);
            };
        } else {
            console.warn(`Image not found in cache: ${imageId}`);
        }
    }
    
    /**
     * Cache an image from base64 data
     * @param {string} imageId - Unique identifier for the image
     * @param {number} width - Image width
     * @param {number} height - Image height
     * @param {number} depth - 1 (grayscale), 3 (RGB), or 4 (RGBA)
     * @param {string} base64Data - Base64-encoded image data
     */
    cacheImage(imageId, width, height, depth, base64Data) {
        // Create image element
        const img = new Image();
        
        // Create temp canvas to build image
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d');
        
        // Decode base64 to binary
        const binaryString = atob(base64Data);
        const bytes = new Uint8Array(binaryString.length);
        for (let i = 0; i < binaryString.length; i++) {
            bytes[i] = binaryString.charCodeAt(i);
        }
        
        // Create ImageData
        const imageData = ctx.createImageData(width, height);
        
        // Fill based on depth
        if (depth === 1) {
            // Grayscale: 1 byte per pixel
            for (let i = 0; i < width * height; i++) {
                const gray = bytes[i] || 0;
                imageData.data[i * 4] = gray;     // R
                imageData.data[i * 4 + 1] = gray; // G
                imageData.data[i * 4 + 2] = gray; // B
                imageData.data[i * 4 + 3] = 255;  // A (opaque)
            }
        } else if (depth === 3) {
            // RGB: 3 bytes per pixel
            for (let i = 0; i < width * height; i++) {
                const srcIdx = i * 3;
                const dstIdx = i * 4;
                imageData.data[dstIdx] = bytes[srcIdx] || 0;         // R
                imageData.data[dstIdx + 1] = bytes[srcIdx + 1] || 0; // G
                imageData.data[dstIdx + 2] = bytes[srcIdx + 2] || 0; // B
                imageData.data[dstIdx + 3] = 255;                    // A (opaque)
            }
        } else if (depth === 4) {
            // RGBA: 4 bytes per pixel (direct copy)
            for (let i = 0; i < width * height; i++) {
                const srcIdx = i * 4;
                const dstIdx = i * 4;
                imageData.data[dstIdx] = bytes[srcIdx] || 0;         // R
                imageData.data[dstIdx + 1] = bytes[srcIdx + 1] || 0; // G
                imageData.data[dstIdx + 2] = bytes[srcIdx + 2] || 0; // B
                imageData.data[dstIdx + 3] = bytes[srcIdx + 3] || 255; // A
            }
        } else {
            console.warn(`Unsupported image depth: ${depth}, treating as grayscale`);
            // Fallback to grayscale
            for (let i = 0; i < width * height; i++) {
                const gray = bytes[i] || 0;
                imageData.data[i * 4] = gray;
                imageData.data[i * 4 + 1] = gray;
                imageData.data[i * 4 + 2] = gray;
                imageData.data[i * 4 + 3] = 255;
            }
        }
        
        // Put image data on temp canvas
        ctx.putImageData(imageData, 0, 0);
        
        // Convert canvas to data URL and set as image source
        img.src = canvas.toDataURL();
        
        // Cache the image
        this.imageCache.set(imageId, img);
    }
    
    // ============================================
    // Coordinate transformations
    // ============================================
    
    transformX(x) {
        return x * this.scaleX;
    }
    
    transformY(y) {
        return this.options.height - (y * this.scaleY);
    }
    
    transformWidth(w) {
        return w * this.scaleX;
    }
    
    transformHeight(h) {
        return h * this.scaleY;
    }
    
    // ============================================
    // Color conversion
    // ============================================
    
    colorToHex(colorIndex) {
        // Standard palette colors (0-14)
        const colors = [
            '#000000', '#0000FF', '#008000', '#00FFFF', '#FF0000',
            '#FF00FF', '#A52A2A', '#FFFFFF', '#808080', '#ADD8E6',
            '#00FF00', '#E0FFFF', '#FF1493', '#9370DB', '#FFFF00'
        ];
        
        // Standard palette
        if (colorIndex >= 0 && colorIndex < colors.length) {
            return colors[colorIndex];
        }
        
        // Packed RGB colors (special colors > 18)
        // Format: (r << 21) + (g << 13) + (b << 5)
        if (colorIndex > 18) {
            const r = (colorIndex >> 21) & 0xFF;
            const g = (colorIndex >> 13) & 0xFF;
            const b = (colorIndex >> 5) & 0xFF;
            
            const rHex = r.toString(16).padStart(2, '0');
            const gHex = g.toString(16).padStart(2, '0');
            const bHex = b.toString(16).padStart(2, '0');
            
            return `#${rHex}${gHex}${bHex}`;
        }
        
        // Default to black
        return '#000000';
    }
    
    // ============================================
    // Utility methods
    // ============================================
    
    /**
     * Clear canvas to background color
     */
    clear() {
        this.ctx.fillStyle = this.currentBackgroundColor;
        this.ctx.fillRect(0, 0, this.options.width, this.options.height);
    }
    
    /**
     * Resize canvas
     */
    resize(width, height) {
        const oldWidth = this.options.width;
        const oldHeight = this.options.height;
        
        this.options.width = width;
        this.options.height = height;
        this.canvas.width = width;
        this.canvas.height = height;
        
        // Get fresh context
        this.ctx = this.canvas.getContext('2d');
        
        // Recalculate scaling if we have window bounds
        if (this.windowBounds && this.options.autoScale) {
            const sourceWidth = this.windowBounds.urx - this.windowBounds.llx;
            const sourceHeight = this.windowBounds.ury - this.windowBounds.lly;
            this.scaleX = width / sourceWidth;
            this.scaleY = height / sourceHeight;
        }
        
        // Redraw last data
        if (this.lastCommandData) {
            this.renderCommands(this.lastCommandData);
        } else {
            this.clear();
        }
        
        console.log(`Canvas resized from ${oldWidth}×${oldHeight} to ${width}×${height}`);
    }
    
    /**
     * Get current scale factors
     */
    getScale() {
        return { x: this.scaleX, y: this.scaleY };
    }
    
    /**
     * Get source dimensions (from setwindow)
     */
    getSourceDimensions() {
        if (this.windowBounds) {
            return {
                width: this.windowBounds.urx - this.windowBounds.llx,
                height: this.windowBounds.ury - this.windowBounds.lly
            };
        }
        return null;
    }
    
    /**
     * Get render statistics
     */
    getStats() {
        return { ...this.stats };
    }
    
    /**
     * Cleanup and dispose
     */
    dispose() {
        if (this.unsubscribe) {
            this.unsubscribe();
            this.unsubscribe = null;
        }
        
        // Clear image cache
        this.imageCache.clear();
        
        this.clear();
        console.log('GraphicsRenderer disposed');
    }
}

// Export for use
if (typeof window !== 'undefined') {
    window.GraphicsRenderer = GraphicsRenderer;
}
