# test_ess_stim_cmd.tcl -- the rmtSend wrapper that records every command
# sent to stim2 as a stim/cmd JSON datapoint (lib/ess-2.0.tm, the block
# between "# BEGIN stim_cmd_log" and "# END stim_cmd_log"), and the stim2
# binding table it publishes as stim/bind (the "# BEGIN stim_bind" block:
# stim_bind_publish, the query-and-publish step configure_stim calls), and
# the per-frame record it switches on in stim2 (the "# BEGIN stim_frame_log"
# block: stim_frame_enable / stim_frame_disable / stim_frame_log_match), and
# the stim script record it publishes as stim/script (the "# BEGIN
# stim_script" block: stim_script_version / stim_script_dlsh /
# stim_script_json / stim_script_publish, whose hash joins the record to the
# load's stim/cmd), and the paradigm libraries configure_stim -libs ships,
# one stim/lib/<name> record each (the "# BEGIN stim_lib" block:
# stim_lib_version / stim_lib_json / stim_lib_publish), plus the
# stim_lib_script resolver they are read through.
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
# stim_frame_enable, of `catch {stim::unbind_all}` and of
# stim_bind_publish, unload_system's stim_frame_disable, and open_datafile's
# logger matches -- those lines are read from the source below instead,
# since the whole of ess-2.0.tm needs dlsh and a systems tree; the
# ess_stim_source test exercises configure_stim itself. The factored
# open_datafile step for stim/frame, stim_frame_log_match, IS run here,
# against a recording stand-in for dservLoggerAddMatch (the real command
# wants an open logger). The stim_bind
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
# the prelude: configure_stim sends `stim::prelude live` when stim2's
# stimess is 1.1 or newer (stim_prelude_version, stim_script block) and the
# inline block otherwise, after the mouse-bridge flag and before the
# libraries and the stim script go out
set prelude_at [string first "rmtSend \[list stim::prelude live -host \[dservGet ess/ipaddr\]\]" $src]
set inline_at  [string first "rmtSend \$rmtcmd" $src]
assert {$prelude_at > [string first "set ::mouse_bridge_mouse" $src] && $inline_at > $prelude_at &&
        $inline_at < $script_at} \
    "configure_stim sends stim::prelude live, with the inline block as fallback, before the stim script"
assert {[string first "if { \[::ess::stim_prelude_version\] ne \"\" }" $src] > 0 &&
        [string first "if { \[::ess::stim_prelude_version\] ne \"\" }" $src] < $prelude_at} \
    "the inline/package choice is stim_prelude_version"
assert {[string first "dservLoggerAddMatch \$filename stim/bind 0 0 1" $src] > 0} \
    "open_datafile subscribes stim/bind unbuffered, not obs-limited, every record"
assert {[string first "dservLoggerAddMatch \$filename \$_bound 1 0 1" $src] > 0} \
    "open_datafile subscribes each bound stream obs-limited, unbuffered, every record"
assert {[string first "dservTouch stim/bind" $src] > [string first "dservLoggerResume \$filename" $src]} \
    "open_datafile re-emits stim/bind after the logger is running"

# --- the stim_frame_log block, same way --------------------------------
set fb [string first "# BEGIN stim_frame_log" $src]
set fe [string first "# END stim_frame_log" $src]
assert {$fb > $be && $fe > $fb} "found the stim_frame_log block in lib/ess-2.0.tm"
namespace eval ess [string range $src $fb [expr {$fe - 1}]]
assert {[info procs ::ess::stim_frame_enable] ne "" && [info procs ::ess::stim_frame_disable] ne "" &&
        [info procs ::ess::stim_frame_log_match] ne ""} "stim_frame_{enable,disable,log_match} defined"
assert {$::ess::stim_frame_log == 0} "stim_frame_log starts 0"

# the call sites outside the block: configure_stim enables right after the
# probe (before anything the logged wrapper sends), records 0 when not
# connected; unload_system disables after the system deinit callback;
# open_datafile calls the factored match step next to the stim/cmd match
set enable_at [string first "::ess::stim_frame_enable \$frame_sink" $src]
assert {$enable_at > [string first "::ess::stim_cmd_probe" $src] && $enable_at < $unbind_at} \
    "configure_stim calls stim_frame_enable \$frame_sink right after the probe"
# the sink is dserv's address as stim2 reaches it, never the stim2 host:
# on a rig with a separate display box, $host would point stim2 at itself
set sink_at [string first "set frame_sink \[dservGet ess/ipaddr\]" $src]
assert {$sink_at > 0 && $sink_at < $enable_at} \
    "configure_stim takes the frame sink from ess/ipaddr, not \$host"
assert {[string first "set ::ess::stim_frame_log 0" $src] > $fe &&
        [string first "set ::ess::stim_frame_log 0" $src] < $enable_at} \
    "configure_stim records flag 0 on the not-connected branch"
set deinit_at [string first "catch {\$s deinit}" $src]
set disable_at [string first "\n            stim_frame_disable\n" $src]
assert {$deinit_at > 0 && $disable_at > $deinit_at &&
        $disable_at < [string first "set current(open_system) 0" $src $deinit_at]} \
    "unload_system calls stim_frame_disable after the system deinit callback"
set match_at [string first "\n        stim_frame_log_match \$filename\n" $src]
assert {$match_at > [string first "dservLoggerAddMatch \$filename stim/cmd 0 0 1" $src] &&
        $match_at < [string first "dservLoggerAddMatch \$filename stim/bind 0 0 1" $src]} \
    "open_datafile calls stim_frame_log_match next to the stim/cmd match"
assert {[regexp {(?s)proc stim_frame_log_match \{ filename \} \{.*?dservLoggerAddMatch \$filename stim/frame 0 0 1} $src]} \
    "stim_frame_log_match subscribes stim/frame not obs-limited, unbuffered (one frame per record), every record"

# --- the stim_script block, same way ------------------------------------
set sb [string first "# BEGIN stim_script" $src]
set se [string first "# END stim_script" $src]
assert {$sb > $fe && $se > $sb} "found the stim_script block in lib/ess-2.0.tm"
namespace eval ess [string range $src $sb [expr {$se - 1}]]
assert {[info procs ::ess::stim_script_version] ne "" && [info procs ::ess::stim_script_json] ne "" &&
        [info procs ::ess::stim_script_publish] ne "" && [info procs ::ess::stim_script_stimess] ne "" &&
        [info procs ::ess::stim_script_dlsh] ne ""} \
    "stim_script_{version,dlsh,json,publish,stimess} defined"
assert {$::ess::stim2_version eq ""} "stim2_version starts empty"
assert {$::ess::stim2_dlsh eq ""} "stim2_dlsh starts empty"

# the call sites outside the block: configure_stim asks the version right
# after the probe; publishes the empty record on the not-connected branch
# (with the version cleared) and again before the script goes out; takes
# the timestamp just before the send; publishes the full record after the
# binding table has been asked for. open_datafile matches stim/script next
# to stim/cmd and re-emits it, guarded, once the logger runs.
set version_at [string first "::ess::stim_script_version\n" $src $se]
assert {$version_at > [string first "::ess::stim_cmd_probe" $src] && $version_at < $enable_at} \
    "configure_stim calls stim_script_version right after the probe"
set dlsh_at [string first "::ess::stim_script_dlsh\n" $src $se]
assert {$dlsh_at > $version_at && $dlsh_at < $enable_at} \
    "configure_stim calls stim_script_dlsh right after the version"
assert {[string first "set ::ess::stim2_dlsh {}" $src $se] > $se &&
        [string first "set ::ess::stim2_dlsh {}" $src $se] < $enable_at} \
    "configure_stim clears the dlsh version when not connected"
# (the variant is ::ess::current(variant), not the System's _variantname:
# configure_stim runs inside protocol_init, before set_variant)
set empty_pub "::ess::stim_script_publish {} {} \[now\] \$_systemname \$_protocolname \$::ess::current(variant)\n"
set empty1_at [string first $empty_pub $src $se]
set empty2_at [string first $empty_pub $src [expr {$empty1_at + 1}]]
assert {$empty1_at > $se && $empty1_at < $enable_at &&
        [string first "set ::ess::stim2_version {}" $src $se] > $se &&
        [string first "set ::ess::stim2_version {}" $src $se] < $empty1_at} \
    "configure_stim clears the version and publishes the empty record when not connected"
assert {$empty2_at > $unbind_at && $empty2_at < $script_at &&
        [string first $empty_pub $src [expr {$empty2_at + 1}]] < 0} \
    "configure_stim publishes the empty record once more before sourcing, and only then"
set tscript_at [string last "set t_script \[now\]" $src]
assert {$tscript_at > [string first "set script \[read \$f\]" $src] && $tscript_at < $script_at} \
    "configure_stim takes t_script just before sending the script"
set full_pub_at [string first "::ess::stim_script_publish \$stimfile \$script \$t_script" $src]
assert {$full_pub_at > [string first "\n        ::ess::stim_bind_publish\n" $src] &&
        [string first "\$_systemname \$_protocolname \$::ess::current(variant)" $src $full_pub_at] > $full_pub_at &&
        [string first "stim_script_publish \$stimfile \$script \$t_script \\\n            \$_systemname \$_protocolname \$_variantname" $src] < 0} \
    "configure_stim publishes the full record after stim_bind_publish, with the load's names (variant from current)"
set script_match_at [string first "dservLoggerAddMatch \$filename stim/script 0 0 1" $src]
assert {$script_match_at > [string first "dservLoggerAddMatch \$filename stim/cmd 0 0 1" $src] &&
        $script_match_at < $match_at} \
    "open_datafile subscribes stim/script unbuffered, not obs-limited, every record, next to stim/cmd"
assert {[string first "if {\[dservExists stim/script\]} { dservTouch stim/script }" $src] > \
            [string first "dservLoggerResume \$filename" $src]} \
    "open_datafile re-emits stim/script, guarded by dservExists, after the logger is running"

# --- the stim_lib block, same way ----------------------------------------
set lb [string first "# BEGIN stim_lib" $src]
set le [string first "# END stim_lib" $src]
assert {$lb > $se && $le > $lb} "found the stim_lib block in lib/ess-2.0.tm"
namespace eval ess [string range $src $lb [expr {$le - 1}]]
assert {[info procs ::ess::stim_lib_version] ne "" && [info procs ::ess::stim_lib_json] ne "" &&
        [info procs ::ess::stim_lib_publish] ne ""} "stim_lib_{version,json,publish} defined"
assert {$::ess::stim_lib_names eq {}} "stim_lib_names starts empty"

# ... and the resolver the libraries are read through, a plain proc outside
# the blocks (it needs only a systems tree): its text, from `proc
# stim_lib_script` to the first line that is exactly four spaces and a brace
set rb [string first "    proc stim_lib_script " $src]
set re [string first "\n    \}\n" $src $rb]
assert {$rb > 0 && $re > $rb} "found stim_lib_script in lib/ess-2.0.tm"
namespace eval ess [string range $src $rb [expr {$re + 6}]]
assert {[info procs ::ess::stim_lib_script] ne ""} "stim_lib_script defined"

# the call sites outside the block: configure_stim clears the names first
# thing (before the not-connected branch), and in the -libs loop publishes
# each library after the `package forget` and before the rmtSend that ships
# it, with the path stim_lib_script resolved; open_datafile adds one
# wildcard match next to stim/script and re-emits each published name,
# guarded, once the logger runs
set names_reset_at [string first "set ::ess::stim_lib_names {}" $src $le]
assert {$names_reset_at > $le && $names_reset_at < [string first "if { !\[rmtOpen \$host\] }" $src $le]} \
    "configure_stim clears stim_lib_names before the not-connected branch"
set lib_read_at [string first "set libscript \[::ess::stim_lib_script \$lib libpath\]" $src]
set forget_at [string first "rmtSend \"catch {package forget \$lib}\"" $src]
set lib_pub_at [string first "::ess::stim_lib_publish \$lib \$libpath \$libscript \[now\]" $src]
set lib_send_at [string first "set result \[rmtSend \$libscript\]" $src]
assert {$lib_read_at > $le && $forget_at > $lib_read_at && $lib_pub_at > $forget_at && $lib_send_at > $lib_pub_at &&
        $lib_send_at < $script_at} \
    "configure_stim publishes stim/lib/<name> in the -libs loop, after package forget, before the ship"
# (searched past the block: its comment quotes the same line)
set lib_match_at [string first "dservLoggerAddMatch \$filename stim/lib/* 0 0 1" $src $le]
assert {$lib_match_at > $script_match_at && $lib_match_at < $match_at} \
    "open_datafile subscribes stim/lib/* unbuffered, not obs-limited, every record, next to stim/script"
set lib_touch_at [string first "if {\[dservExists stim/lib/\$_lib\]} { dservTouch stim/lib/\$_lib }" $src]
assert {$lib_touch_at > [string first "dservLoggerResume \$filename" $src] &&
        [string first "foreach _lib \$stim_lib_names \{" $src] > 0 &&
        [string first "foreach _lib \$stim_lib_names \{" $src] < $lib_touch_at} \
    "open_datafile re-emits each stim/lib/<name> in stim_lib_names, guarded by dservExists, after the logger is running"

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

# --- 7b. a stim2 with stimFrameLog: the per-frame record -----------------
# enable: raw command (no stim/cmd record), flag 1, the fake saw the host.
# stim2 answers the start with its status dict (connected 0 until the
# writer thread has connected): anything but an error means "taken"
proc fake_frame_state {} { return [::ess::rmtSend_raw fake_frame_state] }
set seq0 [field [rec] seq]
set r [::ess::rmtSend_raw [list stimFrameLog stimhost.local 1]]
assert {[dict get $r enabled] == 1 && [dict get $r connected] == 0} \
    "a start answers the status dict, connected 0 at once ($r)"
assert {[::ess::rmtSend_raw [list stimFrameLog stimhost.local 0]] ne ""} "a stop answers the status dict too"
assert {[::ess::stim_frame_enable stimhost.local] == 1} "stim_frame_enable returns 1 on a stim2 with the command"
assert {$::ess::stim_frame_log == 1} "stim_frame_log is 1"
assert {$::ess::stim_frame_host eq "stimhost.local"} "stim_frame_host remembered"
set st [fake_frame_state]
assert {[dict get $st enabled] == 1 && [dict get $st host] eq "stimhost.local" && [dict get $st enables] == 2} \
    "the fake saw stimFrameLog stimhost.local 1 ($st)"
assert {[field [rec] seq] == $seq0} "the enable is not a stim/cmd record"
# the bare status query (stim2's probe form; what an operator would type)
set status [::ess::rmtSend_raw stimFrameLog]
assert {[dict get $status enabled] == 1 && [dict get $status host] eq "stimhost.local" &&
        [dict exists $status sent] && [dict exists $status dropped] && [dict get $status connected] == 1} \
    "bare stimFrameLog answers the status dict ($status)"
# the open_datafile step: with the flag 1 exactly one match, the documented one
set ::matches {}
proc ::ess::dservLoggerAddMatch { args } { lappend ::matches $args }
assert {[::ess::stim_frame_log_match /tmp/x.ess] == 1} "stim_frame_log_match adds a match when the flag is 1"
assert {$::matches eq {{/tmp/x.ess stim/frame 0 0 1}}} \
    "the match is stim/frame, not obs-limited, unbuffered, every record ($::matches)"
# disable: once, raw, flag back to 0; a second disable sends nothing
assert {[::ess::stim_frame_disable] == 1} "stim_frame_disable sends the stop when the flag is 1"
assert {$::ess::stim_frame_log == 0} "flag 0 after disable"
set st [fake_frame_state]
assert {[dict get $st enabled] == 0 && [dict get $st disables] == 2} "the fake saw stimFrameLog stimhost.local 0 ($st)"
assert {[field [rec] seq] == $seq0} "the disable is not a stim/cmd record either"
assert {[::ess::stim_frame_disable] == 0} "a second disable is a no-op"
assert {[dict get [fake_frame_state] disables] == 2} "... and sends nothing"
set ::matches {}
assert {[::ess::stim_frame_log_match /tmp/x.ess] == 0 && $::matches eq {}} \
    "stim_frame_log_match adds nothing when the flag is 0"
# re-enable with another host (a reconnect to a different display), then
# a disable when stim2 has gone away must not throw
assert {[::ess::stim_frame_enable 10.0.0.7] == 1 && [dict get [fake_frame_state] host] eq "10.0.0.7"} \
    "re-enable towards another host reaches the fake"
rmtClose
assert {[catch {::ess::stim_frame_disable} r] == 0 && $r == 1 && $::ess::stim_frame_log == 0} \
    "disable with stim2 gone: no error, flag 0"
assert {[rmtOpen localhost $port] == 1} "reconnected after the disable"
assert {[::ess::stim_frame_disable] == 0} "nothing left to disable after the reconnect"

# --- 7c. the stim script record ------------------------------------------
# version: raw command (no stim/cmd record), remembered
set seq0 [field [rec] seq]
assert {[::ess::stim_script_version] eq "0.27.3"} "stim_script_version answers the fake's stimVersion release"
assert {$::ess::stim2_version eq "0.27.3"} "stim2_version remembered"
assert {[field [rec] seq] == $seq0} "the version query is not a stim/cmd record"
# a 0.27.x stim2: no stimVersion command, but the StimVersion global
rmtSend "fake_stimversion 0"
assert {[::ess::stim_script_version] eq "20"} "stim_script_version falls back to StimVersion"
rmtSend "fake_stimversion 1"
assert {[::ess::stim_script_version] eq "0.27.3"} "stimVersion preferred again"
# the dlsh stim2 loaded: raw command (no stim/cmd record), remembered
set seq0 [field [rec] seq]
assert {[::ess::stim_script_dlsh] eq "0.20.3"} "stim_script_dlsh answers the fake's package present dlsh"
assert {$::ess::stim2_dlsh eq "0.20.3"} "stim2_dlsh remembered"
assert {[field [rec] seq] == $seq0} "the dlsh query is not a stim/cmd record"
# the prelude probe: raw, the stimess version when it is 1.1 or newer
set seq0 [field [rec] seq]
assert {[::ess::stim_prelude_version] eq "1.1"} "stim_prelude_version answers the fake's stimess 1.1"
assert {[field [rec] seq] == $seq0} "the prelude probe is not a stim/cmd record"
rmtSend "fake_stimess_version 1.0"
assert {[::ess::stim_prelude_version] eq ""} "stimess 1.0 (bindings only, no prelude) reads as empty"
rmtSend "fake_stimess_version 2.0"
assert {[::ess::stim_prelude_version] eq "2.0"} "a newer stimess still carries the prelude"
rmtSend "fake_stimess_version 1.1"
assert {[::ess::stim_script_stimess $table] eq "1.0"} "stim_script_stimess reads 1.0 off the table"
assert {[::ess::stim_script_stimess $empty] eq ""} "stim_script_stimess: empty table, no version"
assert {[::ess::stim_script_stimess {{"package":"other 2.0","bindings":[]}}] eq ""} \
    "stim_script_stimess: another package is not stimess"
# the table is up (as after a load whose script required stimess)
::ess::stim_bind_publish
assert {[dservGet stim/bind] eq $table} "stim/bind holds the table before the script record"

# a body with everything the escaper must handle, and long enough that its
# stim/cmd record carries the hash rather than the text (> 4096 chars)
set body "# stim \"quoted\" \\ back\\slash\n\tproc stim_init {} {\n\t\tputs \"café\"\r\n\t}\n"
append body [string repeat "set x \"1\";\n" 500]
set body_bytes [encoding convertto utf-8 $body]
# ... what the escaper gives back: subst's backslash rules ARE JSON's for
# \" \\ \n \r \t \b \f \/ and \uXXXX
proc json_unquote { q } { return [subst -nocommands -novariables [string range $q 1 end-1]] }

set r [rmtSend $body]
assert {$r eq $body} "the script load echoes back"
set load [rec]
assert {[field $load cmd] eq "<absent>" && [string length [string trim [field $load hash] \"]] == 64} \
    "the load's stim/cmd record carries hash, not cmd"
set t [now]
set json [::ess::stim_script_publish /systems/ess/p/s/proto/proto_stim.tcl $body $t sysA protoB varC]
set j [dservGet stim/script]
assert {$j eq $json} "stim/script holds the record stim_script_publish returned"
assert {[dservTimestamp stim/script] == $t} "stim/script timestamp is the one given"
assert {![regexp {[^ -~]} $j]} "the record is pure ASCII"
assert {[field $j path] eq "\"/systems/ess/p/s/proto/proto_stim.tcl\""} "path recorded"
assert {[field $j hash] eq [field $load hash]} "hash equals the load's stim/cmd hash (the join key)"
assert {[field $j hash] eq "\"[sha256 $body]\""} "hash is sha256 of the text"
assert {[field $j len] == [string length $body_bytes]} "len is the UTF-8 byte length ([field $j len])"
assert {[field $j len] > [string length $body]} "... which exceeds the char count (non-ASCII inside)"
assert {[json_unquote [field $j body]] eq $body} "body round-trips through the escaper"
assert {[string first "\\u00e9" [field $j body]] > 0 && [string first "\\n\\t" [field $j body]] > 0} \
    "body is escaped (\\u00e9, \\n, \\t present)"
assert {[field $j system] eq "\"sysA\"" && [field $j protocol] eq "\"protoB\"" && [field $j variant] eq "\"varC\""} \
    "system/protocol/variant recorded"
assert {[field $j stim2] eq "\"0.27.3\""} "stim2 is the remembered version"
assert {[field $j stimess] eq "\"1.0\""} "stimess comes from the stim/bind table"
assert {[field $j dlsh] eq "\"0.20.3\""} "dlsh is the remembered dlsh version"
assert {[field $j hash_note] eq "<absent>"} "no hash_note where sha256 exists"
assert {[field [rec] seq] == [field $load seq]} "publishing sends nothing to stim2"
# an independent sha256 of the same bytes
if { [auto_execok shasum] ne "" } {
    set tmp [file join [pwd] stim_script_[pid].tcl]
    set tf [open $tmp w]
    fconfigure $tf -encoding utf-8 -translation lf
    puts -nonewline $tf $body
    close $tf
    set independent [lindex [exec {*}[auto_execok shasum] -a 256 $tmp] 0]
    file delete -force $tmp
    assert {[field $j hash] eq "\"$independent\""} "hash matches shasum -a 256 of the body"
} else {
    puts "  (no shasum here: independent hash check skipped)"
}
# the real parser, where there is one
if { $::stim_bind_parser eq "tcljson" } {
    set d [::json_to_dict $j]
    assert {[dict get $d body] eq $body} "tcljson parses the record and gives the body back exactly"
    assert {[dict get $d len] == [string length $body_bytes] && [dict get $d stim2] eq "0.27.3" &&
            [dict get $d dlsh] eq "0.20.3"} \
        "tcljson: len, stim2 and dlsh as expected"
}
# the empty record (not connected / before the script is sent)
set t [now]
::ess::stim_script_publish {} {} $t sysA protoB {}
set j [dservGet stim/script]
assert {[field $j path] eq "\"\"" && [field $j body] eq "\"\"" && [field $j hash] eq "\"\"" && [field $j len] == 0} \
    "the empty record: path, body, hash empty, len 0"
assert {[field $j variant] eq "\"\"" && [field $j stimess] eq "\"1.0\""} \
    "the empty record still names the load and the table's package"
# stimess follows the table: the empty table gives ""
::ess::stim_bind_publish 0
::ess::stim_script_publish /x.tcl "puts hi" [now] s p v
assert {[field [dservGet stim/script] stimess] eq "\"\""} "stimess empty when the table names no package"
assert {[field [dservGet stim/script] len] == 7 && [field [dservGet stim/script] body] eq "\"puts hi\""} \
    "a short plain body is stored as is"
# the pure record builder
set jj [::ess::stim_script_json "p\"q" "a\tb" s p v 20 1.0 0.20.3]
assert {[field $jj path] eq {"p\"q"} && [field $jj body] eq {"a\tb"} && [field $jj len] == 3 &&
        [field $jj dlsh] eq {"0.20.3"}} \
    "stim_script_json escapes path and body, counts bytes, carries dlsh"

# --- 7d. the shipped paradigm libraries: stim/lib/<name> ----------------
# a systems tree in a temp dir: <systems>/<project>/lib/runner-1.0.tm and
# runner-1.2.tm (the resolver must pick 1.2), and chaser-0.3.tm
set libroot [file join [pwd] stim_lib_[pid]]
set libdir [file join $libroot myproj lib]
file mkdir $libdir
# (no \r: the resolver reads the file with Tcl's auto translation, as
# configure_stim does, so a CRLF file would be sent -- and recorded -- as LF)
set lib_body "# runner \"quoted\" \\ back\\slash\n\tpackage provide runner 1.2\n\tproc run_step {} {\n\t\tputs \"café\"\n\t}\n"
append lib_body [string repeat "set y \"2\";\n" 500]      ;# > 4096 chars: hash, not cmd
set lib_bytes [encoding convertto utf-8 $lib_body]
foreach { tail text } [list runner-1.0.tm "package provide runner 1.0\n" \
                            runner-1.2.tm $lib_body \
                            chaser-0.3.tm "package provide chaser 0.3\n"] {
    set lf [open [file join $libdir $tail] w]
    fconfigure $lf -encoding utf-8 -translation lf
    puts -nonewline $lf $text
    close $lf
}
set ::ess::system_path $libroot
set ::ess::project myproj
# the resolver: highest version, text verbatim, path handed back alongside
set libpath ""
set got [::ess::stim_lib_script runner libpath]
assert {$got eq $lib_body} "stim_lib_script returns the text of the highest version"
assert {$libpath eq [file join $libdir runner-1.2.tm]} "stim_lib_script hands the resolved path back ($libpath)"
assert {[::ess::stim_lib_script chaser] eq "package provide chaser 0.3\n"} "stim_lib_script without pathvar still works"
set nosuch_rc [catch {::ess::stim_lib_script nosuch} msg]
assert {$nosuch_rc == 1 && [string match "no nosuch-<version>.tm*" $msg]} \
    "stim_lib_script errors for a library that is not there ($msg)"
# the version off the file name
assert {[::ess::stim_lib_version runner /a/b/runner-1.0.tm] eq "1.0"} "stim_lib_version: runner-1.0.tm -> 1.0"
assert {[::ess::stim_lib_version runner $libpath] eq "1.2"} "stim_lib_version: the resolved path -> 1.2"
assert {[::ess::stim_lib_version runner /a/b/runner-1.0.2.tm] eq "1.0.2"} "stim_lib_version: a three-part version"
assert {[::ess::stim_lib_version runner /a/b/other-1.0.tm] eq ""} "stim_lib_version: another name gives empty"
assert {[::ess::stim_lib_version run /a/b/runner-1.0.tm] eq ""} "stim_lib_version: a prefix of the name is not the name"
# what configure_stim does per library: forget, publish, ship
set seq0 [field [rec] seq]
rmtSend "catch {package forget runner}"
set t [now]
set json [::ess::stim_lib_publish runner $libpath $lib_body $t]
set r [rmtSend $lib_body]
assert {$r eq $lib_body} "the library ship echoes back (a large body is a command like any other)"
set ship [rec]
assert {[field $ship cmd] eq "<absent>" && [string length [string trim [field $ship hash] \"]] == 64} \
    "the ship's stim/cmd record carries hash, not cmd"
assert {[field [rec] seq] == $seq0 + 2} "publishing sends nothing to stim2 (forget + ship only)"
set j [dservGet stim/lib/runner]
assert {$j eq $json} "stim/lib/runner holds the record stim_lib_publish returned"
assert {[dservTimestamp stim/lib/runner] == $t} "stim/lib/runner timestamp is the one given"
assert {![regexp {[^ -~]} $j]} "the record is pure ASCII"
assert {[field $j name] eq "\"runner\""} "name recorded"
assert {[field $j version] eq "\"1.2\""} "version parsed from the resolved file name"
assert {[field $j path] eq "\"[::ess::json_escape $libpath]\""} "path recorded"
assert {[field $j hash] eq [field $ship hash]} "hash equals the ship's stim/cmd hash (the join key)"
assert {[field $j hash] eq "\"[sha256 $lib_body]\""} "hash is sha256 of the text"
assert {[field $j len] == [string length $lib_bytes]} "len is the UTF-8 byte length ([field $j len])"
assert {[field $j len] > [string length $lib_body]} "... which exceeds the char count (non-ASCII inside)"
assert {[json_unquote [field $j body]] eq $lib_body} "body round-trips through the escaper"
assert {[string first "\\u00e9" [field $j body]] > 0 && [string first "\\n\\t" [field $j body]] > 0 &&
        [string first "\\\\ back\\\\slash" [field $j body]] > 0 && [string first "\\\"quoted\\\"" [field $j body]] > 0} \
    "body is escaped (\\u00e9, \\n, \\t, backslashes, quotes present)"
assert {[field $j hash_note] eq "<absent>"} "no hash_note where sha256 exists"
assert {$::ess::stim_lib_names eq {runner}} "stim_lib_names holds the published name"
# an independent sha256 of the file as written (the same bytes)
if { [auto_execok shasum] ne "" } {
    set independent [lindex [exec {*}[auto_execok shasum] -a 256 $libpath] 0]
    assert {[field $j hash] eq "\"$independent\""} "hash matches shasum -a 256 of the library file"
} else {
    puts "  (no shasum here: independent hash check skipped)"
}
# the real parser, where there is one
if { $::stim_bind_parser eq "tcljson" } {
    set d [::json_to_dict $j]
    assert {[dict get $d body] eq $lib_body} "tcljson parses the record and gives the body back exactly"
    assert {[dict get $d name] eq "runner" && [dict get $d version] eq "1.2" &&
            [dict get $d len] == [string length $lib_bytes]} "tcljson: name, version and len as expected"
}
# a second library: its own datapoint, both names remembered, the first intact
set chaser_path ""
set chaser_body [::ess::stim_lib_script chaser chaser_path]
::ess::stim_lib_publish chaser $chaser_path $chaser_body
set jc [dservGet stim/lib/chaser]
assert {[field $jc name] eq "\"chaser\"" && [field $jc version] eq "\"0.3\"" &&
        [field $jc body] eq "\"package provide chaser 0.3\\n\"" && [field $jc len] == 27} \
    "stim/lib/chaser is its own record"
assert {[dservGet stim/lib/runner] eq $json} "stim/lib/runner is untouched by the second publish"
assert {$::ess::stim_lib_names eq {runner chaser}} "stim_lib_names holds both, in ship order"
assert {[dservTimestamp stim/lib/chaser] >= $t} "a publish without t stamps now"
# re-publishing a name (a reload) does not duplicate it
::ess::stim_lib_publish runner $libpath $lib_body
assert {$::ess::stim_lib_names eq {runner chaser}} "a re-publish keeps one entry per name"
# the pure record builder: an unparseable file name gives version ""
set jj [::ess::stim_lib_json "r\"q" "" "/p/x.tm" "a\tb"]
assert {[field $jj name] eq {"r\"q"} && [field $jj version] eq {""} && [field $jj body] eq {"a\tb"} &&
        [field $jj len] == 3} \
    "stim_lib_json escapes name and body, counts bytes, keeps an empty version"
file delete -force $libroot

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
assert {[catch {::ess::stim_prelude_version} r] == 0 && $r eq ""} \
    "old stim2: stim_prelude_version returns empty without error (configure_stim sends the inline block)"
rmtSend "fake_stimess 1"

# ... and no stimFrameLog: the enable records 0, no error propagates,
# nothing reaches the fake's writer, the datafile gets no match
rmtSend "fake_frame 0"
set enables0 [dict get [fake_frame_state] enables]
assert {[catch {::ess::stim_frame_enable stimhost.local} r] == 0 && $r == 0} \
    "old stim2: stim_frame_enable returns 0 without error"
assert {$::ess::stim_frame_log == 0} "old stim2: stim_frame_log is 0"
assert {[dict get [fake_frame_state] enables] == $enables0} "old stim2: nothing was enabled"
assert {[string match "!TCL_ERROR*" [::ess::rmtSend_raw stimFrameLog]]} \
    "old stim2: the bare probe errors too"
set ::matches {}
assert {[::ess::stim_frame_log_match /tmp/x.ess] == 0 && $::matches eq {}} "old stim2: no stim/frame match"
assert {[::ess::stim_frame_disable] == 0} "old stim2: disable is a no-op"
rmtSend "fake_frame 1"
rename ::ess::dservLoggerAddMatch {}

# ... and no StimVersion: the version is "", no error, and the record says so
rmtSend "fake_version 0"
assert {[catch {::ess::stim_script_version} r] == 0 && $r eq ""} \
    "old stim2: stim_script_version returns empty without error"
assert {$::ess::stim2_version eq ""} "old stim2: stim2_version cleared"
::ess::stim_script_publish /x.tcl "puts hi" [now] s p v
assert {[field [dservGet stim/script] stim2] eq "\"\""} "old stim2: stim2 field empty"
rmtSend "fake_version 1"

# ... and no dlsh present in stim2's interp: the dlsh field is "", no error
rmtSend "fake_dlsh 0"
assert {[catch {::ess::stim_script_dlsh} r] == 0 && $r eq ""} \
    "no dlsh: stim_script_dlsh returns empty without error"
assert {$::ess::stim2_dlsh eq ""} "no dlsh: stim2_dlsh cleared"
::ess::stim_script_publish /x.tcl "puts hi" [now] s p v
assert {[field [dservGet stim/script] dlsh] eq "\"\""} "no dlsh: dlsh field empty"
rmtSend "fake_dlsh 1"
assert {[::ess::stim_script_dlsh] eq "0.20.3"} "dlsh back after the switch"

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
