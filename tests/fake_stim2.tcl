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
# The per-frame record command (the stim2 side of the stim_frame_log
# block), while frame mode is on -- EVERY form answers the status dict a
# real stim2 answers, {enabled <0|1> host <host> sent <n> dropped <n>
# connected <0|1>} (sent/dropped stay 0 here, the fake never pushes a
# frame; connected is 0 in the reply to a start, as in stim2, whose writer
# thread connects asynchronously, and mirrors enabled afterwards):
#   stimFrameLog <host> 1  enabled 1, remembers the host, counts the
#                          enables (::frame_enables)
#   stimFrameLog <host> 0  enabled 0, counts the disables
#   stimFrameLog           the status (stim2's probe form)
# and after "fake_frame 0", all three: "!TCL_ERROR invalid command name
# \"stimFrameLog\"" (a stim2 without the command). "fake_frame_state"
# answers {enabled <0|1> host <host> enables <n> disables <n>} for the
# test's own bookkeeping, in either mode.
#
# The version query (the stim2 side of the stim_script block):
#   stimVersion            "0.27.3" (the release string, stim2 >= 0.27.3), or
#                          after "fake_stimversion 0" (a 0.27.x build) or
#                          "fake_version 0" the "invalid command name" error
#   set StimVersion        "20" (stim2's Tcl-linked version int), or after
#                          "fake_version 0" the "can't read" error a stim2
#                          without the variable would give
#
# Control commands (never echoed): "fake_ack 0|1", "fake_rawecho 0|1",
# "fake_flip <n>", "fake_stimess 0|1", "fake_bindings <json>" (replaces the
# table; "fake_bindings" alone restores the default), "fake_frame 0|1",
# "fake_frame_state", "fake_version 0|1", "fake_stimversion 0|1",
# "fake_quit". The process also
# exits on its own after 60 s so a crashed test cannot leave it behind.

set portfile [lindex $argv 0]
set ::ack 1
set ::rawecho 0
set ::flip 0
set ::stimess 1
set ::bindings_default {{"package":"stimess 1.0","bindings":[{"name":"ess/dial/pointer","fields":["x","y","show","band"],"mode":"latest","sep":","},{"name":"planko/catcher","fields":["x","y"],"mode":"latest","sep":","}]}}
set ::bindings_json $::bindings_default
set ::frame 1
set ::frame_enabled 0
set ::frame_host {}
set ::frame_enables 0
set ::frame_disables 0
set ::version 1
set ::stimversion 1

# stimFrameLog ?<host> 0|1?, as a stim2 with the command answers it
proc frame_status { connected } {
    return [list enabled $::frame_enabled host $::frame_host \
                sent 0 dropped 0 connected $connected]
}
proc frame_log { args } {
    if { !$::frame } {
        return "!TCL_ERROR invalid command name \"stimFrameLog\""
    }
    if { [llength $args] == 0 } { return [frame_status $::frame_enabled] }
    if { [llength $args] != 2 } {
        return "!TCL_ERROR wrong # args: should be \"stimFrameLog ?host 0|1?\""
    }
    lassign $args host flag
    set ::frame_host $host
    if { $flag } {
        set ::frame_enabled 1
        incr ::frame_enables
        # a start answers before the writer thread has connected
        return [frame_status 0]
    }
    set ::frame_enabled 0
    incr ::frame_disables
    return [frame_status 0]
}

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
        "fake_frame *"   { set ::frame [lindex $cmd 1]; return "" }
        "fake_frame_state" {
            return [list enabled $::frame_enabled host $::frame_host \
                        enables $::frame_enables disables $::frame_disables]
        }
        "fake_quit"      { after 50 exit; return "" }
        "stimAckHeader"  {
            if { $::ack } { return 1 }
            return "!TCL_ERROR invalid command name \"stimAckHeader\""
        }
        "fake_version *" { set ::version [lindex $cmd 1]; return "" }
        "fake_stimversion *" { set ::stimversion [lindex $cmd 1]; return "" }
        "stimVersion" {
            # stim2 >= 0.27.3: the release string. A 0.27.x build has only
            # StimVersion (fake_stimversion 0); an older one has neither
            # (fake_version 0).
            if { $::version && $::stimversion } { return 0.27.3 }
            return "!TCL_ERROR invalid command name \"stimVersion\""
        }
        "set StimVersion" {
            # stim2's Tcl-linked version int (the stim_script block's
            # version query; raw, so never under a header)
            if { $::version } { return 20 }
            return "!TCL_ERROR can't read \"StimVersion\": no such variable"
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
    } elseif { [string trim $cmd] eq "stimFrameLog" } {
        set echo [frame_log]
    } elseif { [string match "stimFrameLog *" $cmd] } {
        # (string match first: an arbitrary command text need not be a list)
        set echo [frame_log {*}[lrange $cmd 1 end]]
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
