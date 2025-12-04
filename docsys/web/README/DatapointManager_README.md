# DatapointManager Component

A lightweight, reusable datapoint subscription manager for dserv WebSocket connections.

## Features

✅ **Simple API** - Subscribe/unsubscribe with callbacks
✅ **Pattern Matching** - Subscribe to wildcards like `sensor/*`
✅ **Auto-reconnect** - Resubscribes on reconnection
✅ **History Tracking** - Keeps last 100 updates per datapoint
✅ **Event System** - Listen for keys updates, connection changes
✅ **Statistics** - Track subscription count, total updates
✅ **No Dependencies** - Pure vanilla JavaScript

## Files

- `DatapointManager.js` - Main component (~450 lines)

## Quick Start

### 1. Include the file

```html
<script src="DatapointManager.js"></script>
```

### 2. Create manager with your WebSocket connection

```javascript
// Assumes you have a DservWebSocket instance
const wsManager = new DservWebSocket();
wsManager.connect();

// Create datapoint manager
const dpManager = new DatapointManager(wsManager, {
    autoGetKeys: true,      // Auto-fetch list of all datapoints
    updateInterval: 1       // Update every N changes
});
```

### 3. Subscribe to datapoints

```javascript
// Simple subscription
dpManager.subscribe('graphics/main', (data) => {
    console.log('Graphics update:', data.value);
});

// With cleanup
const unsubscribe = dpManager.subscribe('sensor/temperature', (data) => {
    console.log('Temperature:', data.value);
});

// Later...
unsubscribe();
```

## API Reference

### Constructor

```javascript
new DatapointManager(connection, options)
```

**Parameters:**
- `connection` - WebSocket manager (must have `on()`, `send()` methods)
- `options` - Configuration object
  - `autoGetKeys` - Auto-fetch datapoint list (default: true)
  - `updateInterval` - Update frequency (default: 1)

**Required connection methods:**
- `connection.on(event, callback)` - Listen for events
- `connection.send(data)` - Send commands

**Required connection events:**
- `'datapoint:update'` - Fired with `{ name, data, timestamp, dtype }`
- `'datapoint:keys'` - Fired with @keys updates
- `'connected'` - Connection established
- `'disconnected'` - Connection lost

### Methods

#### subscribe(pattern, callback, options)

Subscribe to a datapoint pattern.

```javascript
const unsubscribe = dpManager.subscribe('graphics/main', (data) => {
    console.log('Name:', data.name);
    console.log('Value:', data.value);
    console.log('Timestamp:', data.timestamp);
    console.log('Type:', data.dtype);
});
```

**Pattern matching:**
- Exact: `'graphics/main'`
- Wildcard: `'sensor/*'`
- Multiple callbacks per pattern supported

**Returns:** Unsubscribe function

#### unsubscribe(pattern, callback)

Unsubscribe from a datapoint.

```javascript
// Remove specific callback
dpManager.unsubscribe('graphics/main', myCallback);

// Remove all callbacks for pattern
dpManager.unsubscribe('graphics/main');
```

#### unsubscribeAll()

Unsubscribe from all datapoints (except @keys).

```javascript
dpManager.unsubscribeAll();
```

#### get(name)

One-time fetch of datapoint value.

```javascript
const data = await dpManager.get('sensor/temperature');
console.log('Current temp:', data.value);
```

**Returns:** Promise resolving to datapoint data

#### refreshKeys()

Refresh list of all available datapoints.

```javascript
dpManager.refreshKeys();

// Listen for update
dpManager.on('keys:update', ({ keys }) => {
    console.log('Available datapoints:', keys);
});
```

#### getKeys()

Get cached list of all datapoint keys.

```javascript
const keys = dpManager.getKeys();
console.log('All datapoints:', keys);
```

#### getSubscriptions()

Get list of currently subscribed patterns.

```javascript
const patterns = dpManager.getSubscriptions();
console.log('Subscribed to:', patterns);
```

#### getSubscriptionInfo(pattern)

Get detailed info about a subscription.

```javascript
const info = dpManager.getSubscriptionInfo('graphics/main');
console.log('Callbacks:', info.callbackCount);
console.log('Last value:', info.lastValue);
console.log('Updates:', info.updateCount);
```

#### getStats()

Get manager statistics.

```javascript
const stats = dpManager.getStats();
console.log('Total updates:', stats.totalUpdates);
console.log('Active subscriptions:', stats.subscriptionCount);
console.log('Known keys:', stats.totalKeys);
```

#### getDatatypeName(dtype)

Convert numeric datatype to name.

```javascript
console.log(dpManager.getDatatypeName(11)); // "JSON"
console.log(dpManager.getDatatypeName(6));  // "DG"
```

#### on(event, callback)

Listen for manager events.

```javascript
// Keys updated
dpManager.on('keys:update', ({ keys }) => {
    console.log('Keys:', keys);
});

// Subscription added
dpManager.on('subscription:added', ({ pattern }) => {
    console.log('Subscribed:', pattern);
});

// Subscription removed
dpManager.on('subscription:removed', ({ pattern }) => {
    console.log('Unsubscribed:', pattern);
});

// Datapoint update
dpManager.on('datapoint:update', (data) => {
    console.log('Any datapoint updated:', data.name);
});

// Connection events
dpManager.on('connected', () => {
    console.log('Connected to dserv');
});

dpManager.on('disconnected', () => {
    console.log('Disconnected from dserv');
});
```

**Returns:** Unsubscribe function

#### dispose()

Cleanup and disconnect.

```javascript
dpManager.dispose();
```

## Usage Examples

### Example 1: Simple Graphics Viewer

```javascript
const wsManager = new DservWebSocket();
wsManager.connect();

const dpManager = new DatapointManager(wsManager);

// Subscribe to graphics stream
dpManager.subscribe('graphics/main', (data) => {
    if (data.value && data.value.commands) {
        renderGraphics(data.value.commands);
    }
});
```

### Example 2: Multiple Sensors

```javascript
const sensors = ['temperature', 'humidity', 'pressure'];

sensors.forEach(sensor => {
    dpManager.subscribe(`sensor/${sensor}`, (data) => {
        updateUI(sensor, data.value);
    });
});
```

### Example 3: Wildcard Subscriptions

```javascript
// Subscribe to all sensor readings
dpManager.subscribe('sensor/*', (data) => {
    console.log(`${data.name}: ${data.value}`);
});
```

### Example 4: With Datapoint Explorer UI

```javascript
// Get list of all datapoints
dpManager.on('keys:update', ({ keys }) => {
    displayDatapointList(keys);
});

// User clicks on datapoint in list
function onDatapointClick(name) {
    dpManager.subscribe(name, (data) => {
        showDatapointDetail(name, data);
    });
}
```

### Example 5: Temporary Subscription

```javascript
// Subscribe, use, then cleanup
const unsubscribe = dpManager.subscribe('experiment/status', (data) => {
    if (data.value === 'complete') {
        console.log('Experiment done!');
        unsubscribe(); // Stop listening
    }
});
```

### Example 6: With Graphics Renderer

```javascript
// Assuming you also have GraphicsRenderer component
const canvas = document.getElementById('myCanvas');
const renderer = new GraphicsRenderer(canvas);

// Connect them together
dpManager.subscribe('graphics/eyetracker', (data) => {
    if (data.value && data.value.commands) {
        renderer.renderCommands(data.value);
    }
});
```

## Integration with Your WebSocket

Your `DservWebSocket` class needs to:

1. **Emit datapoint events:**
```javascript
handleMessage(data) {
    if (data.name && data.data !== undefined) {
        this.emit('datapoint:update', {
            name: data.name,
            data: data.data,
            value: data.data,
            timestamp: data.timestamp,
            dtype: data.dtype
        });
    }
    
    if (data.name === '@keys') {
        this.emit('datapoint:keys', data);
    }
}
```

2. **Support send() method:**
```javascript
send(data) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(JSON.stringify(data));
        return true;
    }
    return false;
}
```

3. **Emit connection events:**
```javascript
this.ws.onopen = () => {
    this.emit('connected');
};

this.ws.onclose = () => {
    this.emit('disconnected');
};
```

Your existing `DservWebSocket` from `app.js` already does all of this! ✅

## Datatype Reference

| Code | Type | Description |
|------|------|-------------|
| 0 | BYTE | Raw bytes |
| 1 | STRING | Text string |
| 2 | FLOAT | 32-bit float |
| 3 | DOUBLE | 64-bit float |
| 4 | SHORT | 16-bit integer |
| 5 | INT | 32-bit integer |
| 6 | DG | Dynamic group |
| 7 | SCRIPT | Tcl script |
| 8 | TRIGGER_SCRIPT | Triggered script |
| 9 | EVENT | Event notification |
| 10 | NONE | No data |
| 11 | JSON | JSON data |
| 12 | ARROW | Apache Arrow |
| 13 | MSGPACK | MessagePack |
| 14 | JPEG | JPEG image |
| 15 | PPM | PPM image |
| 16 | INT64 | 64-bit integer |
| 17 | UNKNOWN | Unknown type |

## Tips

### Performance

- Subscriptions are lightweight - feel free to have many
- History is limited to 100 updates per datapoint
- Pattern matching is done in JavaScript (not server-side)

### Best Practices

```javascript
// ✅ Good: Unsubscribe when done
const cleanup = dpManager.subscribe('temp', callback);
// Later...
cleanup();

// ✅ Good: Use wildcard for related datapoints
dpManager.subscribe('sensor/*', callback);

// ✅ Good: Check if data exists
dpManager.subscribe('data', (data) => {
    if (data.value) {
        process(data.value);
    }
});

// ❌ Avoid: Subscribing in loops without cleanup
for (let i = 0; i < 1000; i++) {
    dpManager.subscribe(`sensor/${i}`, callback); // Memory leak!
}
```

### Debugging

```javascript
// Check connection
console.log(dpManager.getStats());

// List active subscriptions
console.log(dpManager.getSubscriptions());

// Check specific subscription
console.log(dpManager.getSubscriptionInfo('graphics/main'));

// Listen to all updates
dpManager.on('datapoint:update', (data) => {
    console.log('Update:', data.name, data.value);
});
```

## Browser Support

Works in all modern browsers:
- Chrome/Edge 90+
- Firefox 88+
- Safari 14+

## Next Steps

Now that you have DatapointManager, you can:
1. Build `GraphicsRenderer` component that uses it
2. Create other visualization components
3. Build dashboard UIs
4. Create real-time monitoring pages

See `GraphicsRenderer.js` for an example component that uses DatapointManager.
