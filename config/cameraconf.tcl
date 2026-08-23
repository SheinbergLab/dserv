#
# Camera subprocess -- ~1 snapshot/sec published as JPEG datapoints.
#
# dsconf.tcl starts this only when modules/dserv_camera exists (libcamera
# hosts), so reaching here means the module is present; whether a sensor is
# attached is discovered by `start`, which reports failure on camera/status
# rather than throwing.
#
# The command set and the rig declarations live HERE, not in local/camera.tcl:
# a camera rig gets its commands, its gear entry ("camera enabled" /
# "camera rotation") and the /camera.html page with no hand-copied file at
# all. local/camera.tcl remains the hook for exotic tuning -- see
# local/camera.tcl.EXAMPLE. (Until 2026-08-23 all of this lived only in that
# EXAMPLE, so a rig without the copied file had a camera subprocess with no
# commands and no knobs.)
#
# Datapoints published:
#   camera/preview     JPEG snapshot, every snapshot_interval s (view at /camera.html)
#   camera/full        full-resolution JPEG, on demand via grab_full
#   camera/frame_info  human-readable line per snapshot
#   camera/status      continuous | stopped | error: ...
#   camera/interval    actual seconds between snapshots (skip-rate quantized)
#
# Commands (send camera <cmd>):
#  start ?camera_id?           - Start camera streaming
#  stop                        - Stop camera streaming
#  set_interval secs           - Seconds between snapshots (live, no restart)
#  set_rotation deg            - Declare mounting rotation (persists to rig.tcl)
#  grab_full                   - Publish latest frame full-res to camera/full
#  check_status                - Get camera status
#  check_ring_buffer           - Get ring buffer status
#  get_frame_ppm frame_id      - Get frame as PPM data
#  get_frame_jpeg frame_id     - Get frame as JPEG data
#  save_frame frame_id file    - Save frame (.ppm/.jpg)
#  publish_frame frame_id datapoint ?format? - Publish frame

# Get access to module libraries
set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

# EACH SUBPROCESS GETS ITS OWN INTERPRETER, so the module path dsconf sets
# for the main interp is not inherited -- without this, `package require
# settings` fails and takes the rest of this file with it (soundconf's
# lesson, 8cee1665).
tcl::tm::add $dspath/lib
package require settings

# load camera module (was also loaded in main interp, but here we use it)
load [file join $dspath modules/dserv_camera[info sharedlibextension]]

# enable error logging
errormon enable

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# The green light (camera/health -- see settings-1.0.tm). camera/status
# keeps its continuous/stopped vocabulary for /camera.html; this is the
# gear's coarse translation. NOTE a legacy full-copy local/camera.tcl
# redefines start/stop WITHOUT these writes -- on such a rig the light
# tracks gear flips (apply_enabled below stays ours) and boot, but not a
# bare `send camera start`, until the file is retired.
proc camera_health {state {detail ""}} {
    catch { dservSet camera/health [string trim "$state $detail"] }
}

set last_frame_id -1

# Stream runs at stream_fps; snapshots publish every snapshot_interval seconds
# (rounded to a whole number of skipped frames). Changing snapshot_interval
# while streaming takes effect immediately via set_interval.
set stream_fps 30.0
set snapshot_interval 1.0

# Compute the skip rate for the current interval, apply it, and publish the
# actual (quantized) interval to camera/interval. Before the camera is
# initialized the module call errors — the chosen interval still publishes
# and start's own apply_interval picks it up. Any OTHER failure must
# propagate: publishing an interval that did not apply is how a page ends
# up trusting a rate the stream is not running at.
proc apply_interval {} {
    set skip [expr {max(1, round($::stream_fps * $::snapshot_interval))}]
    if {[catch {cameraSetFrameSkipRate $skip} err]} {
        if {![string match "*not initialized*" $err]} { error $err }
    }
    set actual [expr {$skip / $::stream_fps}]
    dservSet camera/interval [format %.3g $actual]
    return $actual
}

proc set_interval {secs} {
    if {![string is double -strict $secs] || $secs <= 0 || $secs > 3600} {
        error "set_interval: expected seconds in (0, 3600], got \"$secs\""
    }
    set ::snapshot_interval $secs
    return [apply_interval]
}

# Called once per non-skipped frame (~1/sec with the rates set in start)
proc my_frame_handler {frame_id timestamp_ms width height ppm_size ae_settled datapoint_prefix} {
    set ::last_frame_id $frame_id

    set timestamp_sec [expr $timestamp_ms / 1000]
    set datetime [clock format $timestamp_sec -format "%Y-%m-%d %H:%M:%S"]
    dservSet $datapoint_prefix/frame_info \
	"Frame $frame_id at $datetime: ${width}x${height}, AE settled: $ae_settled"

    # Preview-sized JPEG (dimensions set in cameraConfigure below)
    cameraPublishPreviewFrame $frame_id $datapoint_prefix/preview
}

# start streaming
proc start { { camera_id 0 } } {

    # Initialize camera
    if {[catch {cameraInit $camera_id} result]} {
        puts "Error initializing camera: $result"
        dservSet camera/status "error: $result"
        camera_health error $result
        return
    }

    # Declared mounting rotation (local/rig.tcl); must be set before
    # cameraConfigure
    cameraSetRotation [settings::get camera rotation]

    # Native 1920x1080 @ stream_fps; preview 640x360 keeps the 16:9 aspect
    if {[catch {cameraConfigure 1920 1080 $::stream_fps 640 360} result]} {
        puts "Error configuring camera: $result"
        dservSet camera/status "error: $result"
        camera_health error $result
        return
    }

    # Rig tuning hook: define `camera_tune` in local/camera.tcl (exposure
    # locks, white balance, sharpness -- anything that must land between
    # configure and streaming). See local/camera.tcl.EXAMPLE. A tuning error
    # is reported but does not keep the camera down: wrong colors beat no
    # picture.
    if { [llength [info procs camera_tune]] } {
        if {[catch {camera_tune} result]} {
            puts "Error in camera_tune (continuing untuned): $result"
        }
    }

    # Start streaming
    if {[catch {cameraStartStreaming} result]} {
        puts "Error starting streaming: $result"
        dservSet camera/status "error: $result"
        camera_health error $result
        return
    }

    # Publish cadence: skip rate from snapshot_interval, callback on every
    # processed frame
    apply_interval

    if {[catch {cameraStartContinuousCallback my_frame_handler "camera" 1} result]} {
        puts "Error starting continuous callback: $result"
        dservSet camera/status "error: $result"
        camera_health error $result
        return
    }

    dservSet camera/status continuous
    camera_health ok "streaming, ~1 JPEG/$::snapshot_interval s to camera/preview"
    puts "Camera started - JPEG every $::snapshot_interval s to camera/preview"
}

# stop streaming
proc stop {} {
    dservSet camera/status stopped
    camera_health off stopped
    if {[catch {cameraStopContinuous} result]} {
        puts "Error stopping continuous mode: $result"
    }
    if {[catch {cameraStopStreaming} result]} {
        puts "Error stopping streaming: $result"
    }
    puts "Camera stopped"
}

# Publish the most recent snapshot at full sensor resolution to camera/full
proc grab_full {} {
    if { $::last_frame_id < 0 } {
        error "no frame captured yet - is the camera started?"
    }
    if {[catch {cameraPublishJpegCallbackFrame $::last_frame_id camera/full} result]} {
        error "Error publishing full frame: $result"
    }
    return $::last_frame_id
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

# Get a specific frame as PPM data
proc get_frame_ppm {frame_id} {
    if {[catch {cameraGetPpmCallbackFrame $frame_id} ppm_data]} {
        puts "Error getting PPM frame $frame_id: $ppm_data"
        return ""
    }
    return $ppm_data
}

# Get a specific frame as JPEG data (on-demand encoding)
proc get_frame_jpeg {frame_id} {
    if {[catch {cameraGetJpegCallbackFrame $frame_id} jpeg_data]} {
        puts "Error getting JPEG frame $frame_id: $jpeg_data"
        return ""
    }
    return $jpeg_data
}

# Save a specific frame from ring buffer
proc save_frame {frame_id filename} {
    # Determine format from filename extension
    set ext [string tolower [file extension $filename]]

    if {$ext eq ".ppm"} {
        if {[catch {cameraSavePpmCallbackFrame $frame_id $filename} result]} {
            puts "Error saving PPM frame: $result"
            return 0
        }
    } elseif {$ext eq ".jpg" || $ext eq ".jpeg"} {
        if {[catch {cameraSaveJpegCallbackFrame $frame_id $filename} result]} {
            puts "Error saving JPEG frame: $result"
            return 0
        }
    } else {
        puts "Unsupported file extension: $ext (use .ppm, .jpg, or .jpeg)"
        return 0
    }

    puts "Saved frame $frame_id to $filename"
    return 1
}

# Publish a specific frame from ring buffer to a custom datapoint
proc publish_frame {frame_id datapoint_name {format "ppm"}} {
    if {$format eq "ppm"} {
        if {[catch {cameraPublishPpmCallbackFrame $frame_id $datapoint_name} result]} {
            puts "Error publishing PPM frame: $result"
            return 0
        }
    } elseif {$format eq "jpeg"} {
        if {[catch {cameraPublishJpegCallbackFrame $frame_id $datapoint_name} result]} {
            puts "Error publishing JPEG frame: $result"
            return 0
        }
    } else {
        puts "Unsupported format: $format (use 'ppm' or 'jpeg')"
        return 0
    }

    puts "Published frame $frame_id as $format to $datapoint_name"
    return 1
}

# Rotation is the RIG's mounting declaration -- enclosures mount cameras
# sideways -- so it lives with the other declared rig facts in local/rig.tcl
# ("setting camera rotation 90"). set_rotation (the /camera.html selector
# calls it) persists the new value there and cycles a running camera; on a
# fresh start the declared value applies directly. 90/270 rotate in software
# during JPEG encode (Pi hardware does 0/180 only); at snapshot rates the
# cost is negligible.
proc apply_rotation {deg} {
    if {[catch {cameraStatus} st]} { return }
    # `state` appears in the status dict only once the capture object exists
    # (pre-init the module reports available/initialized/configured only) --
    # getdef, or a gear write to an idle camera throws (found on imx708).
    if {[dict getdef $st state idle] eq "streaming"} {
        stop
        start
    }
}

proc set_rotation {deg} {
    settings::put camera rotation $deg -persist
}

# Live enable/disable: the gear flips this without a restart. Uses
# cameraStatus rather than a shadow flag so a start that FAILED (no sensor)
# reads as not-streaming and a later enable retries it.
proc apply_enabled {v} {
    set streaming 0
    if {![catch {cameraStatus} st]} {
        # dict getdef: `state` is absent until the capture object exists
        set streaming [expr {[dict getdef $st state idle] eq "streaming"}]
    }
    if {$v && !$streaming} {
        start
        # Our start reports its own health; a legacy local/camera.tcl start
        # does not -- read the outcome back so a gear flip stays truthful
        # on those rigs, without downgrading the richer line ours wrote.
        set _cur ""; catch { set _cur [dservGet camera/health] }
        if {![string match "ok *" $_cur] &&
            ![catch {cameraStatus} st2] &&
            [dict getdef $st2 state idle] eq "streaming"} {
            camera_health ok streaming
        }
    } elseif {!$v && $streaming} {
        stop
        camera_health off stopped     ;# same idem line for a legacy stop
    }
}

# allow local override for this system (tuning, or wholesale proc overrides;
# old full copies of the EXAMPLE still work -- they redefine the same procs
# and re-declare rotation with the same schema)
set localconf [file join  $dspath local camera.tcl]
if { [file exists $localconf] } {
    source $localconf
}

# Declarations AFTER the local file, so a declaration wins over anything a
# legacy copy set by hand (the openephys ordering).
settings::declare camera rotation -default 0 -values {0 90 180 270} -type int \
    -doc "Camera mounting rotation, degrees clockwise" \
    -apply {::apply_rotation}

settings::declare camera enabled -default 0 -type bool \
    -doc "start the camera at boot and keep it streaming (~1 JPEG/s to
camera/preview; watch at /camera.html). Off (the default) loads the
subprocess but leaves the sensor untouched -- start/stop by hand
still work. Flipping it in the gear starts or stops the camera
immediately." \
    -apply {::apply_enabled}

# Boot-time start. `enabled` is new -- no legacy local/camera.tcl ever
# auto-started -- so the value is used directly: only a declaration can make
# it 1, and the default 0 means a rig that declares nothing boots exactly as
# it always did (camera idle until someone says start; the green light says
# so rather than staying dark).
if {[catch {
    if { [settings::get camera enabled] } {
        start
    } else {
        camera_health off "idle -- declare `camera enabled 1` to stream at boot"
    }
} _err]} {
    puts stderr "camera: boot-time start failed: $_err"
}
unset -nocomplain _err
