# GraphicsRenderer Component

A canvas-based graphics renderer for dserv graphics commands. Renders 2D graphics from JSON command streams with automatic scaling and datapoint integration.

## Features

✅ **Complete Graphics API** - All dserv graphics commands supported
✅ **Auto-scaling** - Automatically scales source coordinates to canvas
✅ **Datapoint Integration** - Auto-subscribe to graphics streams
✅ **Color Palette** - Standard colors plus packed RGB support
✅ **Text Rendering** - Fonts, sizes, justification, rotation
✅ **Resizable** - Dynamically resize canvas and redraw
✅ **No Dependencies** - Pure vanilla JavaScript

## Files

- `GraphicsRenderer.js` - Main component (~650 lines)
- `GraphicsDemo.html` - Complete working demo

## Quick Start

### Basic Usage (Manual)

```javascript
// Create renderer
const renderer = new GraphicsRenderer('my-canvas', {
    width: 640,
    height: 480
});

// Render graphics commands
const commands = {
    commands: [
        { cmd: 'setwindow', args: [0, 0, 640, 480] },
        { cmd: 'setcolor', args: [1] },  // Blue
        { cmd: 'fcircle', args: [320, 240, 50, 1] }
    ]
};

renderer.renderCommands(commands);
```

### With DatapointManager (Auto-updates)

```javascript
// Create datapoint manager
const dpManager = new DatapointManager(wsManager);

// Create renderer with auto-subscription
const renderer = new GraphicsRenderer('my-canvas', {
    datapointManager: dpManager,
    streamId: 'graphics/main',  // Auto-subscribes
    width: 640,
    height: 480
});

// Canvas updates automatically when datapoint changes!
```

## Constructor Options

```javascript
new GraphicsRenderer(canvasId, {
    width: 640,                    // Canvas width in pixels
    height: 480,                   // Canvas height in pixels
    autoScale: true,               // Auto-scale source coords
    backgroundColor: '#ffffff',    // Background color
    datapointManager: null,        // DatapointManager instance
    streamId: null,                // Datapoint to subscribe to
    onStats: null                  // Callback for render stats
})
```

## Supported Graphics Commands

### Window & State
- `setwindow` - Set coordinate system
- `gsave` / `grestore` - Save/restore graphics state
- `setclipregion` - Set clipping (disabled)

### Colors & Styles
- `setcolor` - Set drawing color (0-14 or packed RGB)
- `setbackground` - Set background color
- `setlwidth` - Set line width

### Drawing Primitives
- `line` - Draw line between two points
- `moveto` / `lineto` - Move/draw to point
- `circle` / `fcircle` - Circle (outline/filled)
- `frect` / `filledrect` - Filled rectangle
- `poly` / `fpoly` - Polygon (outline/filled)

### Text
- `setfont` - Set font family and size
- `setjust` - Set text justification (-1=left, 0=center, 1=right)
- `setorientation` - Set text rotation (0=0°, 1=270°, 2=180°, 3=90°)
- `drawtext` - Draw text at current position

## Graphics Command Format

Commands are sent as JSON with this structure:

```javascript
{
    "commands": [
        { "cmd": "setwindow", "args": [0, 0, 640, 480] },
        { "cmd": "setcolor", "args": [4] },
        { "cmd": "frect", "args": [100, 100, 200, 200] }
    ]
}
```

## Color Palette

### Standard Colors (0-14)
| Index | Color | Hex |
|-------|-------|-----|
| 0 | Black | #000000 |
| 1 | Blue | #0000FF |
| 2 | Green | #008000 |
| 3 | Cyan | #00FFFF |
| 4 | Red | #FF0000 |
| 5 | Magenta | #FF00FF |
| 6 | Brown | #A52A2A |
| 7 | White | #FFFFFF |
| 8 | Gray | #808080 |
| 9 | Light Blue | #ADD8E6 |
| 10 | Lime | #00FF00 |
| 11 | Light Cyan | #E0FFFF |
| 12 | Deep Pink | #FF1493 |
| 13 | Medium Purple | #9370DB |
| 14 | Yellow | #FFFF00 |

### Packed RGB (Index > 18)
Format: `(r << 21) + (g << 13) + (b << 5)`

```tcl
# Example in Tcl:
set color [expr {($r << 21) + ($g << 13) + ($b << 5)}]
gbuf_cmd $gbuf setcolor $color
```

## Methods

### renderCommands(commandData)

Render a set of graphics commands.

```javascript
renderer.renderCommands({
    commands: [
        { cmd: 'setcolor', args: [1] },
        { cmd: 'fcircle', args: [320, 240, 100, 1] }
    ]
});
```

### clear()

Clear canvas to background color.

```javascript
renderer.clear();
```

### resize(width, height)

Resize canvas and redraw.

```javascript
renderer.resize(800, 600);
```

### getScale()

Get current scale factors.

```javascript
const { x, y } = renderer.getScale();
console.log(`Scale: ${x}x, ${y}y`);
```

### getSourceDimensions()

Get source dimensions from `setwindow` command.

```javascript
const dims = renderer.getSourceDimensions();
if (dims) {
    console.log(`Source: ${dims.width}×${dims.height}`);
}
```

### getStats()

Get render statistics.

```javascript
const stats = renderer.getStats();
console.log('Last render:', stats.lastRenderTime);
console.log('Commands:', stats.commandCount);
```

### dispose()

Cleanup and unsubscribe.

```javascript
renderer.dispose();
```

## Usage Examples

### Example 1: Simple Drawing

```javascript
const renderer = new GraphicsRenderer('canvas');

renderer.renderCommands({
    commands: [
        { cmd: 'setwindow', args: [0, 0, 640, 480] },
        { cmd: 'setbackground', args: [7] },  // White
        { cmd: 'setcolor', args: [1] },       // Blue
        { cmd: 'fcircle', args: [320, 240, 50, 1] }
    ]
});
```

### Example 2: Text with Rotation

```javascript
renderer.renderCommands({
    commands: [
        { cmd: 'setwindow', args: [0, 0, 640, 480] },
        { cmd: 'setfont', args: ['Helvetica', 24] },
        { cmd: 'setcolor', args: [0] },       // Black
        { cmd: 'setorientation', args: [1] }, // 270° (vertical)
        { cmd: 'moveto', args: [100, 300] },
        { cmd: 'drawtext', args: ['Rotated Text'] }
    ]
});
```

### Example 3: Polygon

```javascript
renderer.renderCommands({
    commands: [
        { cmd: 'setwindow', args: [0, 0, 640, 480] },
        { cmd: 'setcolor', args: [4] },  // Red
        { cmd: 'fpoly', args: [
            320, 100,  // Point 1
            450, 350,  // Point 2
            190, 350   // Point 3
        ]}
    ]
});
```

### Example 4: With dserv Backend

**Backend (Tcl):**
```tcl
package require dlsh

# Create graphics buffer
set gbuf [gbuf_new]

# Set coordinates
gbuf_cmd $gbuf setwindow 0 0 640 480

# Draw circle
gbuf_cmd $gbuf setcolor 1
gbuf_cmd $gbuf fcircle 320 240 100 1

# Publish to datapoint
datapoint_set graphics/main [gbuf_toJSON $gbuf]
gbuf_delete $gbuf
```

**Frontend (JavaScript):**
```javascript
const dpManager = new DatapointManager(wsManager);

const renderer = new GraphicsRenderer('canvas', {
    datapointManager: dpManager,
    streamId: 'graphics/main'
});

// Canvas automatically updates when backend publishes!
```

### Example 5: Responsive Canvas

```javascript
const renderer = new GraphicsRenderer('canvas', {
    width: 640,
    height: 480,
    autoScale: true
});

// Resize on window resize
window.addEventListener('resize', () => {
    const container = document.getElementById('canvas-container');
    renderer.resize(container.clientWidth, container.clientHeight);
});
```

### Example 6: Animation Loop

```javascript
let frame = 0;

function animate() {
    renderer.renderCommands({
        commands: [
            { cmd: 'setwindow', args: [0, 0, 640, 480] },
            { cmd: 'setbackground', args: [7] },
            { cmd: 'setcolor', args: [1] },
            { cmd: 'fcircle', args: [
                320 + Math.cos(frame * 0.1) * 100,
                240 + Math.sin(frame * 0.1) * 100,
                30,
                1
            ]}
        ]
    });
    
    frame++;
    requestAnimationFrame(animate);
}

animate();
```

## Coordinate System

The renderer uses a bottom-left origin coordinate system (like OpenGL):
- X increases to the right
- Y increases upward
- Origin (0, 0) is bottom-left corner

Canvas coordinates are automatically transformed:
```javascript
canvasY = canvasHeight - sourceY
```

Auto-scaling is applied when `setwindow` is called:
```javascript
scaleX = canvasWidth / sourceWidth
scaleY = canvasHeight / sourceHeight
```

## Integration with dserv

### Creating Graphics in Tcl

```tcl
package require dlsh

proc draw_scene {} {
    set gbuf [gbuf_new]
    
    # Set window
    gbuf_cmd $gbuf setwindow 0 0 640 480
    
    # Background
    gbuf_cmd $gbuf setbackground 7
    
    # Draw shapes
    gbuf_cmd $gbuf setcolor 1
    gbuf_cmd $gbuf fcircle 100 100 50 1
    
    # Text
    gbuf_cmd $gbuf setcolor 0
    gbuf_cmd $gbuf setfont Helvetica 20
    gbuf_cmd $gbuf moveto 320 240
    gbuf_cmd $gbuf drawtext "Hello!"
    
    # Publish
    set json [gbuf_toJSON $gbuf]
    datapoint_set graphics/demo $json
    
    gbuf_delete $gbuf
}

draw_scene
```

### Subscribing in JavaScript

```javascript
const dpManager = new DatapointManager(wsManager);
const renderer = new GraphicsRenderer('canvas', {
    datapointManager: dpManager,
    streamId: 'graphics/demo',
    onStats: (stats) => {
        console.log(`Rendered ${stats.commandCount} commands`);
    }
});
```

## Performance

- Renders typical scenes (100-200 commands) at 60fps
- Supports thousands of drawing commands
- Canvas is only redrawn when new data arrives
- Auto-scaling computed once per `setwindow` command

## Browser Support

Works in all modern browsers:
- Chrome/Edge 90+
- Firefox 88+
- Safari 14+

## Tips

### Efficient Drawing

```javascript
// ✅ Good: Batch commands
const commands = [];
for (let i = 0; i < 100; i++) {
    commands.push({ cmd: 'fcircle', args: [i*10, 100, 5, 1] });
}
renderer.renderCommands({ commands });

// ❌ Avoid: Multiple render calls
for (let i = 0; i < 100; i++) {
    renderer.renderCommands({
        commands: [{ cmd: 'fcircle', args: [i*10, 100, 5, 1] }]
    });
}
```

### Debugging

```javascript
// Enable stats callback
const renderer = new GraphicsRenderer('canvas', {
    onStats: (stats) => {
        console.log(`Commands: ${stats.commandCount}, Time: ${stats.time}`);
    }
});

// Check scaling
console.log('Scale:', renderer.getScale());
console.log('Source:', renderer.getSourceDimensions());
```

## See Also

- `DatapointManager.js` - For datapoint subscriptions
- `TclTerminal.js` - For Tcl command execution
- `DGTableViewer.js` - For viewing dynamic group data
- `GraphicsDemo.html` - Complete working example

## License

Free to use in your dserv projects.
