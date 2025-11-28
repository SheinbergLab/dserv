#
# Process camera commands
#

# Get access to module libraries
set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

tcl::tm::add $dspath/lib

# load camera module (was also loaded in main interp, but here we use it)
load [file join $dspath modules/dserv_camera[info sharedlibextension]]

# see /usr/local/dserv/local/camera.tcl.EXAMPLE for example configuration

# enable error logging
errormon enable

# allow local override for this system
set localconf [file join  $dspath local camera.tcl]
if { [file exists $localconf] } {
    source $localconf
}


