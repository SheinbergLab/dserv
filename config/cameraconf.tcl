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
#   camera/full        full-resolution JPEG (private), on demand via
#                      grab_full or grab_nearest
#   camera/full/meta   public JSON completion for each grab_full/grab_nearest:
#                      {req_id frame_id timestamp_us width height jpeg_bytes ok}
#                      (ok 0 + error on failure). ESS waits on this point;
#                      timestamp_us is the sensor capture time on dserv's
#                      timebase, directly comparable with event timestamps.
#   camera/frame_info  human-readable line per snapshot
#   camera/status      continuous | stopped | error: ...
#   camera/interval    actual seconds between snapshots (skip-rate
#                      quantized), or "never" when snapshots are off
#
# Commands (send camera <cmd>):
#  start ?camera_id?           - Start camera streaming
#  stop                        - Stop camera streaming
#  set_interval secs|never     - Seconds between snapshots (live, no
#                                restart); never|off|0 disables snapshots
#                                (grabs unaffected, grab_last starves --
#                                unless look_behind keeps the ring parked)
#  set_rotation deg            - Declare mounting rotation (persists to rig.tcl)
#  grab_full ?req_id?          - Publish the NEXT sensor frame full-res to
#                                camera/full (+ camera/full/meta completion).
#                                Latency <= one frame period + encode; the
#                                image always postdates the request.
#  grab_nearest t_us ?req_id?  - Publish the ALREADY-BUFFERED frame whose
#                                capture time is nearest t_us (dserv us,
#                                same axis as ESS [now]); never waits for
#                                a new frame, so the image may predate the
#                                request. Needs the look_behind setting
#                                for stream-rate history (~16 frames,
#                                ~500 ms at 30 fps).
#  grab_last                   - Publish the most recent ring-buffer frame
#                                (can be up to snapshot_interval old)
#  check_status                - Get camera status
#  check_ring_buffer           - Get ring buffer status (hold/hold_depth
#                                report the look-behind state)
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
# actual (quantized) interval to camera/interval ("never" when snapshots
# are off). Before the camera is initialized the module call errors — the
# chosen interval still publishes and start's own apply_interval picks it
# up. Any OTHER failure must propagate: publishing an interval that did
# not apply is how a page ends up trusting a rate the stream is not
# running at.
proc apply_interval {} {
    if {$::snapshot_interval eq "never"} {
        set skip 0
        set actual never
    } else {
        set skip [expr {max(1, round($::stream_fps * $::snapshot_interval))}]
        set actual [format %.3g [expr {$skip / $::stream_fps}]]
    }
    if {[catch {cameraSetFrameSkipRate $skip} err]} {
        if {![string match "*not initialized*" $err]} { error $err }
    }
    dservSet camera/interval $actual
    return $actual
}

# secs in (0, 3600], or never|off|0: the sensor keeps streaming (grab_full
# still works, and costs nothing extra) but no frame is processed for
# camera/preview or the ring buffer — so grab_last has nothing to serve
# until snapshots are re-enabled.
proc set_interval {secs} {
    if {$secs in {never off 0}} {
        set ::snapshot_interval never
        return [apply_interval]
    }
    if {![string is double -strict $secs] || $secs <= 0 || $secs > 3600} {
        error "set_interval: expected seconds in (0, 3600] or never, got \"$secs\""
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

    # Look-behind pool must also be declared before cameraConfigure: it
    # sizes the DMA allocation (16 extra ~6MB buffers per stream at
    # 1080p). catch: an older module without the command just means no
    # look-behind, not a dead camera.
    set lb 0
    catch { set lb [settings::get camera look_behind] }
    catch { cameraSetRingPool [expr {$lb ? 16 : 0}] }

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

    # With the pool allocated, parking every stream frame costs nothing
    # extra (the DMA memory is already committed, no copies) -- so
    # look_behind on means hold on, no separate toggle to forget. A
    # refusal (allocator granted too few buffers) is reported but leaves
    # the camera up: grabs and preview still work, only look-behind is out.
    set lb_note ""
    if { $lb } {
        if {[catch {cameraSetRingHold 1} result]} {
            puts "look_behind unavailable: $result"
            set lb_note " (look_behind unavailable: too few buffers)"
        }
    } else {
        catch { cameraSetRingHold 0 }
    }

    dservSet camera/status continuous
    camera_health ok "streaming, ~1 JPEG/$::snapshot_interval s to camera/preview$lb_note"
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

# Grab the NEXT sensor frame at full resolution: the module encodes it
# off-thread, publishes the JPEG private to camera/full (capture-time
# timestamp), and announces completion on camera/full/meta. Because the
# sensor free-runs at stream_fps, this is fresh (<= 1 frame period) no
# matter how slow the preview cadence is -- there is no need to raise the
# snapshot rate just to keep grabs current.
#
# If the request cannot even be issued (camera idle/broken), publish the
# failure on camera/full/meta ourselves so a waiting state system's
# contract still resolves without its timeout.
proc grab_full { {req_id 0} } {
    if {[catch {cameraGrabNextFrame camera/full $req_id} result]} {
        set msg [string map {\" ' \\ /} $result]
        dservSet camera/full/meta \
            "{\"req_id\":$req_id,\"ok\":0,\"error\":\"$msg\"}"
        error "Error requesting grab: $result"
    }
    return $req_id
}

# Look-behind grab: publish the already-buffered frame nearest t_us (dserv
# microseconds, the axis of ESS [now] and event stamps). The opposite
# contract from grab_full -- never waits for a future frame, so the image
# may PREDATE the request. The module picks and copies the frame in one
# critical section and resolves through the same worker/meta path as
# grab_full (exact capture-time timestamp_us in the meta). Stream-rate
# history needs the look_behind setting; without it the ring only holds
# preview-cadence frames and "nearest" can be a second stale. An empty
# ring still resolves (ok:0 meta), so a waiting trial cannot hang.
proc grab_nearest { t_us {req_id 0} } {
    if {![string is entier -strict $t_us]} {
        set msg "grab_nearest: expected t_us in dserv microseconds, got \"$t_us\""
        dservSet camera/full/meta \
            "{\"req_id\":$req_id,\"ok\":0,\"error\":\"[string map {\" ' \\ /} $msg]\"}"
        error $msg
    }
    if {[catch {cameraGrabNearest camera/full $t_us $req_id} result]} {
        set msg [string map {\" ' \\ /} $result]
        dservSet camera/full/meta \
            "{\"req_id\":$req_id,\"ok\":0,\"error\":\"$msg\"}"
        error "Error requesting nearest grab: $result"
    }
    return $req_id
}

# Publish the most recent ring-buffer snapshot at full sensor resolution
# to camera/full (the pre-contract behavior: instant, but the frame can be
# up to snapshot_interval old and carries no completion meta).
proc grab_last {} {
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

# Preview/snapshot cadence declared in Hz (the module still thinks in
# seconds via set_interval). This is a WATCHING knob: grab_full uses the
# next-frame contract and is always fresh, so rate_hz never needs raising
# for data collection. 0 turns snapshots off entirely (pure acquisition).
proc apply_capture_hz {hz} {
    if {![string is double -strict $hz] || $hz < 0 || $hz > $::stream_fps} {
        error "rate_hz: expected 0 (off) or Hz in (0, $::stream_fps], got \"$hz\""
    }
    if {$hz == 0} {
        set_interval never
    } else {
        set_interval [expr {1.0 / double($hz)}]
    }
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

# The look-behind pool is sized at cameraConfigure, so a gear flip while
# streaming restarts the stream (a beat of black in the preview; grabs in
# flight resolve as FAILed). An idle camera needs nothing now -- the next
# start reads the setting.
proc apply_look_behind {v} {
    set streaming 0
    if {![catch {cameraStatus} st]} {
        set streaming [expr {[dict getdef $st state idle] eq "streaming"}]
    }
    if {$streaming} {
        stop
        start
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
    -doc "start the camera at boot and keep it streaming (see rate_hz
for the JPEG rate to camera/preview; watch at /camera.html). Off (the
default) loads the subprocess but leaves the sensor untouched --
start/stop by hand still work. Flipping it in the gear starts or stops
the camera immediately." \
    -apply {::apply_enabled}

# Named rate_hz (not capture_hz) so the gear lists it after enabled
# (knobs sort alphabetically by key). Grabs do NOT depend on this: the
# sensor free-runs at stream_fps and grab_full takes the next frame.
settings::declare camera rate_hz -default 1 -values {0 0.2 0.5 1 2 5 10} \
    -type double \
    -doc "camera/preview snapshot rate in Hz (quantized to whole frames
at the ${stream_fps} fps stream; live while streaming). Watching
cadence only -- grab_full captures the next sensor frame regardless.
0 = no snapshots at all: pure acquisition for on-demand grabs." \
    -apply {::apply_capture_hz}

# Look-behind history for ::ess::camera_grab_nearest / camera_grab_before:
# every stream-rate frame is parked in the 16-slot ring (~500 ms at 30 fps)
# by holding its DMA buffer -- no copies, no CPU, but 16 extra ~6MB
# buffers per stream at 1080p (~124MB). That memory is why this is a
# declared rig decision and not always-on. On a Pi 5 the pisp ISP takes
# it from ordinary system RAM (rig-verified at stock CMA); on CMA-backed
# pipelines (Pi 4 / unicam) the pool can exceed stock CMA -- the camera
# still runs, look-behind reports unavailable or hold_depth shrinks to
# what was granted -- until config.txt raises it
# (dtoverlay=vc4-kms-v3d,cma-256). Off costs nothing and grab_full is
# unaffected either way.
settings::declare camera look_behind -default 0 -type bool \
    -doc "keep the last ~half second of stream frames grabbable by
::ess::camera_grab_before / camera_grab_nearest (look-back snapshots).
~124MB of buffer memory while streaming (ordinary RAM on a Pi 5). If
the health line says look_behind unavailable, the platform's CMA is
too small -- raise cma= in config.txt. Flipping while streaming
restarts the stream." \
    -apply {::apply_look_behind}

# Declare does not fire -apply; sync the module interval from the
# effective rate_hz before a possible boot-time start so the first
# stream uses it.
if {[catch { apply_capture_hz [settings::get camera rate_hz] } _hz_err]} {
    puts stderr "camera: rate_hz apply at declare: $_hz_err"
}
unset -nocomplain _hz_err

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
