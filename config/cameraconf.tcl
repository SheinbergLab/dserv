#
# Process camera commands
#

set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

tcl::tm::add $dspath/lib

# load camera module (was also loaded in main interp, but here we use it)
load [file join $dspath modules/dserv_camera[info sharedlibextension]]


# Define the callback function that will be called for each frame
proc my_frame_handler {frame_id timestamp_ms width height jpeg_size ae_settled datapoint_prefix} {
    # Convert timestamp to readable format
    set timestamp_sec [expr $timestamp_ms / 1000]
    set datetime [clock format $timestamp_sec -format "%Y-%m-%d %H:%M:%S"]
    
    # Print frame information
    puts "Frame $frame_id captured at $datetime"
    puts "  Resolution: ${width}x${height}"
    puts "  JPEG size: $jpeg_size bytes ([expr $jpeg_size/1024]KB)"

    # to save frames to disk call:
    #  cameraSaveCallbackFrame $frame_id $filename

    # to push to dataserver call:
    #  cameraPublishCallbackFrame $frame_id $datapoint_name
}

# start streaming
proc start { { camera_id 0 } } {
   
    # Initialize camera
    if {[catch {cameraInit $camera_id} result]} {
        puts "Error initializing camera: $result"
        return
    }
    
    # Configure camera at 1920x1080, 1fps
    if {[catch {cameraConfigure 1920 1080 1.0} result]} {
        puts "Error configuring camera: $result"
        return
    }
    
    # Start streaming
    if {[catch {cameraStartStreaming} result]} {
        puts "Error starting streaming: $result"
        return
    }
    puts "Camera streaming started"
    
    # Start continuous callback mode on every (1) frame
    if {[catch {cameraStartContinuousCallback my_frame_handler "camera" 1} result]} {
        puts "Error starting continuous callback: $result"
        return
    }
}

# stop streaming
proc stop {} {
    puts "Stopping continuous mode..."
    
    if {[catch {cameraStopContinuous} result]} {
        puts "Error stopping continuous mode: $result"
    } 

    if {[catch {cameraStopStreaming} result]} {
        puts "Error stopping streaming: $result"
    }
}

# Status check function
proc check_status {} {
    if {[catch {cameraStatus} status]} {
        puts "Error getting status: $status"
        return
    }

    return $status
}

proc check_ring_buffer {} {
    if {[catch {cameraGetRingBufferStatus} ring_status]} {
        puts "Error getting ring buffer status: $ring_status"
        return
    }

    return $ring_status
}


