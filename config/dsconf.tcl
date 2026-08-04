puts "initializing dataserver"

set dspath [file dir [info nameofexecutable]]

set dlshlib [file join [file dirname $dspath] dlsh dlsh.zip]
if [file exists $dlshlib] {
    set base [file join [zipfs root] dlsh]
   zipfs mount $dlshlib $base
   set auto_path [linsert $auto_path [set auto_path 0] $base/lib]
}

package require dlsh
package require qpcs

tcl::tm::add $dspath/lib

# enable error logging
errormon enable

# look for any .tcl configs to run before other subprocesses local/*.tcl
foreach f [glob -nocomplain [file join $dspath local pre-*.tcl]] {
    source $f
}

# Offline / air-gapped mode. Opt in by creating the marker file
# local/offline (contents ignored) or exporting DSERV_OFFLINE=1 — one
# flag, checked here BEFORE any subprocess starts so every interp
# inherits it through the process environment. When set, https_get/
# https_post/https_put/https_delete (TclHttps) refuse any non-loopback
# host instantly — no DNS lookup, no connect timeout — and the
# registry/mesh/trial-ingest configs skip their network paths outright.
# Loopback targets stay allowed, so a registry or ingest server running
# on the box itself keeps working. Published as system/offline for UIs.
if { [file exists [file join $dspath local offline]] } {
    set env(DSERV_OFFLINE) 1
}
if { [info exists env(DSERV_OFFLINE)] &&
     $env(DSERV_OFFLINE) ne "" && $env(DSERV_OFFLINE) ne "0" } {
    dservSet system/offline 1
    puts "offline mode: outbound network disabled (local/offline or DSERV_OFFLINE)"
} else {
    dservSet system/offline 0
}

# start an eye movement subprocess
subprocess em "source [file join $dspath config/emconf.tcl]"

# start the analog-input subprocess (owns the MCP320x SPI driver)
subprocess ain "source [file join $dspath config/ainconf.tcl]"

# start the slider subprocess (calibrates ain channels into slider/position)
subprocess slider "source [file join $dspath config/sliderconf.tcl]"

# start the input subprocess (owns kernel input devices: touchscreen,
# and soon trackpad; publishes mtouch/event and friends)
subprocess input "source [file join $dspath config/inputconf.tcl]"

# start a juicer subprocess
subprocess juicer "source [file join $dspath config/juicerconf.tcl]"

# start the USB extio box subprocess (owns modules/usbio; bridges the box's 128-byte
# frames <-> datapoints, forwards ess/in_obs/config/cmd to the box, hot-swaps USB)
subprocess extio "source [file join $dspath config/extioconf.tcl]"

# start the PTP anchor subprocess -- pushes the PHC->dserv constant to every
# extio box so their timestamps share dserv's timeline (config/ptpconf.tcl).
#
# GATED on the host actually having a PHC, so this is a no-op on hardware that
# cannot do PTP at all (a Pi 4 has no /dev/ptpN, and neither does any Realtek
# NIC). Deliberately self-disabling rather than starting and erroring: the
# lesson from the juicer incident is that a subprocess which loads
# unconditionally will eventually run somewhere it should not.
if { [llength [glob -nocomplain /sys/class/ptp/ptp*]] } {
    subprocess ptp "source [file join $dspath config/ptpconf.tcl]"
}

# start a sound subprocess
subprocess sound "source [file join $dspath config/soundconf.tcl]"

# start a "git" interpreter
# RETIRED (superseded by the dserv.net registry): the git subprocess spent
# ~1.5s at boot running `git branch`/`current_tag`/etc. It published
# ess/git/{branch,tag,branches,commit,push,pull}; all consumers are guarded
# (dservExists) so absence is safe — only the git branch/commit UI goes blank.
# Re-enable this line if you still need git-based script management.
# subprocess git "source [file join $dspath config/gitconf.tcl]"

# start an openephys subprocess for handling state/recording
subprocess openephys "source [file join $dspath config/openephysconf.tcl]"

# start a "powermon"itor if found
subprocess powermon "source [file join $dspath config/powermonconf.tcl]"

# start our isolated ess thread
subprocess ess "source [file join $dspath config/essconf.tcl]"

# helper functions for our main interp
source [file join $dspath config/commands.tcl]

# add ability to call ess functions from main tclserver
source [file join $dspath config/essctrl.tcl]

# start the script-management subprocess (registry sync/push/preview/diff
# engine; keeps registry HTTP and tree hashing off the ess and main
# interps — see config/scriptsconf.tcl). Runs the deferred initial
# workgroup sync a few seconds after boot.
subprocess scripts "source [file join $dspath config/scriptsconf.tcl]"

# start a visualization process
subprocess viz "source [file join $dspath config/vizconf.tcl]"

# start config/sesssion process
subprocess configs "source [file join $dspath config/configsconf.tcl]"

# start datafile process
subprocess df "source [file join $dspath config/dfconf.tcl]"

# setup docs subprocess
subprocess docs "source [file join $dspath config/docsconf.tcl]"

# Discover this machine's LAN IP with NO external network dependency: a kernel
# route lookup (Linux `ip route get`, macOS `route get default` + ipconfig).
# Replaces a blocking connect to google.com:80 that added a DNS/socket hit at
# boot, hung with no internet, and returned a public IPv6 rather than the LAN
# IPv4 stim comms need. Route lookups send no packets and work offline.
proc get_local_hostaddr {} {
    set ip ""
    if { $::tcl_platform(os) == "Linux" } {
	catch {
	    if {[regexp {src (\S+)} [exec ip -4 route get 1.1.1.1] -> m]} { set ip $m }
	}
    } elseif { $::tcl_platform(os) == "Darwin" } {
	catch {
	    set iface [exec sh -c {route -n get default 2>/dev/null | awk '/interface:/{print $2; exit}'}]
	    if { $iface ne "" } { set ip [string trim [exec ipconfig getifaddr $iface]] }
	}
    }
    return $ip
}

proc set_hostinfo {} {
    # set IP addresses for use in stim communication
    if { [dservGet ess/ipaddr] == "" } {
	dservSet ess/ipaddr 127.0.0.1
    }

    # set host address to identify this machine (LAN IPv4; loopback fallback)
    set addr [get_local_hostaddr]
    if { $addr eq "" } {
	set addr 127.0.0.1
    }
    dservSet system/hostaddr $addr

    if { $::tcl_platform(os) == "Darwin" } {
	set name [exec scutil --get ComputerName]
    } elseif { $::tcl_platform(os) == "Linux" } {
	set name [exec hostname]
    } else {
	set name $::env(COMPUTERNAME)
    }
    dservSet system/hostname $name
    dservSet system/os $::tcl_platform(os)
}

set_hostinfo

set host [dservGet system/hostaddr]

# Start subprocess to provide connection to stim
subprocess stim "source [file join $dspath config/stimconf.tcl]"

# Start subprocess to handle ess/* datapoint forwarding
subprocess mesh "source [file join $dspath config/meshconf.tcl]"

# start sqlite local db
subprocess db 2571 "source [file join $dspath config/sqliteconf.tcl]"

# start trial sync process to send to cloud
subprocess trialsync "source [file join $dspath config trialsyncconf.tcl]"

# start a virtual eye subprocess
subprocess virtual_eye "source [file join $dspath config/virtualeyeconf.tcl]"

# start a virtual slider subprocess (analog input simulator)
subprocess virtual_slider "source [file join $dspath config/virtualsliderconf.tcl]"

# if we have camera support (libcamera), start a camera process
if { [file exists $dspath/modules/dserv_camera[info sharedlibextension]] } {
    subprocess camera "source [file join $dspath config/cameraconf.tcl]"
}


