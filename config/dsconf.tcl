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

# configure our mesh dispatcher (started on 2575 if enabled)
if { [info exists ::mesh_enabled] } {
    puts "Mesh networking detected - loading mesh configuration"
     send mesh "source [file join $dspath config/meshconf.tcl]"
}

# start our isolated ess thread
subprocess ess "source [file join $dspath config/essconf.tcl]"

# helper functions for our main interp
source [file join $dspath config/commands.tcl]

# add ability to call ess functions from main tclserver
source [file join $dspath config/essctrl.tcl]

# start a visualization process
subprocess viz "source [file join $dspath config/vizconf.tcl]"

proc set_hostinfo {} {
    # target_host allows us to connect using NIC
    set target_host google.com
    
    # set IP addresses for use in stim communication
    if { [dservGet ess/ipaddr] == "" } {
	dservSet ess/ipaddr 127.0.0.1
    }
    
    # set host address to identify this machine
    set s [socket $target_host 80]
    dservSet system/hostaddr [lindex [fconfigure $s -sockname] 0]
    close $s

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

# start sqlite local db
subprocess db 2571 "source [file join $dspath config/sqliteconf.tcl]"

# homebase computers use postgresql
set hbs "192.168.4.100 192.168.4.101 192.168.4.102 192.168.4.103 192.168.4.104 192.168.4.201"
set rigs "192.168.88.40"
if { [lsearch $hbs $host] >= 0 } {
    subprocess pg 2572 "source [file join $dspath config/postgresconf.tcl]"
} elseif { [lsearch $rigs $host] >= 0 } {
    subprocess pg 2572 "source [file join $dspath config/central_postgresconf.tcl]"
}

# start a "git" interpreter
subprocess git 2573 "source [file join $dspath config/gitconf.tcl]"

# auto update support
source [file join $dspath config/updateconf.tcl]

# if we have camera support (libcamera), start a camera process
load [file join $dspath modules/dserv_camera[info sharedlibextension]]
if { [dict get [cameraStatus libcamera]] == "yes" } {
    subprocess camera "source [file join $dspath config/camconf.tcl]"
}

  
