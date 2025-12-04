# DGTableViewer

A lightweight vanilla JavaScript table viewer for displaying DYN_GROUP data in hybrid JSON format with support for nested arrays.

## Features

- ✅ **Hybrid JSON format support** - handles both primitive columns and nested array references
- ✅ **Expandable arrays** - click on array cells to view details in a modal
- ✅ **2D nested arrays** - displays matrices and nested data in table format
- ✅ **Sorting** - click column headers to sort (works on primitive columns)
- ✅ **Pagination** - handles large datasets efficiently
- ✅ **CSV Export** - export data to CSV file
- ✅ **No dependencies** - pure vanilla JavaScript
- ✅ **Responsive** - adapts to different screen sizes

## Files

- `DGTableViewer.js` - Main JavaScript class
- `DGTableViewer.css` - Stylesheet
- `DGTableViewer_demo.html` - Standalone demo with example data
- `DGTableViewer_dserv_example.html` - Integration example with dserv WebSocket

## Quick Start

### 1. Include the files

```html
<link rel="stylesheet" href="DGTableViewer.css">
<script src="DGTableViewer.js"></script>
```

### 2. Create a container

```html
<div id="table-container"></div>
```

### 3. Initialize with data

```javascript
const data = {
  "name": "experiment_data",
  "rows": [
    {"trial": 1, "rt": 234.5, "accuracy": 1, "fixations": 0},
    {"trial": 2, "rt": 456.2, "accuracy": 1, "fixations": 1},
    {"trial": 3, "rt": 189.7, "accuracy": 0, "fixations": 2}
  ],
  "arrays": {
    "fixations": [
      [0.5, 1.2, 2.3],
      [0.8, 1.9],
      [1.1, 2.2, 3.3, 4.4]
    ]
  }
};

const viewer = new DGTableViewer('table-container', data);
viewer.render();
```

## Data Format

### Hybrid JSON Format (Recommended)

This is the format produced by `dg_to_hybrid_json()` in C:

```javascript
{
  "name": "group_name",           // Optional: name of the dynamic group
  "rows": [                        // Array of row objects
    {
      "trial": 1,                  // Primitive values
      "rt": 234.5,
      "fixations": 0               // Index into arrays object
    },
    // ... more rows
  ],
  "arrays": {                      // Nested array data
    "fixations": [                 // Array indexed by row values
      [0.5, 1.2, 2.3],            // Array for row 0
      [0.8, 1.9],                  // Array for row 1
      // ... more arrays
    ]
  }
}
```

### Simple Array Format

Also supports simple row arrays:

```javascript
[
  {"trial": 1, "rt": 234.5, "accuracy": 1},
  {"trial": 2, "rt": 456.2, "accuracy": 1},
  // ... more rows
]
```

### Columnar Format

If you have columnar data, convert it first:

```javascript
// Columnar data from dg_to_json
const columnarData = {
  "trial": [1, 2, 3],
  "rt": [234.5, 456.2, 189.7],
  "accuracy": [1, 1, 0]
};

// Convert to rows
const columns = Object.keys(columnarData);
const rowCount = columnarData[columns[0]].length;
const rows = [];

for (let i = 0; i < rowCount; i++) {
  const row = {};
  columns.forEach(col => {
    row[col] = columnarData[col][i];
  });
  rows.push(row);
}

const viewer = new DGTableViewer('table-container', {
  name: 'data',
  rows: rows,
  arrays: {}
});
viewer.render();
```

## Options

```javascript
const viewer = new DGTableViewer('container-id', data, {
  pageSize: 50,           // Rows per page (default: 50)
  maxHeight: '500px',     // Max table height (default: '500px')
  showExport: true,       // Show export button (default: true)
  showRowCount: true,     // Show row count in header (default: true)
  compactMode: false      // Use compact spacing (default: false)
});
```

## Usage with dserv

### With WebSocket Connection

```javascript
// 1. Create connection with linked subprocess
const conn = new DservConnection({
    subprocess: 'docs',
    createLinkedSubprocess: true,
    dservHost: 'localhost:2565',
    forceSecure: true
});

await conn.connect();
await conn.sendToLinked('package require dlsh');

// 2. Fetch data
const response = await conn.sendToLinked('dg_to_hybrid_json $trials');
const data = JSON.parse(response);

// 3. Display
const viewer = new DGTableViewer('table-container', data);
viewer.render();
```

### In Documentation/Tutorial Pages

Perfect for displaying command results:

```javascript
// In "Try It" section of command reference
async function runCode() {
    const code = editor.getValue().trim();
    const response = await conn.sendToLinked(code);
    
    // Try to parse as JSON and display in table
    try {
        const data = JSON.parse(response);
        
        // Check if it looks like table data
        if (data.rows || Array.isArray(data)) {
            const viewer = new DGTableViewer('output-container', data, {
                pageSize: 10,
                maxHeight: '300px',
                compactMode: true
            });
            viewer.render();
        } else {
            // Display as text
            outputEl.textContent = response;
        }
    } catch (e) {
        // Not JSON, display as text
        outputEl.textContent = response;
    }
}
```

## Array Display Features

### Simple Arrays (1D)

Displays as a list with indices:

```
[0]: 0.5
[1]: 1.2
[2]: 2.3
```

### Nested Arrays (2D)

Displays as a table with row/column indices:

```
     Col 0  Col 1  Col 2
Row 0  0.00   3.2    100
Row 1  0.01   3.3    101
Row 2  0.02   3.1    102
```

## Styling

The table uses a clean, modern design with:
- Sticky header that stays visible while scrolling
- Hover highlighting on rows
- Sortable columns (indicated by hover effect)
- Clickable array cells (styled in blue)
- Responsive modal for array details

### Customization

Override CSS variables or classes:

```css
/* Change header background */
.dg-table thead {
    background: #f0f0ff;
}

/* Change array link color */
.dg-array-link {
    color: #ff4500;
}

/* Adjust compact mode spacing */
.dg-table-compact td {
    padding: 2px 6px;
}
```

## Browser Support

Works in all modern browsers:
- Chrome/Edge 90+
- Firefox 88+
- Safari 14+

## Examples

See the included demo files:
- `DGTableViewer_demo.html` - Standalone examples with generated data
- `DGTableViewer_dserv_example.html` - Integration with dserv WebSocket

## License

Free to use in your dserv projects.
