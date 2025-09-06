# Simple example: 1fps continuous capture with callback

# Define the callback function that will be called for each frame
proc my_frame_handler {frame_id timestamp_ms width height jpeg_size ae_settled datapoint_prefix} {
    # Convert timestamp to readable format
    set timestamp_sec [expr $timestamp_ms / 1000]
    set datetime [clock format $timestamp_sec -format "%Y-%m-%d %H:%M:%S"]
    
    # Print frame information
    puts "Frame $frame_id captured at $datetime"
    puts "  Resolution: ${width}x${height}"
    puts "  JPEG size: $jpeg_size bytes ([expr $jpeg_size/1024]KB)"
    puts "  Auto-exposure settled: $ae_settled"
    puts "  Datapoint prefix: $datapoint_prefix"
    
    # Optional: Save every 10th frame to disk for testing
    if {$frame_id % 10 == 0} {
        if {[catch {cameraSaveCallbackFrame $frame_id "/tmp/test_frame_${frame_id}.jpg"} result]} {
            puts "  Warning: Could not save frame $frame_id: $result"
        } else {
            puts "  Saved frame $frame_id to: $result"
        }
    }
    
    # Optional: Publish every 5th frame to dataserver  
    if {$frame_id % 5 == 0} {
        if {[catch {cameraPublishCallbackFrame $frame_id "test_stream/frame"} result]} {
            puts "  Warning: Could not publish frame $frame_id: $result"
        } else {
            puts "  Published frame $frame_id to: $result"
        }
    }
    
    puts "---"
}

# Main setup function
proc setup_1fps_test {} {
    puts "Setting up camera for 1fps continuous capture test..."
    
    # Initialize camera
    if {[catch {cameraInit 1} result]} {
        puts "Error initializing camera: $result"
        return
    }
    puts "Camera initialized"
    
    # Configure camera at 1920x1080, 1fps
    if {[catch {cameraConfigure 1920 1080 1.0} result]} {
        puts "Error configuring camera: $result"
        return
    }
    puts "Camera configured for 1920x1080 @ 1fps"
    
    # Start streaming
    if {[catch {cameraStartStreaming} result]} {
        puts "Error starting streaming: $result"
        return
    }
    puts "Camera streaming started"
    
    # Start continuous callback mode with 1fps (every frame since camera is at 1fps)
    if {[catch {cameraStartContinuousCallback my_frame_handler "test_camera" 1} result]} {
        puts "Error starting continuous callback: $result"
        return
    }
    puts "Continuous callback mode started!"
    puts "Frames will be processed at 1fps with my_frame_handler"
    puts ""
    puts "Let this run for a while, then call stop_test to stop..."
}

# Stop function
proc stop_test {} {
    puts "Stopping continuous mode..."
    
    if {[catch {cameraStopContinuous} result]} {
        puts "Error stopping continuous mode: $result"
    } else {
        puts "Continuous mode stopped"
    }
    
    if {[catch {cameraStopStreaming} result]} {
        puts "Error stopping streaming: $result"
    } else {
        puts "Streaming stopped"
    }
    
    puts "Test complete!"
}

# Status check function
proc check_status {} {
    puts "Camera Status:"
    if {[catch {cameraStatus} status]} {
        puts "Error getting status: $status"
        return
    }
    
    foreach {key value} $status {
        puts "  $key: $value"
    }
    
    puts ""
    puts "Ring Buffer Status:"
    if {[catch {cameraGetRingBufferStatus} ring_status]} {
        puts "Error getting ring buffer status: $ring_status"
        return
    }
    
    foreach {key value} $ring_status {
        puts "  $key: $value"
    }
}

# Run the test
puts "=== Camera 1fps Continuous Callback Test ==="
puts ""
puts "Commands available:"
puts "  setup_1fps_test  - Start the test"
puts "  stop_test        - Stop the test"
puts "  check_status     - Check camera and ring buffer status"
puts ""
puts "Call: setup_1fps_test"
