#
# test_private_logger.tcl
#
#  Exercise DSERV_DPOINT_PRIVATE_FLAG from the main tclserver interp:
#  - dpoint scripts (subscription delivery) never fire for private points
#  - the logger DOES receive private points, and the PRIVATE bit is
#    written into the .ds file's flags word
#
#  The log is verified by parsing the file bytes directly (no dlsh
#  needed): 16-byte header, then records of
#    varlen(u16) varname timestamp(u64) flags(u32) type(u32) len(u32) data
#
#  Run as: dserv --cscript tests/test_private_logger.tcl
#

set filename /tmp/test_private.ds
file delete $filename

puts "Start private logger test."

# subscription delivery: a dpoint script on the private name must never
# run; the public one must.  A (wrong) delivery of test/secret would
# print before the test/pub line and break the expected output.
proc got_dpoint { name data } {
    puts "DPOINT $name = $data"
}
dservAddExactMatch test/secret
dpointSetScript test/secret got_dpoint
dservAddExactMatch test/pub
dpointSetScript test/pub got_dpoint

# logger: subscribe to both points, then push one of each
dservLoggerOpen $filename 1
dservLoggerAddMatch $filename test/secret
dservLoggerAddMatch $filename test/pub
dservLoggerResume $filename

dservSetPrivate test/secret logged_secret
dservSet test/pub logged_public

# give the logger queue a moment to drain, then finish via done dpoint
after 50
dservAddExactMatch test/done
dpointSetScript test/done close_and_check
dservSet test/done 1

proc read_log_records { filename } {
    set f [open $filename rb]
    set data [read $f]
    close $f
    set records {}
    set pos 16
    set total [string length $data]
    while { $pos + 2 <= $total } {
        binary scan $data @${pos}su varlen
        incr pos 2
        set varname [string range $data $pos [expr {$pos + $varlen - 1}]]
        incr pos $varlen
        incr pos 8            ;# timestamp
        binary scan $data @${pos}iu flags
        incr pos 4
        incr pos 4            ;# type
        binary scan $data @${pos}iu len
        incr pos 4
        set val [string range $data $pos [expr {$pos + $len - 1}]]
        incr pos $len
        lappend records [list $varname $flags $val]
    }
    return $records
}

proc close_and_check { name data } {
    global filename
    dservLoggerClose $filename

    # the close flows through the logger queues; poll until both
    # records are on disk (or give up and let the checks report)
    for { set i 0 } { $i < 50 } { incr i } {
        after 40
        if { ![catch {read_log_records $filename} records] } {
            set names {}
            foreach r $records { lappend names [lindex $r 0] }
            if { [lsearch $names test/secret] >= 0 &&
                 [lsearch $names test/pub] >= 0 } break
        }
    }

    puts "Checking private log."
    set records [read_log_records $filename]
    foreach r $records {
        lassign $r varname flags val
        if { $varname eq "test/secret" } {
            set p [expr {($flags & 0x100) ? "private" : "public"}]
            puts "log secret: $p val=$val"
        } elseif { $varname eq "test/pub" } {
            set p [expr {($flags & 0x100) ? "private" : "public"}]
            puts "log public: $p val=$val"
        }
    }
    puts "Private logger test done."
    exit
}

puts "End private logger test."
