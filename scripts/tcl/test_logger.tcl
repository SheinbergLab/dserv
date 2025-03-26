#
# test_logger.tcl
#
#  Open log, write test points, close log, read back and convert to json
#
#  Can be run as:
#     /usr/local/dserv/dserv -c ./test_logger.tcl
#
#  To have this script directly close and process after writing out dpoints
#    we set a datapoint called "done" that triggers the close_and_process step
#
#

# add dlsh.zip to library path, so packages can be loaded into dserv
set dspath [file dir [info nameofexecutable]]
set dlshlib [file join [file dirname $dspath] dlsh dlsh.zip]
if [file exists $dlshlib] {
    set base [file join [zipfs root] dlsh]
    zipfs mount $dlshlib $base
    set auto_path [linsert $auto_path [set auto_path 0] $base/lib]
}

# the dslog::read function is found in the dslog package
package require dlsh
package require dslog

# open a test log at /tmp/testlog.ds (the 1 means overwrite)
set filename /tmp/testlog.ds
dservLoggerOpen $filename 1

# have logger listen for dpoints matching foo/*
dservLoggerAddMatch $filename foo/*

# start logging (newly opened loggers start paused
dservLoggerResume $filename

# push out 100 dpoints that should match
set dpoint_count 100
proc do_test {} {
    global dpoint_count
    for { set i 0 } { $i < $dpoint_count } { incr i } {
	dservSet foo/[expr $i/10] $i
    }
    dservSet done 1
}

set dpoint done

# attach callback script to timer dpoint
dservAddExactMatch $dpoint
dpointSetScript $dpoint close_and_process

# proc to close the logger and process result
proc close_and_process { name val } {
    global filename
    global dpoint_count

    # close logger
    dservLoggerClose $filename

    # read the log into a dg
    set dg [dslog::read $filename]

    # check the log contents vs what we pushed in above
    puts "Checking log contents."

    # we pushed in dpoint_count log entries
    # we expect one additional entry from opening the log itself
    set expected_count [expr $dpoint_count + 1]
    set varname_count [dl_length $dg:varname]
    puts "Log contains $varname_count varnames (expecting $expected_count)"
    puts "varname 0 is [dl_get $dg:varname 0] (expecting logger:open)"

    # we pushed in varnames like foo/n
    for { set i 1 } { $i < $varname_count } { incr i } {
        set varname [dl_get $dg:varname $i]
        set expected_varname foo/[expr ($i-1)/10]
        if {![string match $varname $expected_varname]} {
            puts "!!varname $i is $varname but expected $expected_varname"
        }
    }
    puts "Checked $varname_count varnames."

    # we pushed in values like n
    set vals_count [dl_length $dg:vals]
    puts "Log contains $vals_count vals (expecting $expected_count)"
    puts "vals 0 is [dl_get $dg:vals:0 0] (expecting 3)"
    for { set i 1 } { $i < $vals_count } { incr i } {
        set val [dl_get $dg:vals:$i 0]
        set expected_val [expr $i-1]
        if {$val != $expected_val} {
            puts "!!val $i is $val but expected $expected_val"
        }
    }
    puts "Checked $vals_count vals."

    exit
}

do_test

