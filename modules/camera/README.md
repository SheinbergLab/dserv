# Camera Module for Data Server (dserv)

Raspberry Pi camera interface module using libcamera. Provides Tcl commands for camera initialization, configuration, image capture, and streaming with manual exposure controls.

## Table of Contents

- [Overview](#overview)
- [Requirements](#requirements)
- [Building](#building)
- [Quick Start](#quick-start)
- [Command Reference](#command-reference)
  - [Initialization & Configuration](#initialization--configuration)
  - [Capture Commands](#capture-commands)
  - [Streaming Commands](#streaming-commands)
  - [Control Commands](#control-commands)
  - [Continuous Mode Commands](#continuous-mode-commands)
  - [Frame Retrieval Commands](#frame-retrieval-commands)
  - [Status & Information](#status--information)
- [Usage Examples](#usage-examples)
- [Best Practices](#best-practices)
- [Troubleshooting](#troubleshooting)

## Overview

This module provides a comprehensive interface to Raspberry Pi cameras through libcamera. It supports:

- Single-shot capture and continuous streaming
- Manual exposure, gain, brightness, contrast, and sharpness controls
- Manual white balance (red/blue gain) for consistent color
- Hardware and software FPS control
- Image rotation (0°, 90°, 180°, 270°)
- JPEG and PPM output formats
- Ring buffer for frame access
- Preview stream support
- Integration with data server for real-time publishing

**Key Features:**
- Manual exposure control for consistent lighting
- Manual white balance for color consistency
- Sharpness control for image clarity
- Dual stream support (main + preview)
- Callback-based continuous capture
- Background JPEG encoding
- Configurable frame skip rates

## Requirements

### Hardware
- Raspberry Pi (tested on Pi 4, Pi 5)
- Raspberry Pi Camera Module (tested with Camera Module 3)
- USB cameras supported via libcamera

### Software
- libcamera and libcamera-dev (tested with 0.5.2)
- libjpeg-dev (optional, for JPEG support)
- Tcl 8.6 or later
- Data server framework

### Build Defines
- `HAS_LIBCAMERA` - Enable libcamera support
- `HAS_JPEG` - Enable JPEG encoding

## Building

```bash
# Install dependencies
sudo apt-get install libcamera-dev libjpeg-dev

# Build the module
make

# The module will be built as dserv_camera.so
```

## Quick Start

```tcl
# Load the module
load ./dserv_camera.so

# Basic usage
cameraInit
cameraConfigure 1920 1080
cameraCapture
cameraSaveJpeg "image.jpg"
cameraRelease

# Streaming usage
cameraInit
cameraConfigure 1920 1080
cameraStartStreaming
# ... grab frames ...
cameraStopStreaming
cameraRelease

# Fixed lighting setup
cameraInit
cameraConfigure 1920 1080
cameraSetAutoExposure 0
cameraSetExposureTime 15000
cameraSetAnalogGain 2.0
cameraSetAutoWhiteBalance 0
cameraSetRedGain 1.8
cameraSetBlueGain 1.5
cameraSetSharpness 1.3
cameraStartStreaming
```

## Command Reference

See the [full documentation](https://github.com/your-org/dserv) for complete command reference.

### Quick Command List

**Initialization:**
- `cameraList` - List available cameras
- `cameraInit ?index?` - Initialize camera
- `cameraConfigure width height ?preview_w preview_h?` - Configure resolution
- `cameraRelease` - Release camera

**Capture:**
- `cameraCapture` - Capture single frame
- `cameraSaveJpeg filename` - Save as JPEG
- `cameraSavePpm filename` - Save as PPM

**Streaming:**
- `cameraStartStreaming` - Start continuous streaming
- `cameraStopStreaming` - Stop streaming
- `cameraGrabFrame` - Get current frame

**Exposure Controls:**
- `cameraSetAutoExposure enabled` - Enable/disable auto-exposure
- `cameraSetExposureTime microseconds` - Set exposure time (0-1000000 µs)
- `cameraSetAnalogGain gain` - Set analog gain (1.0-16.0)

**Image Quality:**
- `cameraSetBrightness value` - Adjust brightness (-1.0 to 1.0)
- `cameraSetContrast value` - Adjust contrast (0.0 to 2.0)
- `cameraSetSharpness value` - Adjust sharpness (0.0 to 2.0)
- `cameraSetJpegQuality quality` - Set JPEG quality (1-100)

**White Balance:**
- `cameraSetAutoWhiteBalance enabled` - Enable/disable auto WB
- `cameraSetRedGain gain` - Set red gain (0.5-4.0)
- `cameraSetBlueGain gain` - Set blue gain (0.5-4.0)

**Other:**
- `cameraSetRotation degrees` - Set rotation (0, 90, 180, 270) - call before configure
- `cameraSetTargetFPS fps` - Set target frame rate
- `cameraStatus` - Get comprehensive status

## Usage Examples

### Example 1: Fixed Indoor Lighting (Monitoring)

```tcl
# Perfect for fixed lab/office lighting
cameraInit
cameraConfigure 1920 1080

# Lock all settings for consistency
cameraSetAutoExposure 0
cameraSetExposureTime 15000      ;# 15ms
cameraSetAnalogGain 2.0

cameraSetAutoWhiteBalance 0
cameraSetRedGain 1.8             ;# Tuned for your lighting
cameraSetBlueGain 1.5

cameraSetSharpness 1.3           ;# Extra clarity
cameraSetContrast 1.1

cameraStartStreaming

# Periodic monitoring
while {$monitoring} {
    cameraGrabFrame
    cameraSaveJpeg "monitor_[clock seconds].jpg"
    after 5000
}

cameraStopStreaming
cameraRelease
```

### Example 2: Quick Auto Mode

```tcl
# Let camera handle everything
cameraInit
cameraConfigure 1920 1080
cameraSetAutoExposure 1
cameraSetAutoWhiteBalance 1
cameraCapture
cameraSaveJpeg "auto.jpg"
cameraRelease
```

### Example 3: Finding Your Settings

```tcl
# Use this to find good manual settings for your setup
cameraInit
cameraConfigure 1920 1080

# Start with auto
cameraSetAutoExposure 1
cameraSetAutoWhiteBalance 1
cameraStartStreaming
after 2000  ;# Let it settle

# Take reference shot
cameraGrabFrame
cameraSaveJpeg "auto_reference.jpg"

# Check what auto chose
set status [cameraStatus]
puts "Auto-exposure settled: [dict get $status ae_settled]"

# Now lock those settings (you'd tune these values)
cameraSetAutoExposure 0
cameraSetExposureTime 15000
cameraSetAnalogGain 2.0

cameraSetAutoWhiteBalance 0
cameraSetRedGain 1.8
cameraSetBlueGain 1.5

# Test locked settings
cameraGrabFrame
cameraSaveJpeg "manual_test.jpg"

cameraStopStreaming
cameraRelease

# Compare auto_reference.jpg vs manual_test.jpg
# Adjust gains/exposure as needed
```

## Best Practices

### For Fixed Lighting (Recommended for most labs/offices)

1. **Find settings once:**
   - Use auto-exposure/white balance
   - Let it settle
   - Note the values
   - Switch to manual with those values

2. **Lock everything:**
   ```tcl
   cameraSetAutoExposure 0
   cameraSetExposureTime 15000
   cameraSetAnalogGain 2.0
   cameraSetAutoWhiteBalance 0
   cameraSetRedGain 1.8
   cameraSetBlueGain 1.5
   ```

3. **Benefits:**
   - Identical appearance across all captures
   - No brightness/color fluctuations
   - Reproducible results

### White Balance Calibration

```tcl
# Point camera at white/gray reference (paper, card)
cameraInit
cameraConfigure 1920 1080
cameraSetAutoWhiteBalance 1
cameraCapture
cameraSaveJpeg "wb_auto.jpg"

# Now adjust manually until white looks neutral
cameraSetAutoWhiteBalance 0
cameraSetRedGain 1.8   ;# Adjust up/down
cameraSetBlueGain 1.5  ;# Adjust up/down
cameraCapture
cameraSaveJpeg "wb_manual.jpg"

# Save your values for future use!
```

### Typical Indoor Lighting Values

```
Fluorescent: Red 1.7-1.9, Blue 1.4-1.6
LED (daylight): Red 1.4-1.6, Blue 1.6-1.8
Incandescent: Red 1.1-1.3, Blue 1.9-2.1
```

## Troubleshooting

### Images have color tint

```tcl
# Yellow/warm tint
cameraSetAutoWhiteBalance 0
cameraSetRedGain 1.5    ;# Decrease red
cameraSetBlueGain 2.0   ;# Increase blue

# Blue/cool tint
cameraSetRedGain 2.0    ;# Increase red
cameraSetBlueGain 1.3   ;# Decrease blue
```

### Images too dark

```tcl
cameraSetExposureTime 25000  ;# Increase exposure
# or
cameraSetAnalogGain 3.0      ;# Increase gain
```

### Images too bright

```tcl
cameraSetExposureTime 10000  ;# Decrease exposure
# or
cameraSetAnalogGain 1.5      ;# Decrease gain
```

### Images soft/blurry

```tcl
cameraSetSharpness 1.5       ;# Increase sharpness
# Also check focus and motion blur
```

### Brightness keeps changing

```tcl
# Lock exposure
cameraSetAutoExposure 0
cameraSetExposureTime 15000
cameraSetAnalogGain 2.0
```

## Advanced Topics

### Control Ranges

| Control | Range | Default | Notes |
|---------|-------|---------|-------|
| ExposureTime | 0-1000000 µs | Auto | Longer = brighter, more blur |
| AnalogGain | 1.0-16.0 | 1.0 | Higher = brighter, noisier |
| Brightness | -1.0 to 1.0 | 0.0 | Post-processing adjustment |
| Contrast | 0.0 to 2.0 | 1.0 | 1.0 = normal |
| Sharpness | 0.0 to 2.0 | 1.0 | >1.5 may show artifacts |
| RedGain | 0.5 to 4.0 | 1.0 | For white balance |
| BlueGain | 0.5 to 4.0 | 1.0 | For white balance |

### Status Dictionary

```tcl
set status [cameraStatus]

# Key fields:
# - state: "idle", "streaming", "capturing"
# - auto_exposure: boolean
# - exposure_time: microseconds
# - analog_gain: multiplier
# - brightness, contrast, sharpness: current values
# - auto_white_balance: boolean
# - red_gain, blue_gain: WB gains
# - ae_settled: has auto-exposure converged
```

## See Also

- libcamera documentation: https://libcamera.org/
- Raspberry Pi camera documentation
- Data server API documentation

## Contributing

Report issues and submit improvements through the project repository.

---

**Last Updated:** 2025-01-23
**Version:** 1.1 with white balance and sharpness controls
