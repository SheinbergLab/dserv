#
# test_ess_transports.tcl
#
#  Every registered input_transport resolver must name a proc that exists.
#
#  This is the failure a botched extraction produces (see
#  docs/ess_transports_extraction.md): a registration travels without its
#  resolver, or a resolver is dropped in the cut, and nothing notices until
#  a rig binds that transport and a subject presses. The registrations
#  execute at load; the resolver is not consulted until resolution. This
#  test closes that gap by walking the registry the way input_resolve does
#  and demanding each resolver be extractable from the source.
#
#  Unlike test_ess_binds and test_ess_resolve, which pull procs from
#  lib/ess-2.0.tm by name, this test scans ALL of lib/ess*.tm. A
#  registration in one module naming a resolver in another is fine at
#  runtime -- one namespace, both loaded -- so it is fine here too. The
#  point: when the transports move to lib/ess_transports-1.0.tm, this test
#  needs NO edit, and a half-done move (resolver in neither file) still
#  fails.
#
#  A UNIT test: plain tclsh, no server, no rig. The registry procs are pure
#  dict manipulation and the resolvers are only checked for existence,
#  never called, so nothing from dserv needs stubbing.
#
#  Run as: tclsh tests/test_ess_transports.tcl   (or: ctest -R ess_transports)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

set FAIL 0
proc ok   { label } { puts "  ok   $label" }
proc fail { label } { puts "  FAIL $label"; set ::FAIL 1 }

# Every ess module, keyed by tail name. After the extraction the new
# module joins this set by matching the glob; nothing here changes.
set ::SRC [dict create]
foreach f [lsort [glob -directory [file join $::REPO lib] ess*.tm]] {
    set fh [open $f]
    dict set ::SRC [file tail $f] [read $fh]
    close $fh
}

# Same linear scan as test_ess_binds (see there for why not a regex over
# the whole file), tried against each module in turn. Returns
# {file arglist body}, or "" if no module defines the proc.
proc extract_proc { name } {
    dict for { fname src } $::SRC {
        set start [string first "\n    proc $name " $src]
        if { $start < 0 } { continue }
        set end [string first "\n    \}\n" $src $start]
        if { $end < 0 } { continue }
        set nl   [string first "\n" $src [expr {$start + 1}]]
        set head [string trimright [string range $src [expr {$start + 1}] \
                                        [expr {$nl - 1}]] " \{"]
        set body [string range $src [expr {$nl + 1}] [expr {$end - 1}]]
        return [list $fname [lindex $head 2] $body]
    }
    return ""
}

# The registry itself comes from the real source, so this test also breaks
# if input_transport/input_transports move without their variable's
# semantics -- tracking the file, not a copy of it.
namespace eval ess { variable input_transports {} }
foreach p {input_transport input_transports} {
    set got [extract_proc $p]
    if { $got eq "" } { puts "FAIL: could not extract proc $p"; exit 1 }
    proc ::ess::$p [lindex $got 1] [lindex $got 2]
}

# Find the registration CALLS -- the lines that execute at module load.
# Anchored to a whole indented line so the proc definition and prose
# mentioning input_transport do not count.
set found {}
dict for { fname src } $::SRC {
    foreach line [split $src \n] {
        if { [regexp {^\s*#} $line] } { continue }
        if { [regexp {^\s+input_transport\s+(\w+)\s+(\w+)\s+(\S+)\s*$} \
                  $line -> kind name resolver] } {
            lappend found [list $fname $kind $name $resolver]
        }
    }
}

puts "registrations found at load:"
if { [llength $found] == 0 } {
    # A scan that finds nothing must fail, not vacuously pass: if the
    # registration format ever changes, this line is what says "update
    # the scan", loudly.
    fail "no input_transport registrations found in lib/ess*.tm"
}
foreach r $found {
    lassign $r fname kind name resolver
    namespace eval ::ess [list input_transport $kind $name $resolver]
    ok "$kind/$name registered ($fname)"
}

# The roster as of the extraction work order. Extras are welcome -- a new
# transport does not break this test -- but losing one of these means a
# registration line was dropped in a cut.
puts "\nknown transports still registered:"
foreach {kind names} {button {box_label box_pin joystick_bit gpio}
                      joystick {box_group analog}} {
    foreach name $names {
        if { $name in [::ess::input_transports $kind] } {
            ok "$kind/$name"
        } else {
            fail "$kind/$name is no longer registered"
        }
    }
}

# And the assertion itself: walk the registry as input_resolve would, and
# demand every resolver it can dispatch to actually exists somewhere in
# the module set.
puts "\nevery registered resolver names a proc that exists:"
foreach kind {button joystick} {
    foreach name [::ess::input_transports $kind] {
        set resolver [dict get $::ess::input_transports $kind $name]
        set tail [namespace tail $resolver]
        set got [extract_proc $tail]
        if { $got eq "" } {
            fail "$kind/$name -> $resolver: no proc $tail in lib/ess*.tm"
        } else {
            ok "$kind/$name -> $resolver ([lindex $got 0])"
        }
    }
}

puts ""
if { $FAIL } { puts "FAILED"; exit 1 } else { puts "all checks passed" }
