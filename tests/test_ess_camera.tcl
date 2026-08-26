#
# test_ess_camera.tcl
#
#  The camera grab contract (::ess::camera_grab / camera_grab_complete /
#  camera_grab_done / camera_grab_ok): an action requests a snapshot, the
#  camera/full/meta dpoint callback resolves it, transitions poll the
#  predicate. Extracts the CAMERA GRAB CONTRACT section from ess-2.0.tm so
#  the test tracks the file rather than a copy of it.
#
#  A UNIT test: dserv is STUBBED, so this runs under plain tclsh with no
#  server, no rig and no camera (the test_ess_binds pattern).
#
#  Run as: tclsh tests/test_ess_camera.tcl      (or: ctest -R ess_camera)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

set f [open [file join $::REPO lib ess-2.0.tm] r]
set src [read $f]
close $f

# Pull the section out by its delimiting banners (string search, not
# regex -- the file is ~310KB).
set start [string first "# CAMERA GRAB CONTRACT" $src]
set end   [string first "# WHICH EXTIO ANALOG STREAMS THIS SYSTEM RECORDS" $src]
if {$start < 0 || $end < 0 || $end <= $start} {
    puts "FAIL: could not locate CAMERA GRAB CONTRACT section in ess-2.0.tm"
    exit 1
}
set section [string range $src $start [expr {$end - 1}]]

# ---- dserv / ess stubs; record everything the contract touches ----
set ::log {}
namespace eval ess {
    proc evt_put {type subtype time args} {
        lappend ::log [list evt $type $subtype $args]
    }
    proc do_update {} { lappend ::log do_update }
}
proc now {} { return 123456789 }
proc sendNoReply {who script} { lappend ::log [list send $who $script] }
proc dservAddExactMatch {name} { lappend ::log [list match $name] }
proc dpointSetScript {name script} { lappend ::log [list script $name $script] }

namespace eval ess $section

set npass 0
set nfail 0
proc check {label expr} {
    global npass nfail
    if {[uplevel 1 [list expr $expr]]} {
        incr npass
        puts "PASS  $label"
    } else {
        incr nfail
        puts "FAIL  $label  ($expr)"
    }
}

# ---- a fresh grab: request id 1, contract open until meta arrives ----
set req [::ess::camera_grab]
check "first request id is 1"      {$req == 1}
check "REQUEST event stamped"      {[list evt CAMERA REQUEST 1] in $::log}
check "grab sent to camera"        {[list send camera {grab_full 1}] in $::log}
check "binding self-asserted"      {[list match camera/full/meta] in $::log}
check "contract open"              {![::ess::camera_grab_done]}

# ---- completion resolves it ----
set ::log {}
::ess::camera_grab_complete camera/full/meta \
    {{"req_id":1,"frame_id":42,"timestamp_us":1700000000123456,"ok":1}}
check "contract resolved"          {[::ess::camera_grab_done]}
check "resolution ok"              {[::ess::camera_grab_ok]}
check "DONE event stamped"         {[list evt CAMERA DONE 1] in $::log}
check "state machine woken"        {"do_update" in $::log}

# ---- failure path ----
set ::log {}
set req [::ess::camera_grab]
check "second request id is 2"     {$req == 2}
check "contract open again"        {![::ess::camera_grab_done]}
::ess::camera_grab_complete camera/full/meta \
    {{"req_id":2,"ok":0,"error":"jpeg encode failed"}}
check "failed contract resolved"   {[::ess::camera_grab_done]}
check "resolution not ok"          {![::ess::camera_grab_ok]}
check "FAIL event stamped"         {[list evt CAMERA FAIL 2] in $::log}

# ---- a manual grab's meta (req_id 0) must not disturb the sequence ----
set ::log {}
::ess::camera_grab_complete camera/full/meta \
    {{"req_id":0,"frame_id":50,"timestamp_us":1700000001000000,"ok":1}}
check "manual meta leaves state resolved" {[::ess::camera_grab_done]}

# ---- evt_info carries the CAMERA type ----
set tstart [string first "dict set evt_info CAMERA" $src]
check "CAMERA in evt_info" {$tstart > 0}
check "CAMERA subtypes declared" \
    {[string first "REQUEST 0 DONE 1 FAIL 2" $src] > 0}

# ---- file_open logs the camera points ----
check "camera/full logger match" \
    {[string first "dservLoggerAddMatch \$filename camera/full" $src] > 0}
check "camera/full/meta logger match" \
    {[string first "dservLoggerAddMatch \$filename camera/full/meta" $src] > 0}

puts ""
if {$nfail == 0} {
    puts "test_ess_camera: all checks passed ($npass)"
    exit 0
}
puts "test_ess_camera: $nfail of [expr {$npass + $nfail}] checks FAILED"
exit 1
