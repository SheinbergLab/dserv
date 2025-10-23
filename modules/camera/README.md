# Camera Module for Data Server

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
- Manual exposure, gain, brightness, and contrast controls
- Hardware and software FPS control
- Image rotation (0°, 90°, 180°, 270°)
- JPEG and PPM output formats
- Ring buffer for frame access
- Preview stream support
- Integration with data server for real-time publishing

**Key Features:**
- Manual exposure control for consistent lighting (essential for eye tracking)
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
- libcamera and libcamera-dev
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
```

## Command Reference

### Initialization & Configuration

#### `cameraList`
Lists available cameras on the system.

**Syntax:** `cameraList`

**Returns:** Dictionary with camera information
- `count` - Number of cameras found
- `cameras` - List of camera IDs

**Example:**
```tcl
set info [cameraList]
puts "Found [dict get $info count] camera(s)"
foreach cam [dict get $info cameras] {
    puts "  Camera: $cam"
}
```

---

#### `cameraInit ?index?`
Initialize the camera. Must be called before any other camera operations.

**Syntax:** `cameraInit ?index?`

**Arguments:**
- `index` (optional) - Camera index (default: 0)

**Returns:** 0 on success

**Example:**
```tcl
cameraInit 0  ;# Initialize first camera
```

**Notes:**
- Acquires exclusive access to the camera
- Blocks other processes from using the camera
- Must call `cameraRelease` to free the camera

---

#### `cameraConfigure width height ?preview_width preview_height?`
Configure camera resolution and optional preview stream.

**Syntax:** `cameraConfigure width height ?preview_width preview_height?`

**Arguments:**
- `width` - Main stream width in pixels
- `height` - Main stream height in pixels
- `preview_width` (optional) - Preview stream width
- `preview_height` (optional) - Preview stream height

**Returns:** 0 on success

**Example:**
```tcl
# Configure main stream only
cameraConfigure 1920 1080

# Configure with preview stream
cameraConfigure 1920 1080 640 480
```

**Notes:**
- Must be called after `cameraInit` and before capture/streaming
- Camera may adjust requested resolution to supported values
- Preview stream useful for UI display while capturing high-res
- Call `cameraSetRotation` BEFORE this command if rotation needed
- Automatically selects optimal pixel format (RGB888 for CSI, MJPEG for USB)

**Supported Resolutions (typical):**
- 1920x1080 (Full HD)
- 1280x720 (HD)
- 640x480 (VGA)
- Camera-dependent, check your camera specs

---

#### `cameraRelease`
Release the camera and free all resources.

**Syntax:** `cameraRelease`

**Returns:** 0 on success

**Example:**
```tcl
cameraRelease
```

**Notes:**
- Stops any active streaming
- Releases camera for use by other processes
- Should be called before program exit
- Safe to call multiple times

---

### Capture Commands

#### `cameraCapture`
Capture a single image from the camera.

**Syntax:** `cameraCapture`

**Returns:** 0 on success

**Example:**
```tcl
cameraInit
cameraConfigure 1920 1080
cameraCapture
cameraSaveJpeg "capture.jpg"
```

**Notes:**
- Camera must be configured before calling
- Waits for auto-exposure to settle (if enabled)
- Honors `settling_frames` parameter
- Blocks until capture complete (timeout 10 seconds)
- Image data stored internally until saved or overwritten

---

#### `cameraCaptureDatapoint datapoint_name`
Capture an image and publish directly to the data server.

**Syntax:** `cameraCaptureDatapoint datapoint_name`

**Arguments:**
- `datapoint_name` - Name of datapoint to publish

**Returns:** 0 on success

**Example:**
```tcl
cameraCaptureDatapoint "camera/snapshot"
# Image published as JPEG to datapoint
```

**Notes:**
- Combines capture and publish in one operation
- Publishes as JPEG format
- Useful for integration with data server clients
- Does not save to disk

---

#### `cameraSaveJpeg filename`
Save the last captured image as JPEG.

**Syntax:** `cameraSaveJpeg filename`

**Arguments:**
- `filename` - Output filename (path relative to current directory)

**Returns:** 0 on success

**Example:**
```tcl
cameraCapture
cameraSaveJpeg "/tmp/image.jpg"
```

**Notes:**
- Must call `cameraCapture` or `cameraGrabFrame` first
- JPEG quality controlled by `cameraSetJpegQuality`
- Creates parent directories if needed (depends on system)

---

#### `cameraSavePpm filename`
Save the last captured image as PPM (raw RGB).

**Syntax:** `cameraSavePpm filename`

**Arguments:**
- `filename` - Output filename

**Returns:** 0 on success

**Example:**
```tcl
cameraCapture
cameraSavePpm "/tmp/image.ppm"
```

**Notes:**
- PPM is uncompressed RGB format (large files)
- Useful for processing without JPEG artifacts
- Format: P6 binary PPM

---

### Streaming Commands

#### `cameraStartStreaming`
Start continuous streaming from the camera.

**Syntax:** `cameraStartStreaming`

**Returns:** 0 on success

**Example:**
```tcl
cameraInit
cameraConfigure 1920 1080
cameraStartStreaming

# Camera is now streaming, grab frames as needed
while {$running} {
    cameraGrabFrame
    cameraSaveJpeg "frame_[clock milliseconds].jpg"
    after 100
}

cameraStopStreaming
```

**Notes:**
- Camera continuously captures frames
- Frames stored in internal buffer
- Use `cameraGrabFrame` to access current frame
- Much more efficient than repeated `cameraCapture` calls
- Stops automatically when `cameraRelease` called

---

#### `cameraStopStreaming`
Stop streaming from the camera.

**Syntax:** `cameraStopStreaming`

**Returns:** 0 on success

**Example:**
```tcl
cameraStopStreaming
```

**Notes:**
- Safe to call even if not streaming
- Invalidates any buffered frames
- Can restart streaming after stopping

---

#### `cameraGrabFrame`
Get the current frame from the streaming buffer.

**Syntax:** `cameraGrabFrame`

**Returns:** 0 on success

**Example:**
```tcl
cameraStartStreaming
after 100  ;# Let a frame arrive
cameraGrabFrame
cameraSaveJpeg "current_frame.jpg"
```

**Notes:**
- Only works in streaming mode
- Returns most recent frame in buffer
- Non-blocking operation
- Frame may be same as previous call if camera FPS < grab rate

---

### Control Commands

#### `cameraSetSettlingFrames frames`
Set number of frames to wait for auto-exposure to settle.

**Syntax:** `cameraSetSettlingFrames frames`

**Arguments:**
- `frames` - Number of frames to wait (0-100)

**Returns:** Frame count set

**Example:**
```tcl
cameraSetSettlingFrames 10  ;# Wait 10 frames before capture
```

**Notes:**
- Default: 10 frames
- Higher values = more stable exposure, slower capture
- Lower values = faster capture, less stable
- Only affects `cameraCapture`, not streaming
- Set to 0 to capture immediately (may be underexposed)

---

#### `cameraSetJpegQuality quality`
Set JPEG compression quality.

**Syntax:** `cameraSetJpegQuality quality`

**Arguments:**
- `quality` - JPEG quality (1-100, default 85)

**Returns:** Quality value set

**Example:**
```tcl
cameraSetJpegQuality 95  ;# High quality, larger files
cameraSetJpegQuality 60  ;# Lower quality, smaller files
```

**Notes:**
- Higher = better quality, larger file size
- Lower = more compression artifacts, smaller file size
- 85 is good balance for most applications
- 95+ recommended for archival quality
- Below 50 not recommended

---

#### `cameraSetBrightness value`
Adjust image brightness.

**Syntax:** `cameraSetBrightness value`

**Arguments:**
- `value` - Brightness adjustment (-1.0 to 1.0, default 0.0)

**Returns:** Brightness value set

**Example:**
```tcl
cameraSetBrightness 0.0   ;# Normal brightness
cameraSetBrightness 0.2   ;# 20% brighter
cameraSetBrightness -0.3  ;# 30% darker
```

**Notes:**
- Works with both auto and manual exposure
- With auto-exposure: adjusts target brightness
- With manual exposure: post-processing adjustment
- 0.0 = no adjustment
- Positive = brighter, negative = darker
- Applied in hardware by camera ISP

---

#### `cameraSetContrast value`
Adjust image contrast.

**Syntax:** `cameraSetContrast value`

**Arguments:**
- `value` - Contrast adjustment (0.0 to 2.0, default 1.0)

**Returns:** Contrast value set

**Example:**
```tcl
cameraSetContrast 1.0  ;# Normal contrast
cameraSetContrast 1.5  ;# Higher contrast
cameraSetContrast 0.7  ;# Lower contrast
```

**Notes:**
- 1.0 = normal contrast
- > 1.0 = increased contrast
- < 1.0 = decreased contrast
- Applied in hardware by camera ISP

---

#### `cameraSetAutoExposure enabled`
Enable or disable automatic exposure.

**Syntax:** `cameraSetAutoExposure enabled`

**Arguments:**
- `enabled` - Boolean (1/true/yes or 0/false/no)

**Returns:** Boolean value set

**Example:**
```tcl
cameraSetAutoExposure 1  ;# Enable auto-exposure (default)
cameraSetAutoExposure 0  ;# Disable for manual control
```

**Notes:**
- Default: enabled (1)
- When disabled, use `cameraSetExposureTime` and `cameraSetAnalogGain`
- Manual exposure recommended for:
  - Eye tracking (consistent pupil detection)
  - Computer vision (stable thresholds)
  - High contrast scenes (prevent hunting)
  - Frame-to-frame consistency requirements

---

#### `cameraSetExposureTime microseconds`
Set manual exposure time.

**Syntax:** `cameraSetExposureTime microseconds`

**Arguments:**
- `microseconds` - Exposure time (0-1000000, i.e., 0-1 second)

**Returns:** Exposure time set

**Example:**
```tcl
cameraSetAutoExposure 0
cameraSetExposureTime 10000  ;# 10 milliseconds
cameraSetExposureTime 33333  ;# ~30 FPS equivalent
```

**Notes:**
- Only effective when auto-exposure disabled
- Longer exposure = brighter image, more motion blur
- Shorter exposure = darker image, less motion blur
- Typical ranges:
  - Fast motion: 1000-5000 µs (1-5 ms)
  - General use: 10000-20000 µs (10-20 ms)
  - Low light: 30000+ µs (30+ ms)
- 1000000 µs = 1 second (very long exposure)

**Exposure Time Guidelines:**
```
1-5 ms     : Sports, fast motion
5-10 ms    : Normal indoor motion
10-20 ms   : General purpose, good balance
20-40 ms   : Slower motion, more light
40+ ms     : Static scenes, low light
```

---

#### `cameraSetAnalogGain gain`
Set analog gain multiplier.

**Syntax:** `cameraSetAnalogGain gain`

**Arguments:**
- `gain` - Gain multiplier (1.0-16.0)

**Returns:** Gain value set

**Example:**
```tcl
cameraSetAutoExposure 0
cameraSetAnalogGain 1.0  ;# No gain (cleanest image)
cameraSetAnalogGain 2.5  ;# 2.5x gain
cameraSetAnalogGain 8.0  ;# 8x gain (noisy)
```

**Notes:**
- Only effective when auto-exposure disabled
- Higher gain = brighter but noisier image
- 1.0 = no gain (cleanest)
- 2.0 = double brightness
- > 4.0 = noticeable noise
- Use higher exposure time before increasing gain

**Gain Guidelines:**
```
1.0-2.0  : Clean images, minimal noise
2.0-4.0  : Moderate noise, acceptable for most uses
4.0-8.0  : Noticeable noise, use if necessary
8.0-16.0 : High noise, avoid if possible
```

---

#### `cameraSetRotation degrees`
Set camera rotation.

**Syntax:** `cameraSetRotation degrees`

**Arguments:**
- `degrees` - Rotation angle (0, 90, 180, or 270)

**Returns:** Rotation value set

**Example:**
```tcl
cameraInit
cameraSetRotation 90      ;# Rotate 90° clockwise
cameraConfigure 1920 1080 ;# Must configure AFTER rotation
```

**Notes:**
- **Must be called BEFORE `cameraConfigure`**
- Valid values: 0, 90, 180, 270 degrees only
- Rotation applied by camera hardware (no performance penalty)
- Useful when camera is physically rotated
- To change rotation, must reconfigure camera

---

#### `cameraSetFrameSkipRate rate`
Set frame skip rate for streaming.

**Syntax:** `cameraSetFrameSkipRate rate`

**Arguments:**
- `rate` - Number of frames to skip (minimum 1)

**Returns:** 0 on success

**Example:**
```tcl
cameraSetFrameSkipRate 1  ;# Process every frame
cameraSetFrameSkipRate 3  ;# Process every 3rd frame
```

**Notes:**
- Rate of 1 = no skipping (process every frame)
- Rate of N = process every Nth frame
- Useful for reducing CPU load
- Skipped frames still captured but not processed
- Does not affect capture rate, only processing rate

---

#### `cameraSetTargetFPS fps`
Set target frames per second.

**Syntax:** `cameraSetTargetFPS fps`

**Arguments:**
- `fps` - Target FPS (0.0-120.0, 0 = no limit)

**Returns:** FPS value set

**Example:**
```tcl
cameraSetTargetFPS 30.0  ;# Limit to 30 FPS
cameraSetTargetFPS 0.0   ;# No limit (camera default)
```

**Notes:**
- 0.0 = use camera's default frame rate
- Attempts to use hardware FPS control if available
- Falls back to software throttling if hardware unsupported
- Check `fps_control_method` in `cameraStatus` to see method used
- Software throttling may not be as precise

---

#### `cameraSetRingBufferMode mode`
Set ring buffer storage mode.

**Syntax:** `cameraSetRingBufferMode mode`

**Arguments:**
- `mode` - "full_rate" or "skip_rate_only"

**Returns:** 0 on success

**Example:**
```tcl
cameraSetRingBufferMode full_rate     ;# Store all frames
cameraSetRingBufferMode skip_rate_only ;# Store only processed frames
```

**Notes:**
- `full_rate` - Store every frame in ring buffer (default)
- `skip_rate_only` - Store only frames that pass skip filter
- Affects memory usage and frame availability
- Ring buffer size: 16 frames

---

### Continuous Mode Commands

#### `cameraStartContinuous ?save_disk? ?publish_dataserver? ?save_dir? ?datapoint_prefix? ?interval?`
Start continuous capture with automatic saving and/or publishing.

**Syntax:** `cameraStartContinuous ?save_disk? ?publish_dataserver? ?save_dir? ?datapoint_prefix? ?interval?`

**Arguments:**
- `save_disk` (optional) - Save frames to disk (default: 0)
- `publish_dataserver` (optional) - Publish to data server (default: 0)
- `save_dir` (optional) - Directory for saved frames (default: "/tmp/camera_frames/")
- `datapoint_prefix` (optional) - Datapoint prefix (default: "camera")
- `interval` (optional) - Process every Nth frame (default: 1)

**Returns:** 0 on success

**Example:**
```tcl
# Save every frame to disk
cameraStartContinuous 1 0 "/data/captures/" "camera" 1

# Publish every 5th frame to dataserver
cameraStartContinuous 0 1 "" "camera/live" 5

# Both save and publish
cameraStartContinuous 1 1 "/data/" "camera" 1
```

**Notes:**
- Must be in streaming mode first
- Frames saved as JPEG with timestamp in filename
- Background thread handles disk I/O (non-blocking)
- Creates save directory if it doesn't exist
- Stop with `cameraStopContinuous`

---

#### `cameraStartContinuousCallback tcl_proc ?datapoint_prefix? ?interval?`
Start continuous capture with Tcl callback.

**Syntax:** `cameraStartContinuousCallback tcl_proc ?datapoint_prefix? ?interval?`

**Arguments:**
- `tcl_proc` - Tcl procedure name to call for each frame
- `datapoint_prefix` (optional) - Datapoint prefix (default: "camera")
- `interval` (optional) - Call callback every Nth frame (default: 1)

**Returns:** 0 on success

**Example:**
```tcl
proc handleFrame {frame_id timestamp_ms width height ppm_size ae_settled prefix} {
    puts "Frame $frame_id: ${width}x${height} at $timestamp_ms ms"
    
    # Get the frame data
    set ppm [cameraGetPpmCallbackFrame $frame_id]
    
    # Process the frame...
    # ... your code here ...
}

cameraStartStreaming
cameraStartContinuousCallback handleFrame "camera" 1
```

**Callback Signature:**
```tcl
proc callback_name {frame_id timestamp_ms width height ppm_size ae_settled datapoint_prefix} {
    # Your code here
}
```

**Callback Arguments:**
- `frame_id` - Unique frame identifier
- `timestamp_ms` - Capture timestamp (milliseconds since epoch)
- `width` - Frame width in pixels
- `height` - Frame height in pixels
- `ppm_size` - Estimated PPM data size
- `ae_settled` - "true" if auto-exposure settled, "false" otherwise
- `datapoint_prefix` - Datapoint prefix string

**Notes:**
- Callback executed for every Nth frame (based on interval)
- Use frame retrieval commands to access frame data
- Callback must be fast to avoid dropping frames
- Stop with `cameraStopContinuous`

---

#### `cameraStopContinuous`
Stop continuous capture mode.

**Syntax:** `cameraStopContinuous`

**Returns:** 0 on success

**Example:**
```tcl
cameraStopContinuous
```

**Notes:**
- Stops both callback and automatic modes
- Flushes any pending saves
- Invalidates ring buffer frames
- Camera remains in streaming mode

---

### Frame Retrieval Commands

These commands retrieve frames from the ring buffer when in continuous callback mode.

#### `cameraGetPpmCallbackFrame frame_id`
Get frame data as PPM format.

**Syntax:** `cameraGetPpmCallbackFrame frame_id`

**Arguments:**
- `frame_id` - Frame ID from callback

**Returns:** Binary PPM data (Tcl byte array)

**Example:**
```tcl
proc handleFrame {frame_id args} {
    set ppm [cameraGetPpmCallbackFrame $frame_id]
    # Write to file or process...
}
```

---

#### `cameraSavePpmCallbackFrame frame_id filename`
Save frame as PPM file.

**Syntax:** `cameraSavePpmCallbackFrame frame_id filename`

**Arguments:**
- `frame_id` - Frame ID from callback
- `filename` - Output filename

**Returns:** Filename on success

**Example:**
```tcl
proc handleFrame {frame_id args} {
    cameraSavePpmCallbackFrame $frame_id "/tmp/frame_${frame_id}.ppm"
}
```

---

#### `cameraPublishPpmCallbackFrame frame_id datapoint_name`
Publish frame as PPM to datapoint.

**Syntax:** `cameraPublishPpmCallbackFrame frame_id datapoint_name`

**Arguments:**
- `frame_id` - Frame ID from callback
- `datapoint_name` - Datapoint name

**Returns:** Datapoint name on success

**Example:**
```tcl
proc handleFrame {frame_id args} {
    cameraPublishPpmCallbackFrame $frame_id "camera/frame"
}
```

---

#### `cameraGetJpegCallbackFrame frame_id`
Get frame data as JPEG format.

**Syntax:** `cameraGetJpegCallbackFrame frame_id`

**Arguments:**
- `frame_id` - Frame ID from callback

**Returns:** Binary JPEG data (Tcl byte array)

**Example:**
```tcl
proc handleFrame {frame_id args} {
    set jpeg [cameraGetJpegCallbackFrame $frame_id]
    set f [open "frame.jpg" wb]
    puts -nonewline $f $jpeg
    close $f
}
```

---

#### `cameraSaveJpegCallbackFrame frame_id filename`
Save frame as JPEG file.

**Syntax:** `cameraSaveJpegCallbackFrame frame_id filename`

**Arguments:**
- `frame_id` - Frame ID from callback
- `filename` - Output filename

**Returns:** Filename on success

**Example:**
```tcl
proc handleFrame {frame_id args} {
    cameraSaveJpegCallbackFrame $frame_id "/tmp/frame_${frame_id}.jpg"
}
```

---

#### `cameraPublishJpegCallbackFrame frame_id datapoint_name`
Publish frame as JPEG to datapoint.

**Syntax:** `cameraPublishJpegCallbackFrame frame_id datapoint_name`

**Arguments:**
- `frame_id` - Frame ID from callback
- `datapoint_name` - Datapoint name

**Returns:** Datapoint name on success

**Example:**
```tcl
proc handleFrame {frame_id args} {
    cameraPublishJpegCallbackFrame $frame_id "camera/jpeg"
}
```

---

#### `cameraGetPreviewFrame`
Get current preview frame (if preview stream enabled).

**Syntax:** `cameraGetPreviewFrame`

**Returns:** Binary PPM data of preview frame

**Example:**
```tcl
cameraConfigure 1920 1080 640 480  ;# Enable preview
cameraStartStreaming
set preview [cameraGetPreviewFrame]
```

---

#### `cameraSavePreviewFrame filename`
Save preview frame to file.

**Syntax:** `cameraSavePreviewFrame filename`

**Arguments:**
- `filename` - Output filename (PPM format)

**Returns:** Filename on success

---

#### `cameraPublishPreviewFrame datapoint_name`
Publish preview frame to datapoint.

**Syntax:** `cameraPublishPreviewFrame datapoint_name`

**Arguments:**
- `datapoint_name` - Datapoint name

**Returns:** Datapoint name on success

---

#### `cameraGetPreviewInfo`
Get information about preview stream configuration.

**Syntax:** `cameraGetPreviewInfo`

**Returns:** Dictionary with preview information
- `enabled` - Preview stream enabled (boolean)
- `hardware_supported` - Hardware preview available (boolean)
- `width` - Preview width
- `height` - Preview height

---

#### `cameraGetRingBufferStatus`
Get ring buffer status.

**Syntax:** `cameraGetRingBufferStatus`

**Returns:** Dictionary with:
- `oldest_frame_id` - ID of oldest valid frame (-1 if none)
- `newest_frame_id` - ID of newest valid frame (-1 if none)
- `valid_frames` - Number of valid frames in buffer

**Example:**
```tcl
set status [cameraGetRingBufferStatus]
puts "Buffer has [dict get $status valid_frames] valid frames"
puts "Oldest: [dict get $status oldest_frame_id]"
puts "Newest: [dict get $status newest_frame_id]"
```

---

### Status & Information

#### `cameraStatus`
Get comprehensive camera status.

**Syntax:** `cameraStatus`

**Returns:** Dictionary with current camera state and settings

**Dictionary Keys:**
- `available` - Camera support available (boolean)
- `initialized` - Camera initialized (boolean)
- `configured` - Camera configured (boolean)
- `state` - Current state: "idle", "streaming", or "capturing"
- `ae_settled` - Auto-exposure settled (boolean)
- `auto_exposure` - Auto-exposure enabled (boolean)
- `exposure_time` - Current exposure time (microseconds)
- `analog_gain` - Current analog gain
- `brightness` - Current brightness setting
- `contrast` - Current contrast setting
- `rotation` - Current rotation (degrees)
- `configured_fps` - Target FPS (if set)
- `hardware_fps_supported` - Hardware FPS control available (boolean)
- `software_throttling_active` - Software FPS throttling active (boolean)
- `fps_control_method` - "hardware" or "software_throttling"
- `libcamera` - Libcamera support: "yes" or "no"
- `jpeg_support` - JPEG support: "yes" or "no"

**Example:**
```tcl
set status [cameraStatus]

puts "Camera state: [dict get $status state]"
puts "Auto-exposure: [dict get $status auto_exposure]"
puts "Exposure time: [dict get $status exposure_time] µs"
puts "Analog gain: [dict get $status analog_gain]x"
puts "Rotation: [dict get $status rotation]°"

if {[dict get $status configured_fps] > 0} {
    puts "Target FPS: [dict get $status configured_fps]"
    puts "FPS method: [dict get $status fps_control_method]"
}
```

---

## Usage Examples

### Example 1: Basic Single Capture

```tcl
# Simple image capture
cameraInit
cameraConfigure 1920 1080
cameraCapture
cameraSaveJpeg "snapshot.jpg"
cameraRelease
```

### Example 2: Time-lapse Photography

```tcl
cameraInit
cameraConfigure 1920 1080
cameraSetJpegQuality 95

for {set i 0} {$i < 100} {incr i} {
    cameraCapture
    set filename [format "timelapse_%04d.jpg" $i]
    cameraSaveJpeg $filename
    after 60000  ;# Wait 1 minute
}

cameraRelease
```

### Example 3: Manual Exposure for Eye Tracking

```tcl
# Initialize with manual exposure settings
cameraInit
cameraConfigure 1920 1080

# Configure for consistent lighting
cameraSetAutoExposure 0
cameraSetExposureTime 12000    ;# 12ms exposure
cameraSetAnalogGain 2.5
cameraSetBrightness 0.1
cameraSetContrast 1.1

# Start streaming for continuous eye tracking
cameraStartStreaming

# Your eye tracking loop
while {$tracking} {
    cameraGrabFrame
    # Process frame for pupil detection
    # ...
}

cameraStopStreaming
cameraRelease
```

### Example 4: Rotated Camera

```tcl
# Camera is physically rotated 90 degrees
cameraInit
cameraSetRotation 90           ;# MUST be before configure
cameraConfigure 1920 1080
cameraStartStreaming

# Frames now correctly oriented
cameraGrabFrame
cameraSaveJpeg "rotated.jpg"

cameraStopStreaming
cameraRelease
```

### Example 5: Continuous Capture with Callback

```tcl
# Define frame processing callback
proc processFrame {frame_id timestamp width height size settled prefix} {
    puts "Processing frame $frame_id"
    
    # Get JPEG data
    set jpeg [cameraGetJpegCallbackFrame $frame_id]
    
    # Save every 10th frame
    if {$frame_id % 10 == 0} {
        cameraSaveJpegCallbackFrame $frame_id "/data/frame_${frame_id}.jpg"
    }
    
    # Publish to dataserver
    cameraPublishJpegCallbackFrame $frame_id "camera/live"
}

# Start continuous capture
cameraInit
cameraConfigure 1920 1080
cameraStartStreaming
cameraStartContinuousCallback processFrame "camera" 1

# Let it run
after 60000  ;# Run for 1 minute

# Stop
cameraStopContinuous
cameraStopStreaming
cameraRelease
```

### Example 6: Low Light Capture

```tcl
cameraInit
cameraConfigure 1920 1080

# Configure for low light
cameraSetAutoExposure 0
cameraSetExposureTime 50000    ;# 50ms (note: motion blur)
cameraSetAnalogGain 4.0        ;# High gain
cameraSetBrightness 0.2

cameraCapture
cameraSaveJpeg "lowlight.jpg"
cameraRelease
```

### Example 7: Dual Stream (Main + Preview)

```tcl
cameraInit
cameraConfigure 1920 1080 640 480  ;# Main + preview

cameraStartStreaming

# Get high-res main frame
cameraGrabFrame
cameraSaveJpeg "highres.jpg"

# Get low-res preview
cameraGetPreviewFrame
cameraSavePreviewFrame "preview.ppm"

cameraStopStreaming
cameraRelease
```

### Example 8: Auto-Exposure Lock

```tcl
cameraInit
cameraConfigure 1920 1080

# Start with auto-exposure to find good settings
cameraSetAutoExposure 1
cameraStartStreaming

# Wait for AE to settle
while {![dict get [cameraStatus] ae_settled]} {
    after 100
}

# Lock current settings
# (In practice, you'd read actual values from metadata)
cameraSetAutoExposure 0
cameraSetExposureTime 15000
cameraSetAnalogGain 2.0

puts "Auto-exposure locked at 15ms, 2.0x gain"

# Continue with locked exposure
# ...

cameraStopStreaming
cameraRelease
```

### Example 9: High-Speed Capture

```tcl
cameraInit
cameraConfigure 1920 1080

# Configure for minimal motion blur
cameraSetAutoExposure 0
cameraSetExposureTime 2000     ;# 2ms - very fast
cameraSetAnalogGain 8.0        ;# High gain to compensate
cameraSetTargetFPS 120.0       ;# High frame rate

cameraStartStreaming

# Capture fast-moving subject
for {set i 0} {$i < 100} {incr i} {
    cameraGrabFrame
    cameraSaveJpeg "fast_${i}.jpg"
}

cameraStopStreaming
cameraRelease
```

## Best Practices

### Initialization
1. Always call `cameraInit` before any operations
2. Check `cameraStatus` to verify initialization
3. Set rotation BEFORE calling `cameraConfigure`
4. Always call `cameraRelease` when done

### Exposure Control
1. For eye tracking: use manual exposure
   - Prevents brightness fluctuations
   - Ensures consistent pupil detection
   - Typical: 10-15ms exposure, 2-3x gain

2. For general use: auto-exposure is fine
   - Automatically adapts to lighting
   - Use brightness/contrast for fine-tuning

3. Finding manual settings:
   - Start with auto-exposure
   - Wait for `ae_settled`
   - Read values from camera metadata
   - Switch to manual with those values

### Frame Rate Management
1. Use hardware FPS control when possible
2. Set realistic frame rates for your application
3. Use frame skip for processing-heavy tasks
4. Check `fps_control_method` in status

### Memory Management
1. Ring buffer holds 16 frames maximum
2. Use `skip_rate_only` mode to save memory
3. Process frames quickly in callbacks
4. Consider frame skip for high-rate captures

### Error Handling
```tcl
if {[catch {cameraInit} err]} {
    puts "Failed to initialize camera: $err"
    # Handle error
}

if {[catch {cameraCapture} err]} {
    puts "Capture failed: $err"
    cameraRelease
}
```

### Performance
1. JPEG compression is CPU-intensive
   - Use lower quality for faster encoding
   - Consider PPM for processing pipelines
   - Use preview stream for UI displays

2. Streaming is more efficient than repeated captures
   - Use streaming for continuous operation
   - Single capture for occasional snapshots

3. Callback processing
   - Keep callbacks fast
   - Offload heavy processing to separate thread
   - Use frame skip if processing can't keep up

## Troubleshooting

### Camera Not Found
```
Error: Camera not initialized
```
**Solutions:**
- Check camera is connected: `ls /dev/video*`
- Check libcamera installed: `libcamera-hello`
- Verify camera cable connection
- Try different camera index: `cameraInit 1`

### Permission Denied
```
Error: Failed to acquire camera
```
**Solutions:**
- Add user to video group: `sudo usermod -a -G video $USER`
- Check /dev/video* permissions
- Close other applications using camera
- Reboot to apply group changes

### Underexposed Images
**Symptoms:** Images too dark

**Solutions:**
```tcl
# Increase exposure time
cameraSetExposureTime 25000

# Or increase gain
cameraSetAnalogGain 3.0

# Or increase brightness
cameraSetBrightness 0.3
```

### Overexposed Images
**Symptoms:** Images too bright, washed out

**Solutions:**
```tcl
# Decrease exposure time
cameraSetExposureTime 8000

# Or decrease gain
cameraSetAnalogGain 1.5

# Or decrease brightness
cameraSetBrightness -0.2
```

### Motion Blur
**Symptoms:** Moving objects appear blurred

**Solutions:**
```tcl
# Use shorter exposure
cameraSetExposureTime 3000

# Compensate with gain
cameraSetAnalogGain 4.0
```

### Noisy Images
**Symptoms:** Grainy, speckled appearance

**Solutions:**
```tcl
# Reduce gain
cameraSetAnalogGain 2.0

# Increase exposure to compensate
cameraSetExposureTime 20000

# Or improve lighting conditions
```

### Auto-Exposure Hunting
**Symptoms:** Brightness constantly changing

**Solutions:**
```tcl
# Switch to manual exposure
cameraSetAutoExposure 0
cameraSetExposureTime 15000
cameraSetAnalogGain 2.5
```

### Frame Rate Issues
**Symptoms:** Lower FPS than expected

**Check status:**
```tcl
set status [cameraStatus]
puts "FPS method: [dict get $status fps_control_method]"
puts "SW throttling: [dict get $status software_throttling_active]"
```

**Solutions:**
- Hardware FPS may not be supported
- Reduce resolution for higher FPS
- Check CPU load (processing too slow)
- Use frame skip to reduce processing

### Rotation Not Working
**Symptoms:** Image still incorrectly oriented

**Solution:**
```tcl
# Rotation MUST be set before configure
cameraInit
cameraSetRotation 90      # Set rotation FIRST
cameraConfigure 1920 1080  # Then configure
```

### "Command not found" Errors
**Solution:**
```tcl
# Check module loaded
load ./dserv_camera.so

# Or check if already loaded
info commands camera*
```

### JPEG Quality Issues
**Symptoms:** Compression artifacts

**Solutions:**
```tcl
# Increase quality
cameraSetJpegQuality 95

# Or use PPM for lossless
cameraSavePpm "image.ppm"
```

### Ring Buffer Full
**Symptoms:** Frame not found errors

**Check status:**
```tcl
set status [cameraGetRingBufferStatus]
puts "Valid frames: [dict get $status valid_frames]"
```

**Solutions:**
- Process frames faster in callback
- Increase callback interval
- Use `skip_rate_only` buffer mode
- Reduce frame rate

## Advanced Topics

### Camera Control Flow

```
┌─────────────┐
│ cameraInit  │
└──────┬──────┘
       │
       ▼
┌─────────────────┐
│ cameraSetXXX    │ ◄── Set controls (rotation, exposure, etc.)
└──────┬──────────┘
       │
       ▼
┌───────────────────┐
│ cameraConfigure   │ ◄── Must configure after init & controls
└──────┬────────────┘
       │
       ├──────────┬───────────┐
       ▼          ▼           ▼
  ┌─────────┐ ┌────────┐ ┌─────────┐
  │ Capture │ │ Stream │ │ Both OK │
  └─────────┘ └────────┘ └─────────┘
```

### Pixel Format Selection

The module automatically selects optimal pixel format:
- **CSI/Pi Cameras**: RGB888 (ISP-processed, best quality)
- **USB Cameras**: MJPEG (compressed, efficient) → YUYV → RGB888

### Transform Pipeline

Rotation applied via libcamera Transform:
```
Transform::Identity  =   0° (no rotation)
Transform::Rot90     =  90° clockwise
Transform::Rot180    = 180° (upside down)
Transform::Rot270    = 270° clockwise (= 90° CCW)
```

### Control Application

Controls set per-request, applied immediately:
1. Brightness/Contrast → libcamera controls
2. AE Enable → libcamera controls::AeEnable
3. Exposure/Gain → libcamera controls when AE disabled

### Data Flow in Continuous Mode

```
Camera → Callback → Ring Buffer → Your Code
            ↓
        Process/Save/Publish
```

## Performance Characteristics

### Typical Timings (Pi 4, Camera Module 3)

| Operation | Time | Notes |
|-----------|------|-------|
| cameraInit | ~200ms | One-time |
| cameraConfigure | ~100ms | One-time |
| cameraCapture | ~50-200ms | Depends on settling frames |
| JPEG encode | ~20-50ms | Depends on resolution & quality |
| Frame grab (streaming) | <1ms | Just buffer access |

### Memory Usage

| Component | Memory | Notes |
|-----------|--------|-------|
| Single 1920x1080 frame | ~6 MB | Uncompressed RGB |
| Ring buffer (16 frames) | ~96 MB | At 1920x1080 |
| JPEG buffer | ~0.5-2 MB | Depends on quality & content |

## See Also

- libcamera documentation: https://libcamera.org/
- Raspberry Pi camera documentation
- dserv API documentation

## License

See main project LICENSE file.

## Contributing

Report issues and submit improvements through the project repository.

---

**Last Updated:** 2025-01-23
**Version:** 1.0 with manual controls
