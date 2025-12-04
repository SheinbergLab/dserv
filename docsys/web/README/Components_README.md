# dserv Web Components Library

A collection of reusable vanilla JavaScript components for building interactive dserv web applications.

## Components Overview

### 1. DGTableViewer
**Display dynamic group data with nested arrays**

```javascript
const viewer = new DGTableViewer('container', data, {
    pageSize: 50,
    showExport: true
});
viewer.render();
```

**Features:**
- Hybrid JSON format support (row + columnar)
- Expandable nested arrays (1D and 2D)
- Row inspector side panel
- Sorting, pagination, CSV export
- Click rows to inspect details

**Files:** `DGTableViewer.js`, `DGTableViewer.css`

---

### 2. TclTerminal
**Interactive Tcl command terminal**

```javascript
const terminal = new TclTerminal('terminal', connection, {
    interpreter: 'dserv',
    useLinkedSubprocess: true,
    tabCompletion: true
});
```

**Features:**
- Command history (localStorage)
- Tab completion (context-aware)
- Keyboard shortcuts (↑/↓, Ctrl+C, Ctrl+L)
- Error stack traces (expandable)
- Dark/light themes
- Linked subprocess support

**Files:** `TclTerminal.js`, `TclTerminal.css`

---

### 3. DatapointManager
**Pub/sub system for datapoint subscriptions**

```javascript
const dpManager = new DatapointManager(wsManager);

dpManager.subscribe('sensor/temperature', (data) => {
    console.log('Temp:', data.value);
});
```

**Features:**
- Pattern matching (`sensor/*`)
- Auto-reconnect (resubscribes)
- History tracking (last 100 updates)
- Event system
- Statistics tracking
- Wildcard support

**Files:** `DatapointManager.js`

---

### 4. GraphicsRenderer
**Canvas-based 2D graphics from JSON commands**

```javascript
const renderer = new GraphicsRenderer('canvas', {
    datapointManager: dpManager,
    streamId: 'graphics/main',
    width: 640,
    height: 480
});
```

**Features:**
- Complete graphics API (lines, circles, polygons, text)
- Auto-scaling coordinates
- Datapoint integration (auto-updates)
- Color palette + packed RGB
- Text rendering (fonts, rotation, justification)
- Resizable with redraw

**Files:** `GraphicsRenderer.js`

---

## Quick Start

### 1. Include Components

```html
<!-- Core components -->
<script src="DatapointManager.js"></script>
<script src="GraphicsRenderer.js"></script>
<script src="TclTerminal.js"></script>
<script src="DGTableViewer.js"></script>

<!-- Styles -->
<link rel="stylesheet" href="TclTerminal.css">
<link rel="stylesheet" href="DGTableViewer.css">
```

### 2. Setup WebSocket Connection

```javascript
// Your WebSocket manager
const wsManager = new DservWebSocket();
wsManager.connect();
```

### 3. Use Components

```javascript
// Datapoint subscriptions
const dpManager = new DatapointManager(wsManager);

// Graphics display
const renderer = new GraphicsRenderer('canvas', {
    datapointManager: dpManager,
    streamId: 'graphics/main'
});

// Tcl terminal
const terminal = new TclTerminal('terminal', wsManager, {
    useLinkedSubprocess: true
});

// Table viewer
const viewer = new DGTableViewer('table', data);
viewer.render();
```

## Architecture

```
┌─────────────────────────────────────────────┐
│           Your Web Application              │
├─────────────────────────────────────────────┤
│                                             │
│  ┌──────────────┐  ┌──────────────┐       │
│  │ TclTerminal  │  │ DGTableViewer│       │
│  └──────────────┘  └──────────────┘       │
│                                             │
│  ┌──────────────┐  ┌──────────────┐       │
│  │ Graphics     │  │ Datapoint    │       │
│  │ Renderer     │──│ Manager      │       │
│  └──────────────┘  └──────────────┘       │
│                           │                 │
├───────────────────────────┼─────────────────┤
│                           │                 │
│        ┌──────────────────┴────────┐       │
│        │  WebSocket Connection     │       │
│        │  (DservWebSocket)         │       │
│        └───────────────────────────┘       │
│                    │                        │
└────────────────────┼────────────────────────┘
                     │
              ┌──────▼──────┐
              │   dserv     │
              │   Backend   │
              └─────────────┘
```

## Component Interactions

### Example 1: Graphics Workflow

```javascript
// 1. Create datapoint manager
const dpManager = new DatapointManager(wsManager);

// 2. Create graphics renderer with auto-subscription
const renderer = new GraphicsRenderer('canvas', {
    datapointManager: dpManager,
    streamId: 'graphics/demo'
});

// 3. Create terminal for command execution
const terminal = new TclTerminal('terminal', wsManager);

// Backend publishes → Datapoint updates → Canvas redraws
```

### Example 2: Data Exploration

```javascript
// 1. Get data via datapoint
dpManager.subscribe('experiment/results', (data) => {
    // 2. Display in table
    const viewer = new DGTableViewer('table', data.value);
    viewer.render();
});
```

### Example 3: Interactive Session

```javascript
// Terminal sends commands
terminal → wsManager → dserv

// dserv publishes results
dserv → datapoint → dpManager → renderer
```

## File Organization

```
components/
├── DGTableViewer.js        # Table viewer component
├── DGTableViewer.css       # Table styles
├── TclTerminal.js          # Terminal component
├── TclTerminal.css         # Terminal styles
├── DatapointManager.js     # Datapoint subscriptions
├── GraphicsRenderer.js     # Graphics renderer
└── demos/
    ├── GraphicsDemo.html   # Graphics demo
    ├── DGTableViewer_dserv_example.html
    └── TclTerminal_demo.html
```

## Component Features Matrix

| Feature | DGTableViewer | TclTerminal | DatapointManager | GraphicsRenderer |
|---------|---------------|-------------|------------------|------------------|
| Vanilla JS | ✅ | ✅ | ✅ | ✅ |
| Zero deps | ✅ | ✅ | ✅ | ✅ |
| Reusable | ✅ | ✅ | ✅ | ✅ |
| Themeable | ✅ | ✅ | ➖ | ➖ |
| Responsive | ✅ | ✅ | N/A | ✅ |
| WebSocket | ➖ | ✅ | ✅ | ✅ |
| Datapoints | ➖ | ➖ | ✅ | ✅ |

## Usage Patterns

### Pattern 1: Standalone Components

Use components independently without interaction:

```javascript
// Just a table
const viewer = new DGTableViewer('table', data);

// Just a terminal
const terminal = new TclTerminal('term', wsManager);
```

### Pattern 2: Connected Components

Connect components through datapoints:

```javascript
const dpManager = new DatapointManager(wsManager);

// Both subscribe to same datapoint
dpManager.subscribe('experiment/data', (data) => {
    viewer.render(data.value);
});

const renderer = new GraphicsRenderer('canvas', {
    datapointManager: dpManager,
    streamId: 'experiment/graphics'
});
```

### Pattern 3: Full Application

Build complete interactive application:

```javascript
// Setup infrastructure
const wsManager = new DservWebSocket();
const dpManager = new DatapointManager(wsManager);

// Create UI components
const terminal = new TclTerminal('terminal', wsManager);
const renderer = new GraphicsRenderer('canvas', {
    datapointManager: dpManager,
    streamId: 'graphics/main'
});
const viewer = new DGTableViewer('table', null);

// Connect them
dpManager.subscribe('results/table', (data) => {
    viewer.render(data.value);
});

wsManager.connect();
```

## Best Practices

### 1. Cleanup

```javascript
// Always cleanup on page unload
window.addEventListener('beforeunload', () => {
    dpManager.dispose();
    renderer.dispose();
    terminal.dispose();
});
```

### 2. Error Handling

```javascript
try {
    const viewer = new DGTableViewer('table', data);
    viewer.render();
} catch (e) {
    console.error('Failed to create viewer:', e);
}
```

### 3. Component Communication

```javascript
// Use datapoints for inter-component communication
dpManager.subscribe('control/command', (data) => {
    if (data.value === 'clear') {
        renderer.clear();
        viewer.clear();
    }
});
```

### 4. Progressive Enhancement

```javascript
// Check if components exist before using
if (typeof DatapointManager !== 'undefined') {
    const dpManager = new DatapointManager(wsManager);
} else {
    console.warn('DatapointManager not loaded');
}
```

## Browser Compatibility

All components work in modern browsers:
- Chrome/Edge 90+
- Firefox 88+
- Safari 14+

## Performance Tips

1. **Batch updates** - Don't render on every datapoint update
2. **Debounce** - Use debouncing for high-frequency updates
3. **Pagination** - Use pagination in tables for large datasets
4. **Canvas size** - Keep canvas dimensions reasonable (<2000x2000)
5. **Cleanup** - Always dispose components when done

## Common Pitfalls

❌ **Forgetting to call render()** on DGTableViewer
```javascript
const viewer = new DGTableViewer('table', data);
// Missing: viewer.render();
```

❌ **Not checking connection before using terminal**
```javascript
const terminal = new TclTerminal('term', wsManager);
// Should check: wsManager.connected
```

❌ **Memory leaks from subscriptions**
```javascript
dpManager.subscribe('data', callback);
// Missing: cleanup on component unmount
```

✅ **Proper usage**
```javascript
const viewer = new DGTableViewer('table', data);
viewer.render();

const unsubscribe = dpManager.subscribe('data', callback);
// Later: unsubscribe();
```

## Next Steps

1. **Read component READMEs** - Each has detailed documentation
2. **Try demos** - Run the demo HTML files
3. **Build your app** - Combine components as needed
4. **Customize** - Modify CSS and options
5. **Share** - These are all reusable across projects!

## Resources

- `DGTableViewer_README.md` - Table viewer docs
- `TclTerminal_README.md` - Terminal docs
- `DatapointManager_README.md` - Datapoint docs
- `GraphicsRenderer_README.md` - Graphics docs
- `GraphicsDemo.html` - Complete example

## Summary

You now have **four powerful, reusable components**:

1. **DGTableViewer** - Display and explore tabular data
2. **TclTerminal** - Interactive Tcl command execution
3. **DatapointManager** - Subscribe to real-time data streams
4. **GraphicsRenderer** - Render 2D graphics from commands

All are:
- ✅ Vanilla JavaScript (no framework required)
- ✅ Zero dependencies
- ✅ Well-documented
- ✅ Production-ready
- ✅ Easily customizable

**Build amazing dserv web applications!** 🚀
