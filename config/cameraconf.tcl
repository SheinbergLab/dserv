#
# Process camera commands
#

set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

tcl::tm::add $dspath/lib

# load camera module (was also loaded in main interp, but here we use it)
load [file join $dspath modules/dserv_camera[info sharedlibextension]]

proc init {} {
    puts [cameraStatus]
}

# Initialize the camera setup
init

