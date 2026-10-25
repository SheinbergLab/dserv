# fake_stim2.tcl -- a tiny stand-in for stim2's command port, for
# tests/test_ess_stim_cmd.tcl. Plain tclsh (it needs the event loop the
# dserv interps do not run), started by the test as a child process:
#
#     tclsh fake_stim2.tcl <portfile>
#
# Listens on an ephemeral port, writes the port number to <portfile>, then
# speaks stim2's framing: 4-byte big-endian length, payload; reply framed
# the same way. Payload is UTF-8 Tcl text, as the rmt module sends it.
#
# Replies:
#   stimAckHeader          "1" while ack mode is on, else the error string a
#                          stim2 without the command would return
#   @<cmd>            header + echo:  "ACK1 100.000 101.500 42 <flip>\n<echo>"
#                          (<flip> is 0 until "fake_flip <n>" sets it; -1 is
#                          what stim2 sends when its swapsync strategy has
#                          no hardware flip time)
#                          (in raw-echo mode: the echo alone, header omitted,
#                          to stand in for a stim2 that ignores the prefix)
#   <cmd>                  the echo alone
# where <echo> of "error <msg>" is "!TCL_ERROR <msg>" (a failing remote
# command) and of anything else is the command text itself -- except the
# stimess package's two commands (the stim2 side of the stim_bind block),
# answered as a stim2 with the package would while stimess mode is on:
#   stim::bindings_json    the fixed table in ::bindings_json (two bindings,
#                          ess/dial/pointer and planko/catcher)
#   catch {stim::unbind_all}   "0" -- what Tcl's catch returns when the
#                          command exists
# and as a stim2 WITHOUT the package after "fake_stimess 0":
#   stim::bindings_json    "!TCL_ERROR invalid command name ..."
#   catch {stim::unbind_all}   "1"
# (the ACK1 header still applies to both when requested).
#
# Control commands (never echoed): "fake_ack 0|1", "fake_rawecho 0|1",
# "fake_flip <n>", "fake_stimess 0|1", "fake_bindings <json>" (replaces the
# table; "fake_bindings" alone restores the default), "fake_quit". The
# process also exits on its own after 60 s so a crashed test cannot leave it
# behind.

set portfile [lindex $argv 0]
set ::ack 1
set ::rawecho 0
set ::flip 0
set ::stimess 1
set ::bindings_default {{"package":"stimess 1.0","bindings":[{"name":"ess/dial/pointer","fields":["x","y","show","band"],"mode":"latest","sep":","},{"name":"planko/catcher","fields":["x","y"],"mode":"latest","sep":","}]}}
set ::bindings_json $::bindings_default

proc reply_for { cmd } {
    set hdr 0
    if { [string index $cmd 0] eq "@" } {
        set hdr 1
        set cmd [string range $cmd 1 end]
    }
    switch -glob -- $cmd {
        "fake_ack *"     { set ::ack [lindex $cmd 1]; return "" }
        "fake_rawecho *" { set ::rawecho [lindex $cmd 1]; return "" }
        "fake_flip *"    { set ::flip [lindex $cmd 1]; return "" }
        "fake_stimess *" { set ::stimess [lindex $cmd 1]; return "" }
        "fake_bindings"  { set ::bindings_json $::bindings_default; return "" }
        "fake_bindings *" { set ::bindings_json [string range $cmd 14 end]; return "" }
        "fake_quit"      { after 50 exit; return "" }
        "stimAckHeader"  {
            if { $::ack } { return 1 }
            return "!TCL_ERROR invalid command name \"stimAckHeader\""
        }
    }
    if { [string match "error *" $cmd] } {
        set echo "!TCL_ERROR [string range $cmd 6 end]"
    } elseif { [string trim $cmd] eq "stim::bindings_json" } {
        if { $::stimess } {
            set echo $::bindings_json
        } else {
            set echo "!TCL_ERROR invalid command name \"stim::bindings_json\""
        }
    } elseif { [string trim $cmd] eq "catch {stim::unbind_all}" } {
        set echo [expr {$::stimess ? 0 : 1}]
    } else {
        set echo $cmd
    }
    if { $hdr && !$::rawecho } {
        return "ACK1 100.000 101.500 42 $::flip\n$echo"
    }
    return $echo
}

proc serve { chan } {
    set lenbytes [read $chan 4]
    if { [string length $lenbytes] < 4 } { catch { close $chan }; return }
    binary scan $lenbytes Iu len
    set payload ""
    if { $len > 0 } {
        set payload [read $chan $len]
        if { [string length $payload] < $len } { catch { close $chan }; return }
    }
    set cmd [encoding convertfrom utf-8 $payload]
    set out [encoding convertto utf-8 [reply_for $cmd]]
    puts -nonewline $chan [binary format Iu [string length $out]]
    puts -nonewline $chan $out
    flush $chan
}

proc accept { chan addr port } {
    fconfigure $chan -translation binary -buffering none -blocking 1
    fileevent $chan readable [list serve $chan]
}

set srv [socket -server accept 0]
set port [lindex [fconfigure $srv -sockname] 2]
set f [open $portfile.tmp w]
puts $f $port
close $f
file rename -force $portfile.tmp $portfile

after 60000 exit
vwait forever
