# test_ess_stim_cmd.tcl -- the rmtSend wrapper that records every command
# sent to stim2 as a stim/cmd JSON datapoint (lib/ess-2.0.tm, the block
# between "# BEGIN stim_cmd_log" and "# END stim_cmd_log"), and the stim2
# binding table it publishes as stim/bind (the "# BEGIN stim_bind" block:
# stim_bind_publish, the query-and-publish step configure_stim calls).
#
# Run by ctest as:
#     dserv -c tests/test_ess_stim_cmd.tcl
# with DSERV_RMT_MODULE (the built dserv_rmt module) and ESS_TEST_TCLSH (a
# tclsh for the fake stim2) in the environment.
#
# What is real: the rmt module (its socket, framing and failure handling),
# dserv's `now`, `dservSetData`, `sha256` and datapoint table, and the
# wrapper's own code, evaluated from the source file. What is fake: stim2
# (tests/fake_stim2.tcl, a child tclsh speaking the length-prefixed
# protocol). Not covered: configure_stim's calls of the probe, of
# `catch {stim::unbind_all}` and of stim_bind_publish, and open_datafile's
# logger matches -- those lines are read from the source below instead,
# since the whole of ess-2.0.tm needs dlsh and a systems tree; the
# ess_stim_source test exercises configure_stim itself. The stim_bind
# parser is tcljson (dlsh.zip): where the installed dlsh.zip exists it is
# mounted and the real parser runs; elsewhere (CI) the documented regexp
# fallback does, and the test says which.
#
# ONE verdict line, as the other dserv-hosted tests: listener bind-retry
# noise from a live dserv on the same box interleaves with output.

puts "Start stim cmd test."

set here [file dirname [file normalize [info script]]]
set repo [file dirname $here]
set failures {}
proc assert { ok what } {
    if { [uplevel 1 [list expr $ok]] } { puts "  ok: $what" } else {
        puts "  FAIL: $what"
        lappend ::failures $what
    }
}
# An error anywhere must still end in the verdict line AND in the process
# ending: `exit` is refused in the main dserv interp and there is no Tcl
# shutdown command, so ask for the clean signal path (dserv.cpp
# signalHandler -> graceful_shutdown), which main() honours as soon as
# this config script returns. Without it the test only ends when ctest's
# TIMEOUT kills it (or, on a box with a live dserv, when the listener
# gives up its port fight after 30 s).
proc finish {} {
    catch { ::ess::rmtSend_raw fake_quit }
    if { [llength $::failures] == 0 } {
        puts "STIM CMD TEST: ALL PASS"
    } else {
        puts "STIM CMD TEST: [llength $::failures] FAILURE(S): $::failures"
    }
    flush stdout
    catch { exec kill -TERM [pid] }
}

proc run_tests {} {
    global here repo
# --- the rmt module, in THIS interp -------------------------------------
set module $::env(DSERV_RMT_MODULE)
if { [catch { load $module } err] } {
    puts "  FAIL: cannot load rmt module $module: $err"
    lappend ::failures "load rmt module"
    return
}
assert {[info commands ::rmtSend] ne ""} "rmt module provides rmtSend"

# --- the wrapper block, straight from the source -------------------------
set f [open [file join $repo lib ess-2.0.tm]]
set src [read $f]
close $f
set b [string first "# BEGIN stim_cmd_log" $src]
set e [string first "# END stim_cmd_log" $src]
assert {$b >= 0 && $e > $b} "found the stim_cmd_log block in lib/ess-2.0.tm"
namespace eval ess {
    proc ess_warning { msg {cat system} } { puts "  (ess_warning: $msg)" }
}
namespace eval ess [string range $src $b [expr {$e - 1}]]
assert {[info procs ::rmtSend] ne ""} "rmtSend is now the wrapper proc"
assert {[info commands ::ess::rmtSend_raw] ne ""} "the C command was renamed to ::ess::rmtSend_raw"

# the two call sites outside the block
assert {[string first "::ess::stim_cmd_probe" $src] > $e} "configure_stim calls the probe"
assert {[string first "dservLoggerAddMatch \$filename stim/cmd 0 0 1" $src] > 0} \
    "open_datafile subscribes stim/cmd unbuffered, not obs-limited, every record"

# --- the stim_bind block, same way --------------------------------------
# its parser, tcljson, lives in dlsh.zip: mount the installed one where
# there is one (as config/dsconf.tcl does) so the real parser is what runs
# here; without it the block falls back to its regexp extraction
set dlshzip /usr/local/dlsh/dlsh.zip
# (ESS_TEST_NO_DLSH=1 in the environment skips the mount, to run the
# fallback path on a machine that has dlsh)
if { [file exists $dlshzip] && ![info exists ::env(ESS_TEST_NO_DLSH)] } {
    catch {
        set base [file join [zipfs root] dlsh]
        zipfs mount $dlshzip $base
        set ::auto_path [linsert $::auto_path 0 [file join $base lib]]
    }
}
set bb [string first "# BEGIN stim_bind" $src]
set be [string first "# END stim_bind" $src]
assert {$bb > $e && $be > $bb} "found the stim_bind block in lib/ess-2.0.tm"
namespace eval ess [string range $src $bb [expr {$be - 1}]]
assert {[info procs ::ess::stim_bind_publish] ne ""} "stim_bind_publish defined"
assert {$::ess::stim_bound_names eq {}} "stim_bound_names starts empty"
set ::stim_bind_parser [expr {[catch {package require tcljson}] ? "regexp fallback" : "tcljson"}]
puts "  (stim_bind json parser here: $::stim_bind_parser)"

# the call sites outside the block: configure_stim unbinds right after the
# probe and before the stim script goes out, publishes the empty table
# until the script has been asked (twice: not connected, and pre-source),
# and asks once after the script section; open_datafile matches the table
# and every bound stream, and re-emits the table once the logger runs
set unbind_at [string first "rmtSend {catch {stim::unbind_all}}" $src $be]
set script_at [string first "set result \[rmtSend \$script\]" $src]
assert {$unbind_at > [string first "::ess::stim_cmd_probe" $src] && $unbind_at < $script_at} \
    "configure_stim sends catch {stim::unbind_all} after the probe, before the stim script"
assert {[regexp -all {::ess::stim_bind_publish 0\n} $src] == 2} \
    "configure_stim publishes the empty table when not connected and before sourcing"
assert {[regexp -all {\n\s+::ess::stim_bind_publish\n} $src] == 1 &&
        [string first "\n        ::ess::stim_bind_publish\n" $src] > $script_at} \
    "configure_stim queries stim2 once, after the stim script section"
assert {[string first "dservLoggerAddMatch \$filename stim/bind 0 0 1" $src] > 0} \
    "open_datafile subscribes stim/bind unbuffered, not obs-limited, every record"
assert {[string first "dservLoggerAddMatch \$filename \$_bound 1 0 1" $src] > 0} \
    "open_datafile subscribes each bound stream obs-limited, unbuffered, every record"
assert {[string first "dservTouch stim/bind" $src] > [string first "dservLoggerResume \$filename" $src]} \
    "open_datafile re-emits stim/bind after the logger is running"

# --- the fake stim2 -------------------------------------------------------
set tclsh $::env(ESS_TEST_TCLSH)
set portfile [file join [pwd] fake_stim2_[pid].port]
file delete -force $portfile
set fake [exec $tclsh [file join $here fake_stim2.tcl] $portfile &]
for { set i 0 } { $i < 100 && ![file exists $portfile] } { incr i } { after 50 }
if { ![file exists $portfile] } {
    puts "  FAIL: fake stim2 did not start"
    lappend ::failures "fake stim2 start"
    return
}
set pf [open $portfile]
set port [string trim [read $pf]]
close $pf
file delete -force $portfile

assert {[rmtOpen localhost $port] == 1} "connected to the fake stim2 on port $port"

# JSON field accessors on the record text. The record is pure ASCII by
# construction (json_escape), so a regexp over it is sound.
proc rec {} { return [dservGet stim/cmd] }
proc field { rec name } {
    if { [regexp "\"$name\":(\"(?:\[^\"\\\\\]|\\\\.)*\"|\[^,\}\]+)" $rec -> v] } { return $v }
    return "<absent>"
}

# --- 1. new stim2: header requested, stripped, recorded ------------------
assert {[::ess::stim_cmd_probe] == 1} "probe: fake answers stimAckHeader with 1"
assert {$::ess::stim_ack_header == 1} "stim_ack_header set"

set r [rmtSend "show_stim 3"]
assert {$r eq "show_stim 3"} "caller gets the bare echo, no header"
set j [rec]
assert {[field $j seq] == 1} "seq 1 ([field $j seq])"
assert {[field $j cmd] eq "\"show_stim 3\""} "cmd recorded without the prefix"
assert {[field $j status] eq "\"ok\""} "status ok"
assert {[field $j dq] == 100.0} "dq 100.0 ([field $j dq])"
assert {[field $j done] == 101.5} "done 101.5 ([field $j done])"
assert {[field $j swap] == 42} "swap 42"
assert {[field $j flip] eq "null"} "flip null when stim2 sent 0 (not swap-acked)"
set ts [field $j t_send]; set tr [field $j t_ret]
assert {[string is wideinteger -strict $ts] && [string is wideinteger -strict $tr] && $tr >= $ts} \
    "t_send/t_ret are integers, t_ret >= t_send"
assert {[dservTimestamp stim/cmd] == $tr} "datapoint timestamp is t_ret"

# a '!' (swap-acked) command: prefix goes in front of the '!'
set r [rmtSend "!flip_now"]
assert {$r eq "!flip_now"} "swap-acked command: echo intact"
assert {[field [rec] cmd] eq "\"!flip_now\""} "swap-acked command recorded as sent"
assert {[field [rec] seq] == 2} "seq advances"

# flip: -1 (a swapsync strategy without a hardware flip time) is not a
# timestamp either; a positive value is stored as given
rmtSend "fake_flip -1"
set r [rmtSend "show_stim 5"]
assert {$r eq "show_stim 5"} "negative flip: header still stripped"
set j [rec]
assert {[field $j flip] eq "null"} "flip null when stim2 sent -1 ([field $j flip])"
assert {[field $j status] eq "\"ok\""} "status ok with flip -1"
assert {[field $j swap] == 42} "other header fields intact with flip -1"
rmtSend "fake_flip 1761350400123456"
rmtSend "!flip_timed"
assert {[field [rec] flip] == 1761350400123456} "a real flip time is stored as given"
rmtSend "fake_flip 0"

# an empty result under the header is still an empty result to the caller
set r [rmtSend "fake_ack 1"]
assert {$r eq ""} "empty remote result stays empty"
assert {[field [rec] status] eq "\"ok\""} "empty result with header is ok, not io"

# --- 2. remote error ------------------------------------------------------
set r [rmtSend "error boom"]
assert {$r eq "!TCL_ERROR boom"} "caller gets the !TCL_ERROR string"
assert {[field [rec] status] eq "\"error\""} "status error"
assert {[field [rec] dq] == 100.0} "header still parsed on an error reply"

# --- 3. escaping ----------------------------------------------------------
set weird "say \"hi\"\\ tab\there\nnl é ☃ \U0001F600 \x01"
set r [rmtSend $weird]
assert {$r eq $weird} "odd characters round-trip to the caller"
set j [rec]
# (built with doubled backslashes in a plain `set`: expr backslash-
# substitutes even a braced string operand, so the literal cannot live in
# the assert expression itself)
set expected "\"say \\\"hi\\\"\\\\ tab\\there\\nnl \\u00e9 \\u2603 \\ud83d\\ude00 \\u0001\""
assert {[field $j cmd] eq $expected} \
    "cmd escaped: quotes, backslash, controls, non-ASCII, surrogate pair ([field $j cmd])"
assert {![regexp {[^\u0020-\u007e]} $j]} "record is pure ASCII"

# --- 4. large body: hash + len, no cmd ----------------------------------
set big [string repeat "set x 1;" 700]      ;# 5600 chars
set r [rmtSend $big]
assert {$r eq $big} "large command echoes back"
set j [rec]
assert {[field $j cmd] eq "<absent>"} "cmd omitted for a large body"
assert {[field $j len] == 5600} "len is the byte length ([field $j len])"
assert {[field $j hash] eq "\"[sha256 $big]\""} "hash is sha256 of the text"
assert {[string length [string trim [field $j hash] \"]] == 64} "hash is 64 hex chars"

# --- 5. restart continuity + io -----------------------------------------
set before [field [rec] seq]
rmtClose
set r [rmtSend "after_close"]
assert {$r eq ""} "not connected: empty reply (unchanged behaviour)"
assert {[field [rec] status] eq "\"io\""} "status io when not connected"
assert {[field [rec] dq] eq "null"} "header fields null on io"
assert {[field [rec] seq] == $before + 1} "seq continues across the gap"
assert {[rmtOpen localhost $port] == 1} "reconnected (a stim2 restart)"
rmtSend "after_reopen"
assert {[field [rec] seq] == $before + 2} "seq continuous across stim2 restarts"

# --- 6. nohdr: header requested, reply without one ----------------------
rmtSend "fake_rawecho 1"
set r [rmtSend "hello"]
assert {$r eq "hello"} "reply returned unchanged when the header is missing"
assert {[field [rec] status] eq "\"nohdr\""} "status nohdr"
assert {[field [rec] swap] eq "null"} "swap null on nohdr"
rmtSend "fake_rawecho 0"

# --- 7. a stim2 with the stimess package: the binding table ------------
# (header mode is still on: the table comes back under an ACK1 header,
# which the wrapper strips before stim_bind_publish sees it)
set table {{"package":"stimess 1.0","bindings":[{"name":"ess/dial/pointer","fields":["x","y","show","band"],"mode":"latest","sep":","},{"name":"planko/catcher","fields":["x","y"],"mode":"latest","sep":","}]}}
set empty {{"package":"","bindings":[]}}
set r [rmtSend {catch {stim::unbind_all}}]
assert {$r eq "0"} "catch {stim::unbind_all} answers 0 when the package is there ($r)"
set seq0 [field [rec] seq]
set names [::ess::stim_bind_publish]
assert {$names eq {ess/dial/pointer planko/catcher}} "stim_bind_publish returns the bound names ($names)"
assert {$::ess::stim_bound_names eq {ess/dial/pointer planko/catcher}} "stim_bound_names holds them"
assert {[dservGet stim/bind] eq $table} "stim/bind is the fake's table, verbatim"
assert {[dservTimestamp stim/bind] > 0} "stim/bind carries a timestamp"
assert {[field [rec] cmd] eq "\"stim::bindings_json\"" && [field [rec] seq] == $seq0 + 1} \
    "the query is itself one stim/cmd record"
assert {[field [rec] status] eq "\"ok\""} "the query's record is ok"
# query 0: the empty table, without asking stim2
set seq0 [field [rec] seq]
assert {[::ess::stim_bind_publish 0] eq {}} "stim_bind_publish 0 returns no names"
assert {[dservGet stim/bind] eq $empty} "stim_bind_publish 0 publishes the empty table"
assert {$::ess::stim_bound_names eq {}} "stim_bind_publish 0 clears the names"
assert {[field [rec] seq] == $seq0} "stim_bind_publish 0 sends nothing to stim2"
# a reply that is not JSON: empty table, no names (tcljson validates the
# document; the regexp fallback only extracts names, so only under tcljson)
if { $::stim_bind_parser eq "tcljson" } {
    rmtSend {fake_bindings {"package":"stimess 1.0","bindings":[{"name":"x",}]}}
    assert {[::ess::stim_bind_publish] eq {}} "unparseable table: no names"
    assert {[dservGet stim/bind] eq $empty} "unparseable table: the empty table is published"
    rmtSend fake_bindings
    assert {[::ess::stim_bind_publish] eq {ess/dial/pointer planko/catcher}} "default table restored"
    assert {[dservGet stim/bind] eq $table} "stim/bind is the table again"
}

# --- 8. old stim2: probe fails, nothing prefixed -----------------------
rmtSend "fake_ack 0"
assert {[::ess::stim_cmd_probe] == 0} "probe: an old stim2 errors on stimAckHeader"
set r [rmtSend "show_stim 4"]
assert {$r eq "show_stim 4"} "no prefix sent: the fake would have answered with a header"
set j [rec]
assert {[field $j status] eq "\"ok\""} "status ok without header"
assert {[field $j dq] eq "null" && [field $j done] eq "null" &&
        [field $j swap] eq "null" && [field $j flip] eq "null"} "header fields null"
assert {[field $j cmd] eq "\"show_stim 4\""} "cmd recorded"
set r [rmtSend "error boom2"]
assert {$r eq "!TCL_ERROR boom2"} "error string returned without header too"
assert {[field [rec] status] eq "\"error\""} "status error without header"

# ... and no stimess package either: nothing is bound, the table is empty
rmtSend "fake_stimess 0"
set r [rmtSend {catch {stim::unbind_all}}]
assert {$r eq "1"} "catch {stim::unbind_all} answers 1 without the package ($r)"
assert {[::ess::stim_bind_publish] eq {}} "old stim2: stim_bind_publish returns no names"
assert {$::ess::stim_bound_names eq {}} "old stim2: stim_bound_names empty"
assert {[dservGet stim/bind] eq $empty} "old stim2: stim/bind is the empty table"
assert {[field [rec] cmd] eq "\"stim::bindings_json\"" && [field [rec] status] eq "\"error\""} \
    "old stim2: the failed query is a stim/cmd error record"
rmtSend "fake_stimess 1"

# --- 9. pure helpers ----------------------------------------------------
assert {[::ess::stim_bind_names $table] eq {ess/dial/pointer planko/catcher}} \
    "stim_bind_names ($::stim_bind_parser) extracts the names in order"
assert {[::ess::stim_bind_names $empty] eq {}} "stim_bind_names: empty table"
if { $::stim_bind_parser eq "tcljson" } {
    assert {[catch {::ess::stim_bind_names {{"package":"x","bindings":[{"name":"a",}]}}}] == 1} \
        "stim_bind_names throws on invalid JSON (tcljson)"
    assert {[::ess::stim_bind_names {{"package":"x"}}] eq {}} "stim_bind_names: no bindings key"
}
assert {[::ess::stim_bind_names_regexp $table] eq {ess/dial/pointer planko/catcher}} \
    "regexp fallback extracts the names in order"
assert {[::ess::stim_bind_names_regexp $empty] eq {}} "regexp fallback: empty table"
set esc {{"bindings":[{"name":"a\"b\\c"}]}}
# (lindex: the list's string rep would re-escape the backslash)
assert {[lindex [::ess::stim_bind_names_regexp $esc] 0] eq "a\"b\\c"} "regexp fallback unescapes \\\" and \\\\"
assert {[::ess::stim_cmd_parse_ack "ACK1 1.000 2.000 3 4\nres"] eq {1 1.000 2.000 3 4 res}} \
    "parse_ack splits header and result"
assert {[::ess::stim_cmd_parse_ack "ACK1 1.000 2.000 3 4"] eq {1 1.000 2.000 3 4 {}}} \
    "parse_ack: header with no newline has an empty result"
assert {[::ess::stim_cmd_parse_ack "ACK1 100.000 101.500 42 -1\nres"] eq {1 100.000 101.500 42 -1 res}} \
    "parse_ack accepts a signed flip"
assert {[::ess::stim_cmd_parse_ack "ACK1 x y z w\nres"] eq 0} "parse_ack rejects non-numeric fields"
assert {[::ess::stim_cmd_parse_ack "ACK2 1 2 3 4\nres"] eq 0} "parse_ack rejects another tag"
assert {[::ess::json_escape {plain}] eq {plain}} "json_escape fast path"
assert {[::ess::json_escape "a\"b\\c\nd"] eq {a\"b\\c\nd}} "json_escape basics"
assert {[::ess::json_escape "\x7f"] eq {\u007f}} "json_escape DEL"
}

if { [catch run_tests err opts] } {
    puts "  FAIL: error: $err\n[dict get $opts -errorinfo]"
    lappend failures "error: $err"
}
finish
