#
# Handle em incoming data - degrees output with calibration
#

package require dlsh
package require yajltcl

package require math::linearalgebra

tcl::tm::add $dspath/lib
package require extio       ;# decode extio state/ain blocks (analog eye source)
package require settingsdb  ;# persist stable eye calibration across dserv restarts
package require settings    ;# `eye source` is DECLARED, not learned -- see below

# Calibration store (override em_caldb_path before this file loads to relocate).
if {![info exists em_caldb_path]} { set em_caldb_path [file join $dspath db calibration.db] }
settingsdb::init $em_caldb_path

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# enable error logging
errormon enable

namespace eval biquadratic {
    namespace import ::math::linearalgebra::*
    
    proc create_design_matrix {x_coords y_coords} {
        set n [llength $x_coords]
        set A {}
        
        for {set i 0} {$i < $n} {incr i} {
            set x [lindex $x_coords $i]
            set y [lindex $y_coords $i]
            
            set x2 [expr {$x * $x}]
            set y2 [expr {$y * $y}]
            set xy [expr {$x * $y}]
            set x2y [expr {$x2 * $y}]
            set xy2 [expr {$x * $y2}]
            set x2y2 [expr {$x2 * $y2}]
            
            lappend A [list 1.0 $x $y $x2 $y2 $xy $x2y $xy2 $x2y2]
        }
        return $A
    }
    
    proc fit_single {x_coords y_coords target_values} {
        set n [llength $x_coords]
        set A [create_design_matrix $x_coords $y_coords]
        
        set z_vector {}
        foreach z $target_values {
            lappend z_vector [list $z]
        }
        
        if {$n > 9} {
            set At [transpose $A]
            set AtA [matmul $At $A]
            set Atz [matmul $At $z_vector]
            set coeffs [solveGauss $AtA $Atz]
        } else {
            set coeffs [solveGauss $A $z_vector]
        }
        
        set coeff_list {}
        foreach row $coeffs {
            lappend coeff_list [lindex $row 0]
        }
        return $coeff_list
    }
    
    proc fit {raw_x raw_y target_x target_y} {
        set x_coeffs [fit_single $raw_x $raw_y $target_x]
        set y_coeffs [fit_single $raw_x $raw_y $target_y]
        return [list $x_coeffs $y_coeffs]
    }
}

namespace eval em {
    proc do_fit { raw_x raw_y target_x target_y } {
	lassign [biquadratic::fit $raw_x $raw_y $target_x $target_y] x_coeffs y_coeffs
	set_bq_h_coeffs $x_coeffs
	set_bq_v_coeffs $y_coeffs
    }

    # Parameters for converting from raw Purkinje reflection differences to degrees
    # Linear model: degrees = scale * (raw_diff - raw_center)
    # For biquadratic: add higher-order terms after running calibration
    variable settings [dict create \
        scale_h 1.0 \
        scale_v 1.0 \
        raw_center_h 0.0 \
        raw_center_v 0.0 \
        invert_h 0 \
        invert_v 0 \
        swap_axes 0 \
        use_biquadratic 0 \
        source auto \
        bq_h_coeffs {0 0 0 0 0 0 0 0 0} \
        bq_v_coeffs {0 0 0 0 0 0 0 0 0}]
    
    # Track current raw values for "set center" functionality
    variable current_raw_h 0.0
    variable current_raw_v 0.0
    
    # Track last valid position in degrees
    variable last_valid_h 0.0
    variable last_valid_v 0.0

    # ---- which eye source may publish ------------------------------------
    #
    # Every processor here writes the SAME three datapoints
    # (eyetracking/position, eyetracking/raw, ess/em_pos), so with more than one
    # source live it is last-writer-wins at whatever rate they run. That was
    # harmless while only ever one source existed at a time, and it is the
    # reason this worked so smoothly: you looked at the eye viewer and saw
    # whichever one was plugged in, without having to say so.
    #
    # DEFAULT auto KEEPS EXACTLY THAT. auto lets every processor publish, so
    # nothing changes for a rig with one source. Naming a source pins it, for
    # when a box publishing an analog group called `eye` and a VideoStream
    # tracker are on one dserv and would otherwise race at 250 Hz.
    #
    #   ::em::set_source auto      any source publishes (default, permissive)
    #   ::em::set_source video     VideoStream tracker only (eyetracking/results)
    #   ::em::set_source analog    extio analog group only (state/ain/eye)
    #   ::em::set_source virtual   virtual eye only
    variable source_names {auto video analog virtual}

    proc source_allows { name } {
        variable settings
        set s [dict get $settings source]
        return [expr {$s eq $name || $s eq "auto"}]
    }

    #
    # THE SOURCE IS DECLARED, not learned.
    #
    # It used to ride in the calibration db, because set_param persists
    # everything -- and the comment in load_calibration below has been
    # explaining the consequence ever since: unlike a gain or a centre, a
    # pinned source can make a rig read the wrong eye, or no eye, for a
    # reason someone set days ago with no comment attached to it. That is
    # the obs_autobind incident exactly, and the settings layer exists for
    # it (docs/settings_panel_plan.md, local/rig.tcl).
    #
    # So set_source is now a RUNTIME override on the declared knob:
    #
    #   flipping the panel selector (virtual <-> real, twenty times an
    #   afternoon) = runtime. It shows amber in the settings gear, ↺ drops
    #   it, and a restart forgets it -- which is what a test flip should do.
    #
    #   making it the rig's answer = `settings::put eye source <v> -persist`,
    #   i.e. choosing it in the gear. That writes local/rig.tcl, where a
    #   comment can sit beside it and a diff can show it.
    #
    # One path, so em/settings and settings/eye/source can never disagree.
    #
    proc set_source { s } {
        ::settings::put eye source $s      ;# validates; -apply does the work
        return $s
    }

    # The -apply end of that: what actually moves the value into the dict
    # every processor gates on. Called for a runtime put, a persisted put,
    # and a reload, so there is one place where a source change happens.
    proc source_apply { s } {
        set_param source $s
        puts "em: eye source = $s"
        return $s
    }

    # WHICH SOURCE IS ACTUALLY WRITING, published on CHANGE only -- the whole
    # point of `auto` is not having to declare it, so the cost of that
    # convenience should be an observable rather than a mystery. A viewer can
    # show it, and a rig that suddenly reads the wrong eye says so here instead
    # of looking merely wrong. On change only: these run at up to 250 Hz.
    #
    # Called at each processor's PUBLISH SITE, never at its entry. A processor
    # can be entered and then bail (analog returns on nchan < 2, video on a
    # bad frame), and marking on entry claimed a source was feeding the rig
    # when it was producing nothing -- the exact misreading this exists to
    # prevent.
    #
    # THE DATAPOINT FLICKERS ON PURPOSE. Two live sources under `auto` alternate
    # per sample, so em/source_active alternates with them -- and that flicker
    # is the clearest possible sign that you need to choose between them. It is
    # kept.
    #
    # The LOG LINE must not flicker with it. One puts per change meant a line
    # per sample once two sources competed -- hundreds a second, drowning the
    # log at exactly the moment it is worth reading. So the line is throttled to
    # 1 Hz, and repeated flipping is reported as CONTENDED with a count rather
    # than as a stream of "now coming from" lines that each look like news.
    #
    # No contention FLAG is published from here. Contention is a RATE, and a
    # rate needs a clock to decay -- this interp has no event loop, so a flag
    # set on a flip could only be cleared by another flip. It would latch on
    # after the contention stopped, and a burst shorter than the throttle
    # window would never set it at all (measured: 5 flips accumulated, flag
    # never fired). The consumer that does have a clock is the page, so it
    # watches em/source_active change and decides for itself.
    variable source_last ""
    variable source_flips 0
    variable source_log_t 0
    proc source_mark { name } {
        variable source_last; variable source_flips
        variable source_log_t
        if { $name eq $source_last } return
        set prev $source_last
        set source_last $name
        dservSet em/source_active $name
        incr source_flips

        # no `after` in this interp (there is no Tcl event loop -- it would be
        # silently inert), so the throttle is a synchronous clock comparison
        set now [clock milliseconds]
        set dt [expr {$now - $source_log_t}]
        if { $source_log_t != 0 && $dt < 1000 } return

        if { $source_flips > 2 } {
            puts "em: eye source CONTENDED -- $source_flips changes in ${dt} ms\
 (now '$name', was '$prev'). More than one source is publishing;\
 ::em::set_source video|analog|virtual to pick one."
        } else {
            puts "em: eye data now coming from '$name'"
        }
        set source_flips 0
        set source_log_t $now
    }

    proc update_settings {} {
        variable settings
        dservSet em/settings $settings
    }

    # Set param values in our settings directory by name
    proc set_param {param_name value} {
        variable settings
        dict set settings $param_name $value
        update_settings
        save_calibration        ;# stable per-setup values persist immediately
    }

    # Persist / restore the whole calibration dict (opaque to settingsdb). Load
    # MERGES over the compiled defaults, so adding a new setting key later still
    # reads an old db cleanly (default fills the gap). Per-rig profile "default".
    variable cal_profile default
    # The db is the LEARNED half: gains, centres, inversions, the biquadratic
    # coefficients -- values measured against a subject, which a human should
    # never be typing. `source` is not one of those and is excluded here, so
    # the store can no longer pin a rig's eye behind everyone's back. (The
    # slider's cal_keys made this same exclusion for the same reason; em
    # persisted the whole dict and paid for it.)
    variable db_exclude { source }

    # NOTHING PERSISTS BEFORE THE DB HAS BEEN READ.
    #
    # set_param saves on every call (above), and the declaration at the
    # bottom of this namespace ends with a boot-time `source_apply`, which
    # goes through set_param. So for one commit (23b90904) every boot wrote
    # the COMPILED DEFAULTS over this rig's stored calibration a few lines
    # BEFORE load_calibration read it back -- and read back the defaults it
    # had just written. Gains and centres measured against a subject were
    # gone at the next restart, silently, because the save and the load both
    # "worked". (2026-08-22)
    #
    # The guard, rather than a reordering: the declaration has to come after
    # the procs it names, load_calibration has to come after the declaration
    # (it can adopt an old db's pinned source into local/rig.tcl), and any
    # future apply-at-boot would walk into the same hole. This makes the
    # rule the code states: a save before the load is a save of things
    # nobody measured.
    variable cal_loaded 0

    proc save_calibration {} {
        variable settings; variable cal_profile; variable db_exclude
        variable cal_loaded
        if { !$cal_loaded } return
        set d $settings
        foreach k $db_exclude { catch { dict unset d $k } }
        catch { ::settingsdb::save eye $d $cal_profile }
    }
    proc load_calibration {} {
        variable settings; variable cal_profile; variable db_exclude
        variable cal_loaded
        set stored [::settingsdb::load eye $cal_profile]
        if { $stored eq "" } { set cal_loaded 1; return }

        # A source pinned in an OLDER db is carried over into local/rig.tcl,
        # once, and then ignored here forever. The rig keeps behaving the way
        # it did across the upgrade -- the value simply moves to where a
        # human can see it, which is the whole migration (obs_autobind did
        # exactly this, via source_of).
        if { [dict exists $stored source] } {
            set old [dict get $stored source]
            if { $old ne "auto" && [::settings::source_of eye source] eq "default" } {
                if { [catch { ::settings::put eye source $old -persist } err] } {
                    puts stderr "em: could not adopt pinned source '$old': $err"
                } else {
                    puts "em: adopted the db's pinned eye source '$old' into\
                          local/rig.tcl (setting eye source $old) -- it is a\
                          rig DECLARATION, not a measurement"
                }
            }
            foreach k $db_exclude { catch { dict unset stored $k } }
        }

        set settings [dict merge $settings $stored]
        set cal_loaded 1
        update_settings
        puts "em: eye calibration restored (profile $cal_profile)"

        # A pinned source is still worth announcing -- it can make a rig read
        # the wrong eye, or no eye -- but it is no longer a mystery: it now
        # says WHERE it was declared, and the settings gear shows the same
        # thing with its provenance badge.
        set s [dict get $settings source]
        if { $s ne "auto" } {
            puts "em: NOTE eye source is PINNED to '$s'\
                  ([::settings::source_of eye source]) -- other sources will\
                  not publish. ::em::set_source auto restores the permissive\
                  default for this session; the gear persists it."
        }
    }
    
    proc set_scale_h {s} { set_param scale_h $s }
    proc set_scale_v {s} { set_param scale_v $s }
    proc set_raw_center_h {o} { set_param raw_center_h $o }
    proc set_raw_center_v {o} { set_param raw_center_v $o }
    proc set_invert_h {o} { set_param invert_h $o }
    proc set_invert_v {o} { set_param invert_v $o }
    proc set_use_biquadratic {b} { set_param use_biquadratic $b }
    proc set_bq_h_coeffs {coeffs} { 
        if {[llength $coeffs] != 9} {
            error "bq_h_coeffs requires 9 coefficients"
        }
        set_param bq_h_coeffs $coeffs 
    }
    proc set_bq_v_coeffs {coeffs} { 
        if {[llength $coeffs] != 9} {
            error "bq_v_coeffs requires 9 coefficients"
        }
        set_param bq_v_coeffs $coeffs 
    }

    # Set current eye position as center - call this when subject is fixating center
    proc set_current_as_center {} {
        variable current_raw_h
        variable current_raw_v
        
        set_raw_center_h $current_raw_h
        set_raw_center_v $current_raw_v
    }

    # Convert raw sub-pixel difference to degrees using full biquadratic fit
    # deg = c0 + c1*x + c2*y + c3*x² + c4*y² + c5*xy + c6*x²y + c7*xy² + c8*x²y²
    proc biquadratic_transform {x y coeffs} {
        lassign $coeffs c0 c1 c2 c3 c4 c5 c6 c7 c8
        set x2 [expr {$x * $x}]
        set y2 [expr {$y * $y}]
        set xy [expr {$x * $y}]
        return [expr {$c0 + $c1*$x + $c2*$y + $c3*$x2 + $c4*$y2 + $c5*$xy + 
                      $c6*$x2*$y + $c7*$x*$y2 + $c8*$x2*$y2}]
    }

    proc process { dpoint data } {
        if { ![source_allows video] } return
        variable settings
        variable last_valid_h
        variable last_valid_v
        variable current_raw_h
        variable current_raw_v

        lassign $data frame_id frame_time pupil_x pupil_y pupil_r \
            p1_x p1_y p4_x p4_y \
            blink p1_detected p4_detected

        # Get timestamp to use for all the eye data
        set cur_t [now]

        # Store raw detection data as floats (sub-pixel positions)
        set pupil "$pupil_x $pupil_y"
        set p1 "$p1_x $p1_y"
        set p4 "$p4_x $p4_y"
        foreach v "pupil p1 p4" {
            set fvals [binary format "ff" [set ${v}_x] [set ${v}_y]]
            dservSetData em/$v $cur_t 2 $fvals  ;# type 2 = DSERV_FLOAT
        }
        set fvals [binary format "f" $pupil_r]
        dservSetData em/pupil_r $cur_t 2 $fvals

        set frame_id_binary [binary format i [expr {int($frame_id)}]]
        dservSetData em/frame_id $cur_t 5 $frame_id_binary

        set seconds_binary [binary format d $frame_time]
        dservSetData em/time $cur_t 3 $seconds_binary
                      
        foreach v "blink p1_detected p4_detected" {
            set val [binary format c [expr {int([set $v])}]]
            dservSetData em/$v $cur_t 0 $val
        }
        
        # Only compute eye position if BOTH reflections detected
        dict with settings {
            if {$p1_detected > 0 && $p4_detected > 0} {
                # Calculate raw eye position in sub-pixels (P1-P4 difference)
                set raw_h [expr {$invert_h ? ($p1_x-$p4_x) : ($p4_x-$p1_x)}]
                set raw_v [expr {$invert_v ? ($p1_y-$p4_y) : ($p4_y-$p1_y)}]
                
                # Store for "set center" functionality
                set current_raw_h $raw_h
                set current_raw_v $raw_v
                
                # Convert to degrees
                if {$use_biquadratic} {
                    # Biquadratic fit: deg = f(raw_h, raw_v)
                    set h_deg [biquadratic_transform $raw_h $raw_v $bq_h_coeffs]
                    set v_deg [biquadratic_transform $raw_h $raw_v $bq_v_coeffs]
                } else {
                    # Simple linear mapping: degrees = scale * (raw - center)
                    # scale is in degrees per sub-pixel
                    set h_deg [expr {$scale_h * ($raw_h - $raw_center_h)}]
                    set v_deg [expr {$scale_v * ($raw_v - $raw_center_v)}]
                }
                
                # Remember valid position
                set last_valid_h $h_deg
                set last_valid_v $v_deg
            } else {
                # Use last valid position (hold during blinks)
                set h_deg $last_valid_h
                set v_deg $last_valid_v
            }
        }
        
        # Send eye position in degrees as floats [h, v] convention
        set eyevals [binary format ff $h_deg $v_deg]
        source_mark video
        dservSetData eyetracking/position $cur_t 2 $eyevals ;# 2 = DSERV_FLOAT
        
        # Also send raw differences for calibration/debugging
        set rawvals [binary format ff $current_raw_h $current_raw_v]
        dservSetData eyetracking/raw $cur_t 2 $rawvals
        
        # Send to visualization/monitoring
        dservSet ess/em_pos "$h_deg $v_deg"
    }    

    # Process virtual eye data (already in degrees)
    proc process_virtual { dpoint data } {
        if { ![source_allows virtual] } return
        lassign $data h_deg v_deg

        # Virtual data is already in degrees, just pass through
        source_mark virtual
        set eyevals [binary format ff $h_deg $v_deg]
        dservSetData eyetracking/position 0 2 $eyevals

	# We don't have real "raw" values so just use actual
        dservSetData eyetracking/raw 0 2 $eyevals

        # Also send to visualization
        dservSet ess/em_pos "$h_deg $v_deg"
    }

    # Process an extio MCP3204 analog eye source: an eye tracker's analog output
    # digitized by the box (state/ain/<label>). The raw ADC counts ARE the raw
    # eye position -- the same role P1-P4 sub-pixel difference plays for the
    # camera -- so they run through the SAME calibration (set-center + scale, or
    # biquadratic). Decoded via lib/extio-1.0.tm. CH0 -> h, CH1 -> v.
    # TIMESTAMPS COME FROM THE BOX, NOT FROM `now`, and EVERY scan in the block
    # is emitted -- see the two notes below. Both matter for the same reason:
    # this is the path by which eye position reaches the data file, and a sample
    # is only as useful as the instant it claims to have happened at.
    proc process_analog { dpoint data } {
        if { ![source_allows analog] } return
        variable settings
        variable current_raw_h
        variable current_raw_v

        set d    [::extio::ain_decode $data]
        set n    [dict get $d nchan]
        set s    [dict get $d samples]
        if { $n < 2 } return                   ;# need an h and a v
        set rows [expr {[llength $s] / $n}]
        if { $rows < 1 } return

        # ---- WHEN: the box's own sample time, on dserv's timeline ----
        #
        # `now` was dserv's clock AT CALLBACK TIME, which carries the transport
        # delay from the box plus however long this interp took to get to the
        # frame -- measured on officepi 2026-08-11 as ordinary 1-5 ms with
        # intermittent ~200 ms spikes. That jitter landed directly in the eye
        # timestamps, and it is exactly what the box's hardware-paced pipeline
        # exists to remove: sample times there are trigger-clock arithmetic good
        # to about a microsecond.
        #
        # It also put eye data and OBS BOUNDARIES on two different clocks --
        # state/in_obs is published with the box's stamp, so aligning eye to obs
        # was comparing a hardware instant against a receipt instant. The frame's
        # own timestamp is the box's t0 mapped to dserv time, so both now come
        # from the same place.
        set t0  [dservTimestamp $dpoint]
        # From the block, not from a nominal rate: the box's trigger clock runs
        # off an RC oscillator ~0.3% away from nominal, so counting samples at
        # "250 Hz" walks ~14 ms off across a 5 s trial.
        set ivl [dict get $d interval_us]

        # ---- WHAT: every scan, not just the newest ----
        #
        # This used ain_latest, which returns ONLY the last row. At batch 1 that
        # is every sample and nothing is lost -- but batching is the standard
        # tool for cutting the frame rate (`ain group G batch 10`), and with it
        # this silently kept one sample in ten. The block is self-describing and
        # says how many scans it carries; honour it, so reducing frame rate stays
        # a bandwidth decision rather than a data-loss one.
        #
        # Settings are read ONCE, ahead of the loop, rather than per row. Same
        # argument as the `dict get` vs `dict with` note this replaces (dict with
        # copies all 10 keys in and writes them back; measured 5.18 us against
        # 0.91 us on a Pi 5) -- it just now also has to not repeat per scan.
        set swap [dict get $settings swap_axes]
        set bq   [dict get $settings use_biquadratic]
        if { $bq } {
            set bqh [dict get $settings bq_h_coeffs]
            set bqv [dict get $settings bq_v_coeffs]
        } else {
            set sh [dict get $settings scale_h]
            set sv [dict get $settings scale_v]
            set ch [dict get $settings raw_center_h]
            set cv [dict get $settings raw_center_v]
            set ih [dict get $settings invert_h]
            set iv [dict get $settings invert_v]
        }

        for { set k 0 } { $k < $rows } { incr k } {
            set i     [expr {$k * $n}]
            set raw_h [lindex $s $i]
            set raw_v [lindex $s [expr {$i + 1}]]
            # stick orientation: swap CH0<->CH1 before any calibration, so
            # center/scale/invert and set_current_as_center all act on the
            # intended axes.
            if { $swap } { lassign [list $raw_v $raw_h] raw_h raw_v }

            if { $bq } {
                set h_deg [biquadratic_transform $raw_h $raw_v $bqh]
                set v_deg [biquadratic_transform $raw_h $raw_v $bqv]
            } else {
                set h_deg [expr {$sh * ($raw_h - $ch)}]
                set v_deg [expr {$sv * ($raw_v - $cv)}]
                if { $ih } { set h_deg [expr {-$h_deg}] }
                if { $iv } { set v_deg [expr {-$v_deg}] }
            }

            set t [expr {$t0 + $k * $ivl}]
            source_mark analog
            dservSetData eyetracking/position $t 2 [binary format ff $h_deg $v_deg]
            dservSetData eyetracking/raw      $t 2 [binary format ff $raw_h $raw_v]
        }

        # The NEWEST scan is what "where is the eye now" means: the center-capture
        # helper and the scalar ess/em_pos both want the current position, not a
        # replay. Published once per block rather than per scan -- em_pos is a
        # level, and at batch 10 per-scan writes would be nine redundant sets.
        set current_raw_h $raw_h               ;# feeds set_current_as_center (post-swap)
        set current_raw_v $raw_v
        dservSet ess/em_pos "$h_deg $v_deg"
    }

    update_settings

    # Declared last, so `source_names` and the procs it names already exist.
    # `auto` is the permissive default and the reason most rigs never declare
    # this at all: every source publishes and the last writer wins, which is
    # fine while only one is live. A rig that must read ONE eye says so here
    # rather than in a db nobody reads.
    ::settings::declare eye source -default auto \
        -values $source_names \
        -doc "which eye source may publish. auto lets every live source
write (fine while only one is); video, analog or virtual PINS
one and silences the others. Flipping this from the Eye panel
is a session override -- it shows as `runtime` in the settings
gear and a restart forgets it. Persist it here to make it the
rig's answer." \
        -apply {::em::source_apply}

    # And take whatever that resolved to, once, so a declared source is in
    # force from boot rather than from the first flip.
    catch { source_apply [::settings::get eye source] }
}

# restore any saved calibration for this rig (overrides the compiled defaults)
em::load_calibration

dservAddExactMatch eyetracking/virtual
dpointSetScript    eyetracking/virtual em::process_virtual

dservAddExactMatch eyetracking/results
dpointSetScript    eyetracking/results em::process

# extio MCP3204 analog eye source: an eye tracker's analog out digitized by the
# box, published as state/ain/<label>. Glob dev follows whatever box is present.
# Label your box's analog group "eye" (ain group N label eye), or declare a
# different label via `eye ain_group` (below).
# NB: this writes eyetracking/position, same as the other sources. With
# em/settings source == auto (the default) they ALL publish and it is
# last-writer-wins, which is fine while only one is live; ::em::set_source
# video|analog|virtual pins one when they are not.
#
# WHICH analog group is the eye is a rig declaration, not a constant. The
# label was hardwired here, so a box whose group is called something else
# (or a rig with two analog groups) had to have this file edited. It is the
# same shape as `eye source`: declared, pickable, and -- because ess can
# enumerate and CAPTURE analog streams -- fillable by wiggling the input
# rather than by typing its name.
#
# The subscription is by LABEL with a wildcard box, which is what makes it
# self-activating: define the group on any box and it comes alive with no
# rig-side binding to keep in step. Changing the label re-subscribes.
#
namespace eval em {
    variable ain_dp ""

    proc ain_group_apply { g } {
        variable ain_dp
        set g [string trim $g]
        if { $g eq "" } { return }
        set want extio/*/state/ain/$g
        if { $want eq $ain_dp } { return }
        if { $ain_dp ne "" } {
            catch { dpointRemoveScript $ain_dp em::process_analog }
            catch { dservRemoveMatch   $ain_dp }
        }
        set ain_dp $want
        dservAddMatch   $ain_dp
        dpointSetScript $ain_dp em::process_analog
        puts "em: analog eye group = $g ($ain_dp)"
        return
    }
}

::settings::declare eye ain_group -default eye \
    -candidates analog \
    -doc "the extio analog group carrying the eye signal, by LABEL --
extio/<any box>/state/ain/<this>. Wildcard box on purpose: define
the group on whichever box is present and it comes alive. Only
consulted when the eye source is analog (or auto)." \
    -apply {::em::ain_group_apply}

::em::ain_group_apply [::settings::get eye ain_group]

puts "Eye movement subprocessor started"
