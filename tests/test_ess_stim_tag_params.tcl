# test_ess_stim_tag_params.tcl -- swap-tag params reach integer events.
#
# stimess packs a swapTag's params as floats, so `swapTag {TARGET SET 1}`
# arrives as 1.0.  TARGET's params are long, and evt_put refused 1.0
# ("expected integer but got 1.0"): stim_tag_evt logged a warning and the
# frame-exact event was lost.  stim_tag_params now hands whole-number vals
# to a short/long event as integers, and leaves a fractional val (and every
# val of a float/string event) as it came, so a real mismatch still warns.
#
#   dlsh tests/test_ess_stim_tag_params.tcl
#
# The two procs are read out of lib/ess-2.0.tm (the whole module needs a
# dserv); evt_put, the event table and the stim2 blob are stand-ins.

set here [file dirname [file normalize [info script]]]
set lib  [file join [file dirname $here] lib ess-2.0.tm]

set fails 0
proc check { got want what } {
    if { $got eq $want } { puts "  ok: $what" } else {
        puts "  FAIL: $what: got {$got} want {$want}"
        incr ::fails
    }
}

# one proc's source, from its header line to the closing brace at its indent
proc proc_source { file name } {
    set fd [open $file] ; set src [read $fd] ; close $fd
    set lines [split $src \n]
    set start [lsearch -glob $lines "    proc $name *"]
    if { $start < 0 } { error "no proc $name in $file" }
    set close "    [format %c 125]"
    set end [lsearch -start $start -exact $lines $close]
    return [join [lrange $lines $start $end] \n]
}

namespace eval ::ess {
    variable evt_datatypes { unknown 0 null 1 string 2 short 3 \
                                 long 4 float 5 double 6 }
    variable current
    set current(state_system) ::sys
    variable warned {}
    variable put {}
    proc ess_warning { msg cat } { variable warned ; lappend warned $msg }
    proc stim_evt_time { args } { return 1000 }
    # evt_put as the real one fails: evtPut refuses a non-integer for a
    # long event
    proc evt_put { type subtype t args } {
        variable put
        if { $type in {TARGET ABORT} } {
            foreach a $args {
                if { ![string is wide -strict $a] } {
                    error "expected integer but got \"$a\""
                }
            }
        }
        lappend put [list $type $subtype {*}$args]
    }
}
namespace eval ::sys {
    variable _evt_ptype_ids { TARGET 4 ABORT 4 EMPARAMS 5 NAME 2 PATTERN 1 }
}
# the wire format's float list, as qpcs::evtUnpack returns it
namespace eval ::qpcs {
    proc evtUnpack { data } { return $data }
}

namespace eval ::ess [proc_source $lib stim_tag_params]
namespace eval ::ess [proc_source $lib stim_tag_evt]

puts "stim_tag_params:"
check [::ess::stim_tag_params TARGET {1.0}]       {1}       "whole float -> int for a long event"
check [::ess::stim_tag_params TARGET {1.0 2.0}]   {1 2}     "each val"
check [::ess::stim_tag_params TARGET {-3.0}]      {-3}      "negative"
check [::ess::stim_tag_params TARGET {1.5}]       {1.5}     "fractional left alone"
check [::ess::stim_tag_params TARGET {2.0 2.5}]   {2.0 2.5} "one fractional val leaves them all"
check [::ess::stim_tag_params EMPARAMS {1.0 10.0}] {1.0 10.0} "float event untouched"
check [::ess::stim_tag_params NAME {1.0}]         {1.0}     "string event untouched"
check [::ess::stim_tag_params NOSUCH {1.0}]       {1.0}     "unknown type untouched (evt_put reports it)"

puts "stim_tag_evt:"
# {swap ticksF vals flipwall tag}
::ess::stim_tag_evt stim/tagevt {10 1000.0 {1.0} 0 {TARGET SET}}
check [lindex $::ess::put end] {TARGET SET 1} "TARGET SET 1 becomes an event"
::ess::stim_tag_evt stim/tagevt {11 1008.0 {} 0 {TARGET ON}}
check [lindex $::ess::put end] {TARGET ON} "a tag with no params"
::ess::stim_tag_evt stim/tagevt {12 1016.0 {2.5} 0 {TARGET SET}}
check [llength $::ess::warned] 1 "a fractional val still warns"

if { $fails } {
    puts "$fails FAILURE(S)"
    exit 1
}
puts "all checks passed"
