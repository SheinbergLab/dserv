#
# test_logger.tcl
#
#  Open log, write test points, close log, read back and convert to json
#
#  Can be run as:
#     /usr/local/dserv/dserv -c ./test_logger.tcl
#
#   Note that because the logger runs in its own thread, it is difficult
#  to have this script directly close and process after writing out dpoints
#
#   We use the timer module, to attach a script to run after 500ms, which
#  closes the log, processes it, and then exits dserv
#

puts "Begin logger test"

# add dlsh.zip to library path, so packages can be loaded into dserv
set dspath [file dir [info nameofexecutable]]
set dlshlib [file join [file dirname $dspath] dlsh dlsh.zip]
if [file exists $dlshlib] {
    set base [file join [zipfs root] dlsh]
    zipfs mount $dlshlib $base
    set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

    puts "Added dlsh.zip to library path"
}

# the dslog::read function is found in the dslog package
package require dlsh
puts "Require dlsh OK"
#package require dslog
#puts "Require dslog OK"

# open a test log at /tmp/testlog.ds (the 1 means overwrite)
set filename /tmp/testlog.ds
dservLoggerOpen $filename 1

# have logger listen for dpoints matching foo/*
dservLoggerAddMatch $filename foo/*

# start logging (newly opened loggers start paused
dservLoggerResume $filename

# push out 100 dpoints that should match
for { set i 0 } { $i < 100 } { incr i } { dservSet foo/[expr $i/10] $i }

#######################################################
#  use timer callback to trigger close log and process
#######################################################

# load timer module for timerTick function
load [file join $dspath modules dserv_timer[info sharedlibextension]]

# user timer 0, expiration will set dpoint named timer/0
set dpoint timer/0

# attach callback script to timer dpoint
dservAddExactMatch $dpoint
dpointSetScript $dpoint close_and_process

# wait 500ms, which should be plenty to allow log to be flushed
timerTick 0 500

# proc to close the logger and process result
proc close_and_process { name val } {
    global filename

    # close logger
    dservLoggerClose $filename

    # read the log into a dg
    #set dg [dslog::read $filename]
    
    # convert the dg to JSON and print
    #puts [dg_toJSON $dg]

    # temporary workaround for dslog loading trouble
    set logSize [file size $filename]
    puts "Log file size $logSize"

    exit
}


