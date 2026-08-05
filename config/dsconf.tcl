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

# Boot breadcrumbs. dsconf.tcl runs as ONE Tcl_Eval, so a script-level error
# anywhere in it silently truncates the boot: everything below that line never
# runs, and the only evidence is which subprocesses are missing from
# `subprocessInfo`. These two datapoints make that self-reporting:
#
#   system/boot_stage      last thing dsconf.tcl attempted
#   system/boot_complete   0 until dsconf.tcl reaches its last line, then 1
#
# So `dservGet system/boot_complete` answers "did the boot finish?" and
# system/boot_stage names the exact step it died on. The subprocess wrapper
# below only records while booting, so on-demand spawns later (virtual_subject,
# virtual_extio, -link children) leave the post-mortem breadcrumb intact.
dservSet system/boot_complete 0
dservSet system/boot_stage    start
set ::dsconf_booting 1

# The stderr line matters as much as the datapoint. subprocess config scripts
# run SYNCHRONOUSLY (subprocess_command -> child->eval blocks on the reply
# queue), so a config that blocks on hardware -- juicer serial, extio USB,
# ALSA, a stim2 connect -- hangs dsconf.tcl forever. In that state the main
# interp never returns to its request loop, so ports 2560/2565/2570 accept
# connections but answer nothing: the breadcrumb DATAPOINTS are unreadable
# precisely when they would be most useful. The journal is the only channel
# that still works, so mirror there. Last "dsconf: starting X" in the journal
# with no matching "ok" names the subprocess that hung.
rename subprocess _subprocess
proc subprocess {args} {
    if { ![info exists ::dsconf_booting] } {
	return [uplevel 1 [linsert $args 0 _subprocess]]
    }
    set who [lindex $args 0]
    dservSet system/boot_stage "subprocess $who"
    puts stderr "dsconf: starting $who"
    flush stderr
    set rc [catch { uplevel 1 [linsert $args 0 _subprocess] } result options]
    puts stderr "dsconf: started $who [expr {$rc ? "FAILED: $result" : "ok"}]"
    flush stderr
    return -options $options $result
}

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
    # set host address to identify this machine (LAN IPv4; loopback fallback)
    set addr [get_local_hostaddr]
    if { $addr eq "" } {
	set addr 127.0.0.1
    }
    dservSet system/hostaddr $addr

    # ess/ipaddr is the address ESS hands to stim2 over rmt --
    #     rmtSend "set dservhost [dservGet ess/ipaddr]"   (lib/ess-2.0.tm)
    # -- so stim2 knows where to send its datapoints BACK. Three cases, in
    # precedence order:
    #
    #  1. $env(ESS_IPADDR) set (local/pre-remote.tcl): essconf.tcl already
    #     seeded ess/ipaddr from it. Explicit rig declaration; never override.
    #  2. $env(ESS_RMT_HOST) names another machine (config/stimconf.tcl uses
    #     the same variable to locate stim2): stim2 is REMOTE, so it must be
    #     handed our LAN address. Loopback here tells a remote stim2 to send
    #     datapoints to ITSELF, where they vanish with no error on either
    #     side -- such rigs only worked because ESS_IPADDR was set by hand.
    #  3. otherwise stim2 is local: 127.0.0.1.
    #
    # Case 3 stays on loopback deliberately. Routing-wise it makes no odds --
    # the kernel resolves this box's own LAN address to `dev lo` (verified:
    # `ip route get <own addr>` -> "local ... dev lo"), and 30k round trips
    # through dserv's port measured identical (median 28 us either way, minima
    # within 0.1 us). But loopback survives a downed link, and the LAN address
    # does not: pull the cable and the local route disappears with it, taking
    # a LOCAL stim2's connection with it. No reason to make the common case
    # depend on the network being up.
    #
    # dservExists guards the read because ess/ipaddr is owned by a DIFFERENT
    # subprocess, and a bare dservGet of an absent dpoint raises a Tcl error
    # (Dataserver.cpp dserv_get_command). That used to abort this whole script
    # -- taking stim/mesh/db/trialsync/virtual_eye/virtual_slider/camera with
    # it -- whenever essconf.tcl died before reaching its own `dservSet
    # ess/ipaddr`. Same "one rogue subprocess kills dsconf" failure the
    # subprocess_command fix closed; it survived there because this coupling
    # runs through a datapoint read rather than the subprocess return code.
    set stim_addr 127.0.0.1
    if { [info exists ::env(ESS_RMT_HOST)] } {
	set rmt $::env(ESS_RMT_HOST)
	if { $rmt ne "" && $rmt ne "localhost" &&
	     ![string match 127.* $rmt] && $rmt ne $addr } {
	    set stim_addr $addr
	}
    }
    if { ![dservExists ess/ipaddr] || [dservGet ess/ipaddr] eq "" } {
	dservSet ess/ipaddr $stim_addr
    }

    # Every branch here is wrapped, for the same reason get_local_hostaddr
    # wraps its own execs: `exec` FORKS, and this process is large, heavily
    # threaded and mlockall()ed under RT scheduling. A fork that fails (or a
    # tool that is missing, or COMPUTERNAME being unset on a non-Darwin,
    # non-Linux host) raised a bare Tcl error on the boot path and truncated
    # dsconf.tcl right here -- losing stim/mesh/db/trialsync/virtual_eye/
    # virtual_slider/camera. A cosmetic datapoint must never be able to do
    # that; fall back to the kernel hostname file, then to "unknown".
    set name ""
    catch {
	if { $::tcl_platform(os) == "Darwin" } {
	    set name [exec scutil --get ComputerName]
	} elseif { $::tcl_platform(os) == "Linux" } {
	    set name [exec hostname]
	} else {
	    set name $::env(COMPUTERNAME)
	}
    }
    if { $name eq "" } {
	catch {
	    set f [open /proc/sys/kernel/hostname r]
	    set name [string trim [read $f]]
	    close $f
	}
    }
    if { $name eq "" } { set name unknown }
    dservSet system/hostname $name
    dservSet system/os $::tcl_platform(os)
}

dservSet system/boot_stage set_hostinfo
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

# dsconf.tcl ran to completion -- see the boot breadcrumb comment at the top.
unset ::dsconf_booting
dservSet system/boot_stage    done
dservSet system/boot_complete 1


