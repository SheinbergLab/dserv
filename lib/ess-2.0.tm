# -*- mode: tcl -*-

package require dlsh
package provide ess 2.0
package require yajltcl
package require dslog

catch {System destroy}

#
# System class
#
oo::class create System {
    variable _systemname
    variable _protocolname
    variable _variantname
    variable _status
    variable _start_state
    variable _end_state
    variable _current_state
    variable _states
    variable _params
    variable _vars
    variable _default_param_vals
    variable _state_ms
    variable _callbacks
    variable _evt_info
    variable _evt_type_ids
    variable _evt_type_names
    variable _evt_subtype_ids
    variable _evt_subtype_names
    variable _evt_ptype_ids
    variable _state_time
    variable _variants
    variable _variant_args
    variable _version

    constructor { name } {
        set _systemname $name
        set _protocolname {}
        set _variantname {}
        set _status stopped
        set _start_state {}
        set _end_state {}
        set _states [dict create]
        set _evt_info $::ess::evt_info
        set _evt_type_ids $::ess::evt_type_ids
        set _evt_type_names $::ess::evt_type_names
        set _evt_subtype_ids $::ess::evt_subtype_ids
        set _evt_subtype_names $::ess::evt_subtype_names
        set _evt_ptype_ids $::ess::evt_ptype_ids
        set _vars {}
        set _variants {}
        set _variant_args {}
        set _version 0.0
    }

    destructor { my deinit }

    method name { } { return $_systemname }
    method get_system { } { return $_systemname }

    method set_version { v } { set _version $v }
    method get_version { } { return $_version }

    method set_protocol { p } { set _protocolname $p }
    method get_protocol { } { return $_protocolname }

    method set_variants { vdict } { set _variants $vdict }
    method get_variants { } { return $_variants }

    method set_variant { v } { set _variantname $v }
    method get_variant { } { return $_variantname }

    method reset_variant_args {} { set _variant_args {} }
    method set_variant_args { vdict } {
        set _variant_args [dict merge  $_variant_args $vdict]
    }
    method get_variant_args { } { return $_variant_args }

    method add_variant { name method params } {
        dict set _variants $name [list $method $params]
    }

    method configure_stim { host } {
        variable current
        foreach var "screen_halfx screen_halfy screen_w screen_h" {
            oo::objdefine [self] variable $var
        }

        if { ![rmtOpen $host] } {
            # not connected but set some defaults
            variable screen_halfx 16.0
            variable screen_halfy 9.0
            variable screen_w 1024
            variable screen_h 600
            return 0
        }

        variable screen_halfx [rmtSend "screen_set HalfScreenDegreeX"]
        variable screen_halfy [rmtSend "screen_set HalfScreenDegreeY"]
        set scale_x [rmtSend "screen_set ScaleX"]
        set scale_y [rmtSend "screen_set ScaleY"]
        variable screen_w [expr int([rmtSend "screen_set WinWidth"]/$scale_x)]
        variable screen_h [expr int([rmtSend "screen_set WinHeight"]/$scale_y)]
        if { $screen_halfx == "" } {
            variable screen_halfx 16.0
            variable screen_halfy 9.0
            variable screen_w 1024
            variable screen_h 600
        }

        foreach v "halfx halfy w h" { dservSet ess/screen_${v} [set screen_${v}] }

        rmtSend "set dservhost [dservGet ess/ipaddr]"

        set rmtcmd {
            # connect to data server receive stimdg updates
            package require qpcs
            qpcs::dsStimRegister $dservhost
            qpcs::dsStimAddMatch $dservhost stimdg

            # to stimdg is sent as b64 encoded string, this proc unpacks into stim
            proc readdg { args } {
                dg_fromString64 [lindex $args 4]
            }

            # this sets the callback upon receipt of stimdg
            set ::dsCmds(stimdg) readdg

            namespace inscope :: {
                proc onMousePress {} {
                    global dservhost
                    dl_local coords [dl_create short $::MouseXPos $::MouseYPos]
                    qpcs::dsSetData $dservhost mtouch/touchvals $coords
                }
            }
        }

        rmtSend $rmtcmd

        # source this protocol's stim functions
        set stimfile [file join [set ::ess::system_path] [set ::ess::project] \
                $_systemname $_protocolname ${_protocolname}_stim.tcl]
        if { ![catch {set f [open $stimfile]}] } {
            set script [read $f]
            close $f
            rmtSend $script
        }
        return 1
    }

    method update_stimdg {} {
        if { [dg_exists stimdg] } {
            dg_toString stimdg s
            dservSetData stimdg [now] 6 $s
        }
    }

    method set_init_callback { cb } {
        oo::objdefine [self] method init_cb {} $cb
        set _callbacks(init) init_cb
    }

    method set_deinit_callback { cb } {
        oo::objdefine [self] method deinit_cb {} $cb
        set _callbacks(deinit) deinit_cb
    }

    method set_protocol_init_callback { cb } {
        oo::objdefine [self] method protocol_init_cb {} $cb
        set _callbacks(protocol_init) protocol_init_cb
    }

    method set_final_init_callback { cb } {
        oo::objdefine [self] method final_init_cb {} $cb
        set _callbacks(final_init) final_init_cb
    }
    
    method set_protocol_deinit_callback { cb } {
        oo::objdefine [self] method protocol_deinit_cb {} $cb
        set _callbacks(protocol_deinit) protocol_deinit_cb
    }

    method set_start_callback { cb } {
        oo::objdefine [self] method start_cb {} $cb
        set _callbacks(start) start_cb
    }

    method set_end_callback { cb } {
        oo::objdefine [self] method end_cb {} $cb
        set _callbacks(end) end_cb
    }

    method set_reset_callback { cb } {
        oo::objdefine [self] method reset_cb {} $cb
        set _callbacks(reset) reset_cb
    }

    method set_quit_callback { cb } {
        oo::objdefine [self] method quit_cb {} $cb
        set _callbacks(quit) quit_cb
    }

    method set_file_suggest_callback { cb } {
        oo::objdefine [self] method file_suggest_cb { filename } $cb
        set _callbacks(file_suggest) file_suggest_cb
    }

    method set_file_open_callback { cb } {
        oo::objdefine [self] method file_open_cb { filename } $cb
        set _callbacks(file_open) file_open_cb
    }

    method set_file_close_callback { cb } {
        oo::objdefine [self] method file_close_cb { filename } $cb
        set _callbacks(file_close) file_close_cb
    }

    method set_subject_callback { cb } {
        oo::objdefine [self] method subject_cb {} $cb
        set _callbacks(subject) subject_cb
    }

    method get_states {} {
        return [dict keys $_states]
    }

    method add_state { name args } {
        if { [llength $args] > 0 } {
            oo::objdefine [self] method ${name}_a {} [lindex $args 0]
        }
        if { [llength $args] > 1 } {
            oo::objdefine [self] method ${name}_t {} [lindex $args 1]
        }
        set s [dict create name $name action ${name}_a transition ${name}_t]
        dict set _states $name $s
        if { $name eq "start" } {
            set _start_state $s
            my reset
        }
        if { $name eq "end" } {
            set _end_state $s
        }
        return
    }

    method add_action { name action } {
        if { ![dict exist _states $name] } { my add_state $name }
        oo::objdefine [self] method ${name}_a {} $action
    }

    method add_transition { name transition } {
        if { ![dict exist _states $name] } { my add_state $name }
        oo::objdefine [self] method ${name}_t {} $transition
    }

    method set_start { state } {
        my add_state start {}  [list return $state]
    }

    method set_end { action } {
        my add_state end $action {}
    }

    method do_action {} {
        dservSet ess/action_state [dict get $_current_state name]_a
        set state_time [now]
        my [dict get $_current_state name]_a
    }

    method do_transition {} {
        dservSet ess/transition_state [dict get $_current_state name]_t
        my [dict get $_current_state name]_t
    }

    method set_params { p } {
        set _params $p
        foreach var [dict keys $_params] vals [dict values $_params] {
            my add_variable $var [lindex $vals 0]
        }
    }

    method set_param { var val } {
        if { [dict exists $_params $var] } {
            lassign [dict get $_params $var] oldval type ptype
            dict set _params $var "$val $type $ptype"
            my add_variable $var $val
            return $oldval
        }
        return
    }

    method get_params {} {
        return $_params
    }

    method get_param { p } {
        return [dict get $_params $p]
    }

    method set_default_param_vals {} {
        dict for { var val } $_default_param_vals {
            my set_param $var $val
        }
    }

    method add_param { pname val type ptype } {
        set t [dict get $::ess::param_types [string toupper $type]]
        dict set _params $pname [list $val $t $ptype]
        dict set _default_param_vals $pname $val
        my variable $pname
        oo::objdefine [self] variable $pname
        set $pname $val
    }

    method set_parameters {} {
        dict for { var vals } $_params { set $var [lindex $vals 0] }
    }

    method add_method { name params script } {
        oo::objdefine [self] method $name $params $script

        # export method (even if starts with upper case)
        oo::objdefine [self] export $name
    }

    method add_variable { var { val {} } } {
        set pos [lsearch $_vars $var]
        if { $pos < 0 } {
            my variable $var
            lappend _vars $var
            oo::objdefine [self] variable $var
        }
        if { $val != {} } {
            variable $var $val
        }
    }

    method get_variable { var } {
        return [set [self namespace]::$var]
    }

    method set_variable { var val } {
        return [set [self namespace]::$var $val]
    }

    method init {} {
        if { [info exists _callbacks(init)] } {
            my $_callbacks(init)
        }
    }

    method deinit {} {
        if { [info exists _callbacks(deinit)] } {
            my $_callbacks(deinit)
        }
    }

    method protocol_init {} {
	if { [info exists _callbacks(protocol_init)] } {
	    my $_callbacks(protocol_init)
	}
    }
    
    method protocol_deinit {} {
	if { [info exists _callbacks(protocol_deinit)] } {
	    my $_callbacks(protocol_deinit)
        }
    }
    
    method final_init {} {
	if { [info exists _callbacks(final_init)] } {
	    my $_callbacks(final_init)
        }
    }
    
    method start {} {
        if { $_status != "stopped" } return
        my set_status running

        ::ess::evt_put SYSTEM_STATE RUNNING [now]

        if { [info exists _callbacks(start)] } {
            my $_callbacks(start)
        }
        set _current_state $_start_state
        my update
    }

    method reset {} {
        if { $_status != "running" } {
            set _current_state $_start_state
            if { [info exists _callbacks(reset)] } {
                my $_callbacks(reset)
            }
        }
    }

    method stop {} {
        my set_status stopped
        ::ess::evt_put SYSTEM_STATE STOPPED [now]
        if { [info exists _callbacks(quit)] } {
            my $_callbacks(quit)
        }
        return
    }

    method end {} {
        my set_status stopped
        ::ess::evt_put SYSTEM_STATE STOPPED [now]
        if { [info exists _callbacks(end)] } {
            my $_callbacks(end)
        }
        return
    }

    method file_suggest {} {
        if { $_status != "stopped" } return
        if { [info exists _callbacks(file_suggest)] } {
            return [my $_callbacks(file_suggest)]
        }
        return
    }

    method file_open { f } {
        if { $_status != "stopped" } return
        if { [info exists _callbacks(file_open)] } {
            my $_callbacks(file_open) $f
        }
        return
    }

    method file_close { f } {
        if { $_status != "stopped" } return
        if { [info exists _callbacks(file_close)] } {
            my $_callbacks(file_close) $f
        }
        return
    }

    method set_subject {} {
        if { $_status != "stopped" } return
        if { [info exists _callbacks(subject)] } {
            my $_callbacks(subject)
        }
        return
    }

    method status {} { return $_status }

    method set_status { status } {
    dservSet ess/status $status
    set _status $status
    }

    method update {} {
        if { $_status eq "running" } {
            while { [set next [my do_transition]] != {} &&
                [dict exists $_states $next] } {
                    set _current_state [dict get $_states $next]
                    my do_action
                    if { $_current_state == $_end_state } {
                        return [my end]
                    }
                }
            return
        }
    }
}

#
# ess namespace
#
namespace eval ess {
    variable current

    set current(state_system) {}
    set current(open_system) 0
    set current(open_protocol) 0
    set current(open_variant) 0

    set current(project) {}
    set current(system) {}
    set current(protocol) {}
    set current(variant) {}

    set current(trialid) 0

    variable param_types { ANY 0 TIME 1 VARIABLE 2 SUBJECT 3 STORAGE 4 STIM 5 }
    variable param_datatypes { null 0 char 1 short 2 long 3 \
                   int 3 bool 3 float 4 \
                   double 5 long_array 6 \
                   float_array 7 string 8 }
    variable evt_datatypes { unknown 0 null 1 string 2 short 3 \
                 long 4 float 5 double 6 }
    variable open_datafile {}
    variable subject_id {}
    variable subject_ids {}

    # for tracking begin/end obs and sync'ing with external systems
    variable in_obs 0
    variable obs_pin 26

    variable em_active 0
    variable touch_active 0

    # if this is set, there is a callback associated with errorInfo to set the ess/errorInfo dpoint
    variable error_trace 0
    
    proc version_string {} {
        if {[dservExists ess/git/branch] && [dservExists ess/git/tag]} {
            set branch [dservGet ess/git/branch]
            set tag [dservGet ess/git/tag]
        } else {
            set branch default
            set tag 0.0.0
        }
        return "$branch-$tag"
    }

    proc create_system {name} {
        # only allow one instance of a system with this name
        foreach s [info class instances System] {
            if {[$s name] == $name} {
                $s destroy
            }
        }
        set s [System new $name]
        $s set_version [version_string]

        return $s
    }

    proc clear_systems {name} {
        foreach s [info class instances System] {
            if {[$s name] == $name} {
                $s destroy
            }
        }
    }

    proc find_system {name} {
        foreach s [info class instances System] {
            if {[$s name] == $name} {
                return $s
            }
        }
    }

    proc system_version {} {
        variable current
        if {$current(state_system) != ""} {
            return [$current(state_system) get_version]
        } else {
            return ""
        }
    }

    proc save_script { type s } {
	variable current
        if { $current(system) == "" } return

	set f [file join $::ess::system_path $current(project) $current(system)]
	if { $type == "system" } {
	    set saveto [file join $f $current(system).tcl]
	} elseif { $type == "protocol" } {
	    set saveto [file join $f $current(protocol) $current(protocol).tcl]
	} elseif { $type == "loaders" } {
	    set saveto [file join $f $current(protocol) ${current(protocol)}_loaders.tcl]
	} elseif { $type == "variants" } {
	    set saveto [file join $f $current(protocol) ${current(protocol)}_variants.tcl]
	} elseif { $type == "stim" } {
	    set saveto [file join $f $current(protocol) ${current(protocol)}_stim.tcl]
	}

	# could do some backup or versioning here
	set f [open $saveto w]
	puts $f $s
	close $f
    }
    
    proc system_script {} {
        variable current
        if {$current(system) != ""} {
            set f [file join $::ess::system_path $current(project) $current(system)]
            if {
                [file isdirectory $f] &&
                [file exists $f/[file tail $f].tcl]
            } {
                set fname [file join $f [file tail $f].tcl]
                set script_file [open $fname r]
                set script [read $script_file]
                close $script_file
                return $script
            }
        }
    }

    proc protocol_script {} {
        variable current
        if {$current(system) != "" && $current(protocol) != ""} {
            set f [file join $::ess::system_path $current(project) \
                $current(system) $current(protocol)]
            if {
                [file isdirectory $f] &&
                [file exists $f/[file tail $f].tcl]
            } {
                set fname [file join $f [file tail $f].tcl]
                set script_file [open $fname r]
                set script [read $script_file]
                close $script_file
                return $script
            }
        }
    }

    proc loaders_script {} {
        variable current
        if {
            $current(system) != "" &&
            $current(protocol) != "" && $current(variant) != ""
        } {
            set f [file join $::ess::system_path $current(project) \
                $current(system) $current(protocol) $current(protocol)_loaders.tcl]
            if {[file exists $f]} {
                set script_file [open $f r]
                set script [read $script_file]
                close $script_file
                return $script
            }
        }
    }

    proc variants_script {} {
        variable current
        if {$current(system) != "" && $current(protocol) != "" && $current(variant) != ""} {
            set f [file join $::ess::system_path $current(project) \
                $current(system) $current(protocol) $current(protocol)_variants.tcl]
            if {[file exists $f]} {
                set script_file [open $f r]
                set script [read $script_file]
                close $script_file
                return $script
            }
        }
    }


    proc stim_script {} {
        variable current
        if {$current(system) != "" && $current(protocol) != "" && $current(variant) != ""} {
            set f [file join $::ess::system_path $current(project) \
                $current(system) $current(protocol) $current(protocol)_stim.tcl]
            if {[file exists $f]} {
                set script_file [open $f r]
                set script [read $script_file]
                close $script_file
                return $script
            }
        }
    }


    proc unload_system {} {
        variable current
        set sname $current(system)

        if {$current(open_system)} {
            set s [find_system $sname]

            # call variant_deinit if exists
            set vdeinit_method $current(variant)_deinit

            if {[lsearch [info object methods $s] $vdeinit_method] != -1} {
                $s $vdeinit_method
            }

            # protocol deinit callback
            $s protocol_deinit

            # system deinit callback
            $s deinit

            set current(open_system) 0
            set current(system) {}
        }
    }

    proc error_callback { name element op } {
	dservSet ess/errorInfo [set $name]
    }

    proc error_trace { } {
	variable error_trace
	if { $error_trace } { return }
	trace add variable ::errorInfo write ::ess::error_callback
	set error_trace 1
    }

    proc error_untrace { } {
	variable error_trace
	if { !$error_trace } { return }
	trace remove variable ::errorInfo write ::ess::error_callback
	set error_trace 0
    }
    
    proc load_system {{system {}} {protocol {}} {variant {}}} {
        # Reset the result so we can check if a new error is raised
        set ::errorInfo ""
        set ::errorCode NONE

        variable current

        if {$current(system) != {} && [info exists $current(system)]} {
            if {[query_state] == "running"} {return}
            catch {unload_system}
        }

        set systems [find_systems]

        # store these so client interfaces can know what protocols we have
        dservSet ess/systems $systems

        if {$system == ""} {
            set current(system) [lindex $systems 0]
        } else {
            if {[lsearch $systems $system] < 0} {
                error "system $system not found"
            }
            set current(system) $system
        }

        [set current(system)]::create

        system_init $current(system)

        set protocols [find_protocols $current(system)]

        # store these so client interfaces can know what protocols we have
        dservSet ess/protocols $protocols

        if {$protocol == ""} {
            set current(protocol) [lindex $protocols 0]
        } else {
            if {[lsearch $protocols $protocol] < 0} {
                error "protocol $protocol not found in $current(system)"
            }
            set current(protocol) $protocol
        }

        set variants [find_variants $current(system) $current(protocol)]

        # store these so client interfaces can know what protocols we have
        dservSet ess/variants $variants

        if {$variant == ""} {
            set current(variant) [lindex $variants 0]
        } else {
            if {[lsearch $variants $variant] < 0} {
                set s_p "$current(system)/$current(protocol)"
                error "variant $variant not found in $s_p"
            }
            set current(variant) $variant
        }

	# setup protocol
        protocol_init $current(system) $current(protocol)

        # set the variant for the current system
        set s [find_system $current(system)]
        $s set_variant $current(variant)

        # this resets any unsaved variant args
        $s reset_variant_args

        # load saved settings
        load_settings

        # initialize the loaders by calling the appropriate loader
        variant_init $current(system) $current(protocol) $current(variant)

	# one final init now that protocol and variant info set
	$s final_init

        # dictionary of states and associated transition states
        dservSet ess/state_table [get_state_transitions]

        # set list of rmtSend commands used by this protocol
        dservSet ess/rmt_cmds [get_rmt_cmds]

        # update current scripts in dserv
        foreach t "system protocol loaders variants stim" {
            dservSet ess/${t}_script [${t}_script]
        }

        set current(trialid) 0
        if {[dg_exists trialdg]} {dg_delete trialdg}
        reset_trial_info
    }

    proc reload_system {} {
        variable current
        if {$current(system) == {} || $current(protocol) == {} || $current(variant) == {}} {
            if {[query_state] == "running"} {return}
            return
        }
        load_system $current(system) $current(protocol) $current(variant)
    }

    proc reload_protocol {} {
        variable current
        if {$current(system) == {} || $current(protocol) == {}} {
            if {[query_state] == "running"} {return}
            return
        }
        load_system $current(system) $current(protocol)
    }

    proc reload_variant {} {
        variable current
        if {$current(system) == {} || $current(protocol) == {} || $current(variant) == {}} {
            if {[query_state] == "running"} {return}
            return
        }

        # initialize the variant by calling the appropriate loader
        variant_init $current(system) $current(protocol) $current(variant)

        # reset counters
        reset

        set current(trialid) 0
        if {[dg_exists trialdg]} {dg_delete trialdg}
        reset_trial_info
    }

    proc set_system {system} {
        variable current
        catch {unload_system}
        set s [find_system $system]
        if {$s == ""} {
            error "system $system not found"
        }
        $s init
        set current(system) $system
        set current(state_system) $s
    }

    proc set_subject {subj} {
        variable subject_id
        set subject_id [string tolower $subj]
        ::ess::evt_put ID SUBJECT [now] $subject_id

        # load subject specific settings and reload
        load_settings
        reload_variant
    }

    proc set_subjects {args} {
        variable subject_ids
        set subject_ids {*}[string tolower $args]
        dservSet ess/subject_ids $subject_ids
    }

    proc add_subject {subj} {
        variable subject_ids
        if {[lsearch $subject_ids $subj] < 0} {
            lappend subject_ids $subj
        }
        dservSet ess/subject_ids $subject_ids
    }

    proc add_subjects {args} {
        variable subject_ids
        foreach subj $args {
            if {[lsearch $subject_ids $subj] < 0} {
                lappend subject_ids $subj
            }
        }
        dservSet ess/subject_ids $subject_ids
    }

    proc get_subject {} {
        variable subject_id
        dservTouch ess/subject_id
        return $subject_id
    }

    proc get_subjects {} {
        variable subject_ids
        dservTouch ess/subject_ids
        return $subject_ids
    }

    proc init {} {
        dservRemoveAllMatches
        dpointRemoveAllScripts
        for {set i 0} {$i < $::nTimers} {incr i} {
            timerSetScript $i "[namespace current]::do_update"
        }
        # default to not tracking ems, but ::ess::em_init will turn on
        variable em_active
        set em_active 0

        # default to not tracking touch, but ::ess::touch_init will turn on
        variable touch_active
        set touch_active 0
    }

    proc do_update {args} {
        variable current
        if {$current(state_system) != ""} {
            $current(state_system) update
        }
    }

    proc start {} {
        variable current
        if {$current(system) == ""} {print "no system set"; return}
        set current(state_system) [find_system $current(system)]
        if {$current(state_system) != ""} {
            ::ess::evt_put USER START [now]
            $current(state_system) start
            dservSet ess/user_start 1
        }
        return
    }

    proc stop {} {
        variable current
        if {$current(system) == ""} {print "no system set"; return}
        set current(state_system) [find_system $current(system)]
        if {$current(state_system) != ""} {
            $current(state_system) stop
            ::ess::evt_put USER QUIT [now]
            dservSet ess/user_quit 1
        }
        return
    }

    proc reset {} {
        variable current
        if {$current(system) == ""} {print "no system set"; return}
        set current(state_system) [find_system $current(system)]
        if {$current(state_system) != ""} {
            $current(state_system) reset
            ::ess::evt_put USER RESET [now]
            dservSet ess/user_reset 1
        }
        return
    }

    proc set_obs_pin {pin} {
        variable obs_pin
        set obs_pin $pin
    }

    proc begin_obs {current total} {
        variable in_obs
        variable obs_pin
        rpioPinOn $obs_pin
        ::ess::evt_put BEGINOBS INFO [now] $current $total
        set in_obs 1
        dservSet ess/in_obs 1
    }

    proc create_trialdg {status rt {stimid {}}} {
        variable current
        variable subject_id

        if {[dg_exists trialdg]} {dg_delete trialdg}
        dg_create trialdg
        set g trialdg

        dl_set $g:trialid [dl_ilist $current(trialid)]
        dl_set $g:project [dl_slist $current(project)]
        dl_set $g:system [dl_slist $current(system)]
        dl_set $g:protocol [dl_slist $current(protocol)]
        dl_set $g:variant [dl_slist $current(variant)]
        dl_set $g:version [dl_slist [$current(state_system) get_version]]
        dl_set $g:subject [dl_slist $subject_id]
        dl_set $g:status [dl_ilist $status]
        dl_set $g:rt [dl_ilist $rt]

        if {$stimid != {}} {
            if {[dg_exists stimdg]} {
                set rlen [dl_length stimdg:stimtype]
                foreach l [dg_tclListnames stimdg] {
                    if {$rlen == [dl_length stimdg:$l]} {
                        dl_set $g:$l [dl_choose stimdg:$l $stimid]
                    }
                }
            }
        }
        return $g
    }

    proc append_trialdg {status rt {stimid {}}} {
        variable current
        variable subject_id
        set g trialdg
        dl_append $g:trialid $current(trialid)
        dl_append $g:project $current(project)
        dl_append $g:system $current(system)
        dl_append $g:protocol $current(protocol)
        dl_append $g:variant $current(variant)
        dl_append $g:version [$current(state_system) get_version]
        dl_append $g:subject $subject_id
        dl_append $g:status $status
        dl_append $g:rt $rt

        if {$stimid != {}} {
            if {[dg_exists stimdg]} {
                set rlen [dl_length stimdg:stimtype]
                foreach l [dg_tclListnames stimdg] {
                    if {$rlen == [dl_length stimdg:$l]} {
                        dl_append $g:$l [dl_get stimdg:$l $stimid]
                    }
                }
            }
        }
        return $g
    }

    proc create_trial_json {status rt {stimid {}}} {
        variable current
        variable subject_id
        variable open_datafile
        set obj [yajl create #auto]
        set parseobj [yajl create #auto]
        $obj map_open

        # always include these
        $obj string trialid number $current(trialid)
        $obj string project string $current(project)
        $obj string system string $current(system)
        $obj string protocol string $current(protocol)
        $obj string variant string $current(variant)
        $obj string version string [$current(state_system) get_version]
        $obj string subject string $subject_id
        $obj string status number $status
        $obj string rt number $rt
        $obj string filename string $open_datafile

        # if a trial id is supplied, add addition info
        if {$stimid != {}} {
            if {[dg_exists stimdg]} {
                $obj map_key stiminfo

                $obj map_open
                set rlen [dl_length stimdg:stimtype]
                foreach l [dg_tclListnames stimdg] {
                    $parseobj reset
                    if {$rlen == [dl_length stimdg:$l]} {
                        set dtype [dl_datatype stimdg:$l]
                        if {$dtype == "list"} {
                            $obj string $l {*}[$parseobj parse [dl_toJSON stimdg:$l:$stimid]]
                        } elseif {$dtype == "long" || $dtype == "short" || $dtype == "char"} {
                            $obj string $l integer [dl_get stimdg:$l $stimid]
                        } elseif {$dtype == "float"} {
                            $obj string $l double [dl_get stimdg:$l $stimid]
                        } elseif {$dtype == "string"} {
                            $obj string $l string [dl_get stimdg:$l $stimid]
                        }
                    }
                }
                $obj map_close
            }
        }

        $obj map_close
        set result [$obj get]
        $obj delete
        $parseobj delete
        return $result
    }

    proc reset_trial_info {} {
        dservClear ess/block_n_complete
        dservClear ess/block_pct_complete
        dservClear ess/block_pct_correct
        dservClear trialdg
    }

    proc save_trial_info {status rt {stimid {}}} {
        variable current
        if {![dg_exists trialdg]} {
            create_trialdg $status $rt $stimid
        } else {
            append_trialdg $status $rt $stimid
        }

        # save some basic block info
        set n_trials [dl_length trialdg:status]
        dservSet ess/block_n_trials $n_trials

        dl_local completed_status [dl_select trialdg:status [dl_noteq trialdg:status -1]]
        set n_complete [dl_length $completed_status]
        dservSet ess/block_n_complete $n_complete

        dservSet ess/block_pct_complete [expr {double($n_complete) / $n_trials}]
        dservSet ess/block_pct_correct [dl_mean $completed_status]

        # store trialdg in dserv
        dg_toString trialdg s
        dservSetData trialdg [now] 6 $s

        set trial_json [create_trial_json $status $rt $stimid]
        dservSetData ess/trialinfo [now] 11 $trial_json
        incr current(trialid)
    }

    proc end_obs {{status 1}} {
        variable in_obs
        variable obs_pin
        if {!$in_obs} {return}
        rpioPinOff $obs_pin
        ::ess::evt_put ENDOBS $status [now]
        set in_obs 0
        dservSet ess/in_obs 0
    }

    proc query_state {} {
        variable current
        return [$current(state_system) status]
    }

    proc query_system {} {
        variable current
        return $current(system)
    }

    proc query_system_by_index {index} {
        set sysnames [find_systems]
        if {$index >= [llength $sysnames]} {error {}}
        return [dict get $systems [lindex $sysnames $index]]
    }

    proc query_system_name {} {
        variable current
        return $current(system)
    }

    proc query_remote {} {
        return [rmtName]
    }

    proc query_system_name_by_index {index} {
        variable systems
        set sysnames [dict keys $systems]
        if {$index >= [llength $sysnames]} {error {}}
        return [lindex $sysnames $index]
    }

    proc query_param {index type} {
        variable current
        variable param_datatypes
        set params [$current(state_system) get_params]
        set inds [lsearch -index 1 -all [dict values $params] $type]
        if {$index >= [llength $inds]} {error {}}
        set actual_index [lindex $inds $index]
        set name [lindex [dict keys $params] $actual_index]
        set vals [lindex [dict values $params] $actual_index]
        set dtypeinfo [lindex $vals 2]
        set colon [string first : $dtypeinfo]
        if {$colon >= 0} {
            set dtypename [string range $dtypeinfo 0 [expr {$colon - 1}]]
        } else {
            set dtypename $dtypeinfo
        }
        set dtype [dict get $param_datatypes $dtypename]
        return "{$name} {[lindex $vals 0]} [expr {$index + 1}] $type $dtype"
    }

    proc get_params {} {
        variable current
        $current(state_system) get_params
    }

    proc get_param_vals {} {
        variable current
        set params [$current(state_system) get_params]
        set param_vals [dict create]
        dict for {k v} $params {
            dict set param_vals $k [lindex $v 0]
        }
        return $param_vals
    }

    proc get_param {p} {
        variable current
        $current(state_system) get_param $p
    }

    proc set_param {param val} {
        variable current
        $current(state_system) set_param $param $val
        ::ess::evt_put PARAM NAME [now] $param
        ::ess::evt_put PARAM VAL [now] $val

        # set param datapoint for clients
        dservSet ess/params [get_param_vals]
    }

    proc set_params {args} {
        variable current

        set nargs [llength $args]
        if {$nargs % 2 != 0} {
            error "invalid param/setting args ($args)"
        }
        for {set i 0} {$i < $nargs} {incr i 2} {
	    set pname [lindex $args $i]
	    set pval  [lindex [lindex $args [expr {$i + 1}]] 0]
	    $current(state_system) set_param $pname $pval
	    ::ess::evt_put PARAM NAME [now] $pname
	    ::ess::evt_put PARAM VAL [now] $pval
	    
	    # set param datapoint for clients
	    dservSet ess/params [get_param_vals]
        }
    }

    proc get_variable { name } {
        variable current
        return [set $current(state_system)::$name]
    }

    proc evt_put {type subtype time args} {
        variable current
        if {[string is int $type]} {
            set type_id $type
        } else {
            set type_id [dict get [set $current(state_system)::_evt_type_ids] $type]
        }
        if {[string is int $subtype]} {
            set subtype_id $subtype
        } else {
            set subtype_id [dict get [set $current(state_system)::_evt_subtype_ids] \
                $type $subtype]
        }
        set ptype [dict get [set $current(state_system)::_evt_ptype_ids] $type]
        evtPut $type_id $subtype_id $time $ptype {*}$args
    }

    proc evt_id {type {subtype {}}} {
        variable current
        if {[string is int $type]} {
            set type_id $type
        } else {
            set type_id [dict get [set $current(state_system)::_evt_type_ids] $type]
        }
        if {$subtype == {}} {return $type_id}

        if {[string is int $subtype]} {
            set subtype_id $subtype
        } else {
            set subtype_id [dict get [set $current(state_system)::_evt_subtype_ids] \
                $type $subtype]
        }
        return "$type_id $subtype_id"
    }

    proc store_evt_names {} {
        variable current

        # output the current event names
        dict for { k v } [set $current(state_system)::_evt_type_names] {
            set ptype_ids [set $current(state_system)::_evt_ptype_ids]
            if {[dict exists $ptype_ids $v]} {
                set p [dict get $ptype_ids $v]
            } else {
                set p 0
            }
            ::ess::evt_name_set $k $v $p
        }
        dict for { k v } [set $current(state_system)::_evt_info] {
            set subinfo [lindex $v 3]
            if {$subinfo != ""} {
                set id [lindex $v 0]
                ::ess::evt_put SUBTYPES $id [now] $subinfo
            }
        }
    }

    proc evt_name_set {type name ptype} {
        variable evt_datatypes
        if {[string is int $ptype]} {
            set ptype_id $ptype
        } else {
            set ptype_id [dict get $evt_datatypes $ptype]
        }
        evtNameSet $type $name $ptype_id
    }

    proc open_files {} {
        # get all open files
        set open_files [dservLoggerClients]

        # find any .ess files
        set essfiles [lsearch -all -inline -glob $open_files *.ess]

        return $essfiles
    }

    proc file_suggest {} {
        variable current
        variable subject_id
        set suggestion [$current(state_system) file_suggest]
        if {$suggestion == ""} {
            set ts [clock format [clock seconds] -format {%y%m%d%H%M}]
            return "${subject_id}_$current(system)-$current(protocol)-$current(variant)_${ts}"
        }
    }

    proc file_open {f {overwrite 0}} {
        variable current
        variable open_datafile
        variable subject_id
        variable data_dir

        # get all open files
        set open_files [::ess::open_files]

        if {$open_files != ""} {
            print "$open_files open: call ::ess::file_close"
            return -1
        }

        set filename [file join $data_dir $f.ess]
        if {!$overwrite} {
            if {[file exists $filename]} {
                print "file $f already exists in directory $data_dir"
                return 0
            }
        }
        dservLoggerOpen $filename 1
        set open_datafile $filename

        dservSet ess/datafile $f

        dservLoggerAddMatch $filename eventlog/events
        dservLoggerAddMatch $filename eventlog/names
        dservLoggerAddMatch $filename stimdg

        variable em_active
        if {$em_active} {
            # record raw em data: obs_limited, 80 byte buffer, every sample
            dservLoggerAddMatch $filename ain/vals 1 80 1
            dservLoggerAddMatch $filename em_coeffDG

	    if { [dservExists openiris/frameinfo] } {
		foreach side "right" {
		    foreach v "pupil cr1 cr4" {
			dservLoggerAddMatch $filename openiris/$side/$v 1 40 1
		    }
		}
		foreach v "frame time int0 int1" {
		    dservLoggerAddMatch $filename openiris/$v 1 40 1
		}
	    }
        }
        variable touch_active
        if {$touch_active} {
            dservLoggerAddMatch $filename mtouch/touchvals
        }

        # call the system's specific file_open callback
        #  to add matches
        $current(state_system) file_open $filename

        dservLoggerResume $filename

        # reset system and output event names
        ::ess::reset

        # output info for interpreting events
        ::ess::store_evt_names

        dservTouch stimdg

        ::ess::evt_put ID ESS [now] $current(system)
        ::ess::evt_put ID PROTOCOL [now] $current(system):$current(protocol)
        ::ess::evt_put ID VARIANT [now] $current(system):$current(protocol):$current(variant)
        ::ess::evt_put ID SUBJECT [now] $subject_id

        dict for { pname pval } [::ess::get_params] {
            ::ess::evt_put PARAM NAME [now] $pname
            ::ess::evt_put PARAM VAL [now] [lindex $pval 0]
        }

        #   dservTouch em_coeffDG
        return 1
    }

    proc file_close {} {
        variable current
        variable open_datafile
        if {$open_datafile != ""} {
            # could put pre_close callback here

            dservLoggerClose $open_datafile
	    set lastfile [file tail [file root $open_datafile]]
            dservSet ess/lastfile $lastfile
            dservSet ess/datafile {}

            # call the system's specific file_close callback
            if { [catch {$current(state_system) file_close $open_datafile} ioerror] } {
		error $ioerror
	    }

            # convert to other formats
	    if { [catch { file_load_ess $lastfile } result] } {
		error $result
	    } else {
		set g $result
	    }
            if { [catch {ess_to_dgz $lastfile $g} ioerror] } {
		dg_delete $g
		set open_datafile {}
		error $ioerror
	    }
            if { [catch {ess_to_json $lastfile $g} ioerror] } {
		dg_delete $g
		set open_datafile {}
		error $ioerror
	    }
            return 1
        }
        return 0
    }

    proc file_load_ess {f} {
	variable data_dir
        set infile [file join $data_dir $f.ess]
        set g [dslog::readESS $infile]
	return $g
    }
    
    proc ess_to_dgz {f g} {
	variable data_dir
        set dgz_dir [regsub essdat $data_dir dgzdat]
	set outfile [file join $dgz_dir $f.dgz]
	dg_write $g $outfile
    }
    
    proc ess_to_json {f g} {
        variable data_dir
        set json_dir [regsub essdat $data_dir jsondat]
        set outfile [file join $json_dir $f.json]
	
        set j [dg_toJSON $g]
        set out_f [open $outfile w]
        puts $out_f $j
        close $out_f
    }
}

###############################################################################
################################### joystick ##################################
###############################################################################
namespace eval ess {
    # if joystick status changes update ess/joystick/value and wake up ESS
    proc joystick_process_value {dpoint data} {
        dservSet ess/joystick/value $data
        do_update
    }

    proc joystick_process_button {dpoint data} {
        dservSet ess/joystick/button $data
        do_update
    }

    # call generic joystick initialization (dsconf.tcl) and add callback
    proc joystick_init {} {
        ::joystick_init

        dservAddExactMatch joystick/value
        dpointSetScript joystick/value ::ess::joystick_process_value
        dservSet ess/joystick/value 0

        dservAddExactMatch joystick/button
        dpointSetScript joystick/button ::ess::joystick_process_button
        dservSet ess/joystick/button 0
    }
}

###############################################################################
################################## em_windows #################################
###############################################################################

namespace eval ess {
    variable em_windows
    set em_windows(processor) "windows"
    set em_windows(dpoint) "proc/windows"

    proc em_update_setting {win} {
        ainSetIndexedParam $win settings 0
    }

    proc em_window_process {name data} {
        variable em_windows
        lassign $data \
            em_windows(changes) em_windows(states) \
            em_windows(hpos) em_windows(vpos)
        do_update
    }

    proc em_init {} {
        # set flag to ensure em's are stored in data files
        variable em_active
        set em_active 1

        variable em_windows
        ainSetProcessor $em_windows(processor)
        ainSetParam dpoint $em_windows(dpoint)
        dservAddExactMatch $em_windows(dpoint)/status
        dpointSetScript $em_windows(dpoint)/status ::ess::em_window_process

        for {set win 0} {$win < 8} {incr win} {
            ainSetIndexedParam $win active 0
        }
        set em_windows(states) 0

        # these should be loaded per subject
        variable em_scale_h 200
        variable em_scale_v 200
    }

    proc em_check_state {win} {
        variable em_windows
        set state [ainGetIndexedParam $win state]
        if {$state} {
            set em_windows(states) [expr {$em_windows(states) | (1 << $win)}]
        } else {
            set em_windows(states) [expr {$em_windows(states) & ~(1 << $win)}]
        }
        return $state
    }

    proc em_region_on {win} {
        ainSetIndexedParam $win active 1
        em_check_state $win
        em_update_setting $win
    }

    proc em_region_off {win} {
        ainSetIndexedParam $win active 0
        em_check_state $win
        em_update_setting $win
    }

    proc em_region_set {
        win type center_x center_y
        plusminus_x plusminus_y
    } {
        ainSetIndexedParams $win type $type \
            center_x $center_x center_y $center_y \
            plusminus_x $plusminus_x plusminus_y $plusminus_y
        em_check_state $win
        em_update_setting $win
    }

    proc em_fixwin_set {win cx cy r {type 1}} {
        set x [expr {int($cx * $::ess::em_scale_h) + 2048}]
        set y [expr {-1 * int($cy * $::ess::em_scale_v) + 2048}]
        set pm_x [expr {$r * $::ess::em_scale_h}]
        set pm_y [expr {$r * $::ess::em_scale_v}]
        em_region_set $win 1 $x $y $pm_x $pm_y
    }

    proc em_eye_in_region {win} {
        variable em_windows
        return [expr {($em_windows(states) & (1 << $win)) != 0}]
    }

    proc em_sampler_enable {nsamps {nchan 2} {slot 0}} {
        ainSamplerAdd $slot $nchan $nsamps
        dservSet ain/samplers/$slot/status 0
        dservAddExactMatch ain/samplers/$slot/status
        dpointSetScript ain/samplers/$slot/status \
            "[namespace current]::do_update"
    }

    proc em_sampler_start {{slot 0}} {
        ainSamplerStart $slot
    }

    proc em_sampler_status {{slot 0}} {
        return [dservGet ain/samplers/$slot/status]
    }

    proc em_sampler_vals {{slot 0}} {
        return [dservGet ain/samplers/$slot/vals]
    }
}


###############################################################################
########################## juicer/reward _support #############################
###############################################################################

namespace eval ess {
    # initially no juicer is connected
    variable current
    package require juicer
    set current(juicer) {}

    proc juicer_init {} {
        variable current
        if {$current(juicer) != {}} {$current(juicer) destroy}
        set j [Juicer new]
        if {[set jpath [$j find]] != {}} {
            $j set_path $jpath
            $j open
        } else {
            $j use_gpio
        }
        set current(juicer) $j
    }

    proc reward {ml} {
        variable current
        set j $current(juicer)
        if {$j == ""} {return}
        if {[$j using_gpio]} {
            # currently assume only a single juicer is configured
            juicerJuiceAmount 0 $ml
        } else {
            $j reward $ml
        }
    }
}
###############################################################################
################################ touch_windows ################################
###############################################################################

namespace eval ess {
    variable touch_windows
    set touch_windows(processor) "touch_windows"
    set touch_windows(dpoint) "proc/touch_windows"

    proc touch_update_setting {win} {
        touchSetIndexedParam $win settings 0
    }

    proc touch_window_process {name data} {
        variable touch_windows
        lassign $data \
            touch_windows(changes) touch_windows(states) \
            touch_windows(hpos) touch_windows(vpos)
        do_update
    }

    proc touch_reset {} {
        dservTouch mtouch/touchvals
    }

    proc touch_init {} {
        variable touch_windows
        variable current

        variable touch_active
        set touch_active 1

        touchSetProcessor $touch_windows(processor)
        touchSetParam dpoint $touch_windows(dpoint)
        dservAddExactMatch $touch_windows(dpoint)/status
        dpointSetScript $touch_windows(dpoint)/status ::ess::touch_window_process
        for {set win 0} {$win < 8} {incr win} {
            touchSetIndexedParam $win active 0
        }
        set touch_windows(states) 0

        set s $current(state_system)

        set w [set ${s}::screen_w]
        set h [set ${s}::screen_h]
        set w_deg [expr {2 * [set ${s}::screen_halfx]}]
        set h_deg [expr {2 * [set ${s}::screen_halfy]}]
        variable touch_win_w $w
        variable touch_win_h $h
        variable touch_win_half_w [expr {$w / 2}]
        variable touch_win_half_h [expr {$h / 2}]
        variable touch_win_deg_w $w_deg
        variable touch_win_deg_h $h_deg
        variable touch_win_scale_w [expr {$touch_win_w / $touch_win_deg_w}]
        variable touch_win_scale_h [expr {$touch_win_h / $touch_win_deg_h}]
    }

    proc touch_check_state {win} {
        variable touch_windows
        set state [touchGetIndexedParam $win state]
        if {$state} {
            set touch_windows(states) [expr {$touch_windows(states) | (1 << $win)}]
        } else {
            set touch_windows(states) [expr {$touch_windows(states) & ~(1 << $win)}]
        }
        return $state
    }

    proc touch_region_on {win} {
        touchSetIndexedParam $win active 1
        touch_check_state $win
        touch_update_setting $win
    }

    proc touch_region_off {win} {
        touchSetIndexedParam $win active 0
        touch_check_state $win
        touch_update_setting $win
    }

    proc touch_region_set {
        win type center_x center_y
        plusminus_x plusminus_y
    } {
        touchSetIndexedParams $win type $type \
            center_x $center_x center_y $center_y \
            plusminus_x $plusminus_x plusminus_y $plusminus_y
        touch_check_state $win
        touch_update_setting $win
    }

    proc touch_win_set {win cx cy r {type 1}} {
        set x [expr {
            int($cx * $::ess::touch_win_scale_w) +
            $::ess::touch_win_half_w
        }]
        set y [expr {
            -1 * int($cy * $::ess::touch_win_scale_h) +
            $::ess::touch_win_half_h
        }]
        set pm_x [expr {$r * $::ess::touch_win_scale_w}]
        set pm_y [expr {$r * $::ess::touch_win_scale_h}]
        touch_region_set $win $type $x $y $pm_x $pm_y
    }

    proc touch_in_win {win} {
        variable touch_windows
        set in [expr {($touch_windows(states) & (1 << $win)) != 0}]
        return $in
    }

    proc touch_in_region {win} {
        return [touch_in_win $win]
    }
}

###############################################################################
################################### states ####################################
###############################################################################


namespace eval ess {
    proc get_states {} {
        variable current
        if {$current(state_system) != ""} {
            return [$current(state_system) get_states]
        } else {
            return
        }
    }

    proc find_transitions {def states} {
        set returns [regexp -inline -all {return\s+\S+[^ \}]} $def]
        set matches [lmap a $returns {lindex $a 1}]
        return [lmap m $matches {expr {[lsearch $states $m] != -1 ? $m : [continue]}}]
    }

    proc get_state_transitions {} {
        variable current
        set s $current(state_system)
        if {$s == ""} {return}

        # store states+transitions in t
        set t [dict create]

        set states [$s get_states]
        set methods [info object methods $s]
        foreach state $states {
            set t_name ${state}_t
            if {[lsearch $methods $t_name] != -1} {
                dict set t $state \
                    [find_transitions [lindex [info object definition $s $t_name] 1] $states]
            }
        }
        return $t
    }
}


namespace eval ess {
    # not sure this gets all rmtSend's !!!
    proc find_rmt_cmds {def} {
        set pat {rmtSend\s+(\S+)[ \}\";\n]}
        set rmtcmds [regexp -inline -all $pat $def]
        set rmtcmds [regsub -all \" $rmtcmds {}]
        set rmtcmds [lmap a $rmtcmds {lindex $a 1}]
        set matches {}
        for {set i 0} {$i < [llength ${rmtcmds}] / 2} {incr i 2} {
            set cmd [string trimleft [lindex $rmtcmds $i] !]
            if {[lsearch $matches $cmd] == -1} {lappend matches $cmd}
        }
        return $matches
    }

    proc get_rmt_cmds {} {
        variable current
        set s $current(state_system)
        if {$s == ""} {return}

        # store cmds in a list
        set cmds {}

        set states [$s get_states]
        set methods [info object methods $s]
        foreach m $methods {
            set matches [find_rmt_cmds [lindex [info object definition $s $m] 1]]
            foreach m $matches {
                if {[lsearch $cmds $m] == -1} {lappend cmds $m}
            }
        }
        set cmds [lsort $cmds]
        set d [dict create]

        foreach c $cmds {
            set isproc [rmtSend [list info proc $c]]
            if {$isproc != ""} {
                dict set d $c [rmtSend [list info args $c]]
            }
        }
        return $d
    }
}

namespace eval ess {
    variable current

    set current(state_system) {}

    proc set_project {p} {
        # could remove current project/lib from module path...
        variable project
        variable current
        variable system_path
        set current(project) $p
        tcl::tm::add [file join $system_path $p lib]
    }

    #
    # system variables
    #
    #  ESS_SYSTEM_PATH: where to find systems
    #  ESS_RMT_HOST:    ip address of stim
    #  ESS_DATA_DIR:    folder to store raw data files
    #
    if {[info exists ::env(ESS_SYSTEM_PATH)]} {
        variable system_path $::env(ESS_SYSTEM_PATH)
    } else {
        variable system_path /usr/local/dserv/systems
    }

    if {[info exists ::env(ESS_RMT_HOST)]} {
        variable rmt_host $::env(ESS_RMT_HOST)
    } else {
        variable rmt_host localhost
    }

    if {[info exists ::env(ESS_DATA_DIR)]} {
        variable data_dir $::env(ESS_DATA_DIR)
    } else {
        variable data_dir /tmp/essdat
	if { ![file exists $data_dir] } {
	    file mkdir $data_dir
	}
    }

    set project "ess"
    set_project $project

    foreach v "system_path rmt_host data_dir project" {
        dservSet ess/$v [set $v]
    }
    dservSet ess/block_id 0

    proc system_init {system} {
        variable current

        set s [find_system $system]
        set current(state_system) $s

        # initialize the system
        $s init

        #::io::init
        #::screen::init

        # publish this system's event name table
        ::ess::store_evt_names
        ::ess::evt_put ID ESS [now] $system
        set current(open_system) 1
    }

    proc protocol_init {system protocol} {
        variable current
        set s [::ess::find_system $system]

        # initialize the protocol
        ::ess::${system}::${protocol}::protocol_init $current(state_system)

        # initialize this protocol's variants
        set vinfo [set ${system}::${protocol}::variants]
        ${s} set_variants $vinfo

        dict for { k v } $vinfo {
            foreach m "init deinit" {
                if {[dict exists $v $m]} {
                    ${s} add_method ${k}_${m} {} [dict get $v $m]
                }
            }
        }

        # initialize this protocol's loader methods
        ::ess::${system}::${protocol}::loaders_init $current(state_system)

	# call the system's protocol init function
	${s} protocol_init
	
	::ess::evt_put ID PROTOCOL [now] $current(system):$protocol
	set current(protocol) $protocol
	set current(open_protocol) 1
    }

    #
    # using variants, variants_defaults, variants_options to discover and share loader options
    #
    proc variant_loader_defaults {loader_arg_options} {
        set d [dict create]
        dict for {arg options} $loader_arg_options {
            dict set d $arg [lindex [lindex $options 0] 1]
        }
        return $d
    }

    proc variant_loader_info {system protocol variant} {
        set s [::ess::find_system $system]
        set vinfo [dict get [$s get_variants] $variant]
        set loader_proc [dict get $vinfo loader_proc]
        set description [dict get $vinfo description]

        # get all options for this variant
        set loader_arg_options [dict get $vinfo loader_options]

        # get names of args for loader_proc
        set loader_arg_names [lindex [info object definition $s $loader_proc] 0]

        # standardize options
        set d [dict create]
        foreach a $loader_arg_names {
            set opts [dict get $loader_arg_options $a]
            set olist {}
            foreach o $opts {
                # if there are two elements, we have name/value, otherwise just copy
                if {[llength $o] == 2} {
                    lappend olist $o
                } elseif {[llength $o] == 1} {
                    lappend olist [list $o $o]
                }
            }
            dict set d $a $olist
        }
        set loader_arg_options $d

        # choose defaults to get initial valid options
        set loader_variant_defaults [variant_loader_defaults $loader_arg_options]

        # merge the default and override args
        set loader_args_dict \
            [dict merge $loader_variant_defaults [$s get_variant_args]]

        # build list of args for this particular loader_proc
        set loader_args {}
        foreach a $loader_arg_names {
            lappend loader_args [dict get $loader_args_dict $a]
        }

        return [dict create \
            loader_proc $loader_proc \
            loader_args $loader_args \
            loader_arg_names $loader_arg_names \
            loader_arg_options $loader_arg_options]
    }

    proc variant_loader_command {system protocol variant} {
        set d [variant_loader_info $system $protocol $variant]
        return "[dict get $d loader_proc] [dict get $d loader_args]"
    }

    proc set_variant_args {vargs} {
        variable current
        if {$current(system) == {}} {return}
        set s [::ess::find_system $current(system)]
        return [$s set_variant_args $vargs]
    }

    proc get_variant_args {} {
        variable current
        set s [::ess::find_system $current(system)]
        set vinfo [dict get [$s get_variants] $current(variant)]
        set loader_proc [dict get $vinfo loader_proc]

        # get all options for this variant
        set loader_arg_options [dict get $vinfo loader_options]

        # get names of args for loader_proc
        set loader_arg_names [lindex [info object definition $s $loader_proc] 0]

        # standardize options
        set d [dict create]
        foreach a $loader_arg_names {
            set opts [dict get $loader_arg_options $a]
            set olist {}
            foreach o $opts {
                # if there are two elements, we have name/value, otherwise just copy
                if {[llength $o] == 2} {
                    lappend olist $o
                } elseif {[llength $o] == 1} {
                    lappend olist [list $o $o]
                }
            }
            dict set d $a $olist
        }
        set loader_arg_options $d

        # choose defaults to get initial valid options
        set loader_variant_defaults [variant_loader_defaults $loader_arg_options]

        # merge the default and override args
        set loader_args_dict \
            [dict merge $loader_variant_defaults [$s get_variant_args]]
        return $loader_args_dict
    }

    proc save_settings {} {
        variable current
        variable subject_id
        if {
            $current(system) == {} ||
            $current(protocol) == {} ||
            $current(variant) == {}
        } {return}

        set vargs [get_variant_args]
        set param_settings [get_params]

        # locate settings file and create if necessary
        set path [file join $::ess::system_path $current(project)]
        set owner [file attributes $path -owner]
        set group [file attributes $path -group]
        set uid [exec whoami]
        set folder [file join $path $current(system) $current(protocol)]
        set settings_file [file join $folder $current(protocol)_settings.tcl]
        if {![file exists $settings_file]} {
            close [open $settings_file w]
            # change ownership to match owner of systems folder
            if {$owner != $uid} {
                file attributes $settings_file -owner $owner -group $group
            }
        }

        set new_settings \
            [dict create variant_args $vargs param_settings $param_settings]

        set f [open $settings_file r]
        set contents [read $f]
        close $f

        set settings_dict [dict create {*}$contents]
        set name ${subject_id}@$current(system)_$current(protocol)_$current(variant)
        set new_settings_dict [dict merge \
            $settings_dict \
            [dict create $name $new_settings]]
        set f [open $settings_file w]
        puts $f $new_settings_dict
        close $f
    }

    proc load_settings {} {
        variable current
        variable subject_id
        if {
            $current(system) == {} ||
            $current(protocol) == {} ||
            $current(variant) == {}
        } {return}
        set path [file join $::ess::system_path $current(project)]
        set folder [file join $path $current(system) $current(protocol)]
        set settings_file [file join $folder $current(protocol)_settings.tcl]
        if {![file exists $settings_file]} {return}
        set f [open $settings_file r]
        set contents [read $f]
        close $f
        set settings_dict [dict create {*}$contents]
        set name ${subject_id}@$current(system)_$current(protocol)_$current(variant)
        if {[dict exists $settings_dict $name]} {
            set s [dict get $settings_dict $name]
            set vargs [dict get $s variant_args]
            set param_settings [dict get $s param_settings]
            set_variant_args $vargs
            set_params {*}$param_settings
        }
    }

    proc reset_settings {} {
        variable current
        variable subject_id
        if {
            $current(system) == {} ||
            $current(protocol) == {} ||
            $current(variant) == {}
        } {return}
        set path [file join $::ess::system_path $current(project)]
        set folder [file join $path $current(system) $current(protocol)]
        set settings_file [file join $folder $current(protocol)_settings.tcl]
        if {![file exists $settings_file]} {return}

        set f [open $settings_file r]
        set contents [read $f]
        close $f
        set settings_dict [dict create {*}$contents]

        set name \
            ${subject_id}@$current(system)_$current(protocol)_$current(variant)

        if {[dict exists $settings_dict $name]} {
            set settings_dict [dict unset settings_dict $name]
        }

        set f [open $settings_file w]
        puts $f $settings_dict
        close $f

        # reset param_settings and variant argument
        set s [::ess::find_system $current(system)]
        $s set_default_param_vals
        $s reset_variant_args
    }

    proc variant_init {system protocol variant} {
        variable current
        set s [::ess::find_system $system]

        # let clients know we are loading a new set of trials
        $current(state_system) set_status loading

        # get loader info for this variant and call
        $s {*}[variant_loader_command $system $protocol $variant]

        # push new stimdg to dataserver
        $s update_stimdg

        # share new variant options to dataserver and other programs
        dservSet ess/variant_info [variant_loader_info $system $protocol $variant]

        # protocol defaults for system parameters
        set param_default_settings ${system}::${protocol}::params_defaults
        if {[info exists $param_default_settings]} {
            ::ess::set_params {*}[set $param_default_settings]
        }

        # and update system parameters for this variant
        set vinfo [dict get [$s get_variants] $variant]
        if {[lsearch [dict keys $vinfo] params] != -1} {
            set param_settings [dict get $vinfo params]
            ::ess::set_params {*}$param_settings
        }

        # call a specific init function for this variant
        set vinit_method ${variant}_init
        if {[lsearch [info object methods $s] $vinit_method] != -1} {
            $s $vinit_method
        }

        dservSet ess/param_settings [ess::get_params]

        ::ess::evt_put ID VARIANT [now] $system:$protocol:$variant
        set current(variant) $variant
        set current(open_variant) 1


        # loading is complete, so return status to stopped
        $current(state_system) set_status stopped
    }

    proc find_systems {} {
        variable current
        set systems {}
        foreach f [glob [file join $::ess::system_path $current(project) *]] {
            if {
                [file isdirectory $f] &&
                [file exists $f/[file tail $f].tcl]
            } {
		if { [namespace exists ::ess::[file tail $f]] } {
		    namespace delete ::ess::[file tail $f]
		}
                set fname [file join $f [file tail $f].tcl]
                source $fname
                lappend systems [file tail $f]
            }
        }
        return $systems
    }

    proc find_protocols {s} {
        variable current
        set protocols {}
        foreach f [glob [file join $::ess::system_path $current(project) $s *]] {
            if {
                [file isdirectory $f] &&
                [file exists $f/[file tail $f].tcl]
            } {
		if { [namespace exists ::ess::[file tail $f]] } {
		    namespace delete ::ess::${s}::[file tail $f]
		}
                source $f/[file tail $f].tcl
                lappend protocols [file tail $f]
            }
        }
        return $protocols
    }

    proc find_variants {s p} {
        variable current
        set loader_file [file join $::ess::system_path $current(project) $s ${p} ${p}_loaders.tcl]
        source $loader_file
        set variant_file [file join $::ess::system_path $current(project) $s ${p} ${p}_variants.tcl]
        source $variant_file
        return [dict keys [set ::ess::${s}::${p}::variants]]
    }

    # The system and protocol getters don't re-"source"
    proc get_systems {} {
        variable current
        set systems {}
        foreach f [glob [file join $::ess::system_path $current(project) *]] {
            if {
                [file isdirectory $f] &&
                [file exists $f/[file tail $f].tcl]
            } {
                lappend systems [file tail $f]
            }
        }
        return $systems
    }

    proc get_protocols {system} {
        variable current
        set protocols {}
        foreach f [glob [file join $::ess::system_path $current(project) $system *]] {
            if {
                [file isdirectory $f] &&
                [file exists $f/[file tail $f].tcl]
            } {
                lappend protocols [file tail $f]
            }
        }
        return $protocols
    }

    proc get_variants {sysname protocol} {
        return [find_variants $sysname $protocol]
    }

    proc get_system_dict {} {
        variable current
        set d [dict create]
        foreach s [get_systems] {
            set sdict [dict create]
            if {$s == $current(system)} {
                foreach p [get_protocols $s] {
                    dict set sdict $p [get_variants $s $p]
                }
            }
            dict set d $s $sdict
        }
        return $d
    }

    proc system_dict_to_json {d} {
        set obj [yajl create #auto]
        $obj map_open
        dict for {sysname sysinfo} $d {
            $obj string $sysname array_open
            dict for {prot variants} $sysinfo {
                $obj map_open string $prot array_open
                foreach v $variants {
                    $obj string $v
                }
                $obj array_close map_close
            }
            $obj array_close
        }
        $obj map_close
        set result [$obj get]
        $obj delete
        return $result
    }

    proc get_system_json {} {
        return [system_dict_to_json [get_system_dict]]
    }

    proc get_system_status {} {
        variable current
        variable in_obs

        set s $current(state_system)
        if {$s == ""} {return}

        set obj [yajl create #auto]
        $obj map_open
        $obj string system string $current(system)
        $obj string protocol string $current(protocol)
        $obj string variant string $current(variant)
        $obj string state string [$s status]
        $obj string in_obs string $in_obs
        $obj map_close
        return [$obj get]
    }
}


namespace eval ess {
    namespace export create_system set_system
    namespace export init
    namespace export begin_obs end_obs init evt_put
    namespace export start stop reset
    namespace export query_state query_system query_system_by_index
    namespace export query_system_name query_system_name_by_index
    namespace export query_param get_params set_param query_remote
    namespace export evt_name_set
    namespace export file_open file_close
    namespace ensemble create
}

namespace eval ess {
    variable evt_info
    variable evt_type_ids
    variable evt_type_names
    variable evt_subtype_ids
    variable evt_subtypes_names
    variable evt_ptype_ids
    variable evt_datatypes

    # 6-15      Reserved
    # 16-127    System events
    # 128-255   User events

    dict set evt_info MAGIC [list 0 {Magic Number} null]
    dict set evt_info NAME [list 1 {Event Name} string]
    dict set evt_info FILE [list 2 {File I/O} string]

    set subtypes [dict create START 0 QUIT 1 RESET 2]
    dict set evt_info USER [list 3 {User Interaction} null $subtypes]

    set subtypes [dict create ACT 0 TRANS 1]
    dict set evt_info TRACE [list 4 {State System Trace} string]

    set subtypes [dict create NAME 0 VAL 1]
    dict set evt_info PARAM [list 5 {Parameter Set} string $subtypes]

    dict set evt_info SUBTYPES [list 6 {Subtype Names} string]

    set subtypes [dict create STOPPED 0 RUNNING 1 INACTIVE 2]
    dict set evt_info SYSTEM_STATE [list 7 {System State} string $subtypes]

    dict set evt_info FSPIKE [list 16 {Time Stamped Spike} long]
    dict set evt_info HSPIKE [list 17 {DIS-1 Hardware Spike} long]

    set subtypes [dict create ESS 0 SUBJECT 1 PROTOCOL 2 VARIANT 3]
    dict set evt_info ID [list 18 {Name} string $subtypes]

    set subtypes [dict create INFO 0]
    dict set evt_info BEGINOBS [list 19 {Start Obs Period} long $subtypes]

    set subtypes [dict create INCOMPLETE 0 COMPLETE 1 QUIT 2 ABORT 3]
    dict set evt_info ENDOBS [list 20 {End Obs Period} long $subtypes]

    dict set evt_info ISI [list 21 {ISI} long]
    dict set evt_info TRIALTYPE [list 22 {Trial Type} long]
    dict set evt_info OBSTYPE [list 23 {Obs Period Type} long]

    dict set evt_info EMLOG [list 24 {EM Log} long]

    set subtypes [dict create OFF 0 ON 1 SET 2]
    dict set evt_info FIXSPOT [list 25 {Fixspot} float $subtypes]

    set subtypes [dict create SCALE 0 CIRC 1 RECT 2 CALIB 3]
    dict set evt_info EMPARAMS [list 26 {EM Params} float $subtypes]

    set subtypes [dict create OFF 0 ON 1 SET 2]
    dict set evt_info STIMULUS [list 27 {Stimulus} long $subtypes]

    set subtypes [dict create OFF 0 ON 1 SWAP 2]
    dict set evt_info PATTERN [list 28 {Pattern} long $subtypes]

    set subtypes [dict create STIMID 1]
    dict set evt_info STIMTYPE [list 29 {Stimulus Type} long $subtypes]

    set subtypes [dict create OFF 0 ON 1]
    dict set evt_info SAMPLE [list 30 {Sample} long $subtypes]
    dict set evt_info PROBE [list 31 {Probe} long $subtypes]
    dict set evt_info CUE [list 32 {Cue} long $subtypes]
    dict set evt_info TARGET [list 33 {Target} long $subtypes]
    dict set evt_info DISTRACTOR [list 34 {Distractor} long $subtypes]
    dict set evt_info SOUND [list 35 {Sound Event} long $subtypes]
    dict set evt_info CHOICES [list 49 {Choices} long $subtypes]

    set subtypes [dict create OUT 0 IN 1 REFIXATE 2]
    dict set evt_info FIXATE [list 36 {Fixation} long $subtypes]

    set subtypes [dict create NONE 0 LEFT 1 RIGHT 2 BOTH 3]
    dict set evt_info RESP [list 37 {Response} long $subtypes]
    dict set evt_info SACCADE [list 38 {Saccade} long]

    set subtypes [dict create NONE 0 SELECT 1]
    dict set evt_info DECIDE [list 39 {Decide} long $subtypes]

    set subtypes [dict create INCORRECT 0 CORRECT 1 ABORT 2]
    dict set evt_info ENDTRIAL [list 40 {EOT} long $subtypes]

    set subtypes [dict create EYE 0 LEVER 1 NORESPONSE 2 STIM 3]
    dict set evt_info ABORT [list 41 {Abort} long $subtypes]

    set subtypes [dict create DURATION 0 TYPE 1 MICROLITERS 2]
    dict set evt_info REWARD [list 42 {Reward} long $subtypes]

    dict set evt_info DELAY [list 43 {Delay} long]
    dict set evt_info PUNISH [list 44 {Punish} long]

    dict set evt_info PHYS [list 45 {Physio Params} float]
    dict set evt_info MRI [list 46 {Mri} long]

    dict set evt_info STIMULATOR [list 47 {Stimulator Signal} long]

    set subtypes [dict create PRESS 0 RELEASE 1 MOVE 2]
    dict set evt_info TOUCH [list 48 {Touchscreen Press} float $subtypes]

    dict set evt_info TARGNAME [list 128 {Target Name} string]
    dict set evt_info SCENENAME [list 129 {Scene Name} string]
    dict set evt_info SACCADE_INFO [list 130 {Saccade Data} float]
    dict set evt_info STIM_TRIGGER [list 131 {Stimulus Trigger} float]
    dict set evt_info MOVIENAME [list 132 {Movie Name} string]
    dict set evt_info STIMULATION [list 133 {Electrical Stimulation} long]
    dict set evt_info SECOND_CHANCE [list 134 {Second Chance} long]
    dict set evt_info SECOND_RESP [list 135 {Second Response} long]

    set subtypes [dict create PREPCALL 0 PREPRETURN 1 \
        SWAPTRIG 2 MAXTRIG 3 SWAPTRETURN 4]
    dict set evt_info SWAPBUFFER [list 136 {Swap Buffer} float $subtypes]

    dict set evt_info STIM_DATA [list 137 {Stim Data} string]
    dict set evt_info DIGITAL_LINES [list 138 {Digital Input Status} long]

    # initialize evt_type_names
    for {set i 0} {$i < 16} {incr i} {
        dict set evt_type_names $i Reserved[format %03d $i]
    }
    for {set i 16} {$i < 128} {incr i} {
        dict set evt_type_names $i System[format %03d $i]
    }
    for {set i 128} {$i < 255} {incr i} {
        dict set evt_type_names $i User[format %03d $i]
    }
    dict for {k v} $evt_info {
        set id [lindex $v 0]
        set ptype [lindex $v 2]
        dict set evt_type_names $id $k
        dict set evt_type_ids $k $id
        dict set evt_ptype_ids $k [dict get $evt_datatypes $ptype]
    }

    dict for {k v} $evt_info {
        set subtypes [lindex $v 3]
        set id [lindex $v 0]
        if {$subtypes != ""} {
            dict for { sk sv } $subtypes {
                dict set evt_subtype_ids $k $sk $sv
                dict set evt_subtype_names $id $sv $sk
            }
        }
    }
}


## Compatibility with USER functions

#
# Compatibility procs
#

set ESS_QUERY(STATE) 0
set ESS_QUERY(NAME) 1
set ESS_QUERY(DATAFILE) 3
set ESS_QUERY(DETAILS) 10
set ESS_QUERY(REMOTE) 11
set ESS_QUERY(EXECNAME) 12
set ESS_QUERY(NAME_INDEX) 13

# USER_funcs

proc USER_START {} {::ess::start}
proc USER_STOP {} {::ess::stop}
proc USER_RESET {} {::ess::reset}

proc USER_SET_SYSTEM {s} {::ess::set_system $s}
proc USER_SET_TRACE {l} {}

# qpcs::sendToQNX $server USER_SET_EYES $arg1 $arg2 $arg2 $arg4
# qpcs::sendToQNX $server USER_SET_LEVERS $arg1 $arg2
proc USER_SET_PARAMS {name valstr} {::ess::set_param $name $valstr}

proc USER_QUERY_STATE {} {return [::ess::query_state]}
proc USER_QUERY_DETAILS {} {return "24 25 5"}
proc USER_QUERY_NAME {} {return [::ess::query_system_name]}
proc USER_QUERY_NAME_INDEX {ndx} {return [::ess::query_system_name_by_index $ndx]}

proc USER_QUERY_DATAFILE {} {return ""}
proc USER_QUERY_REMOTE {} {return [::ess::query_remote]}

# qpcs::sendToQNX $server USER_QUERY_EXECNAME
proc USER_QUERY_PARAM {index type} {::ess::query_param $index $type}
proc USER_QUERY_PARAMS {} {::ess::get_params}

# qpcs::sendToQNX $server USER_CHECK_DISKSPACE

proc USER_FILEIO {type op flags name} {
    essFileIO $type $op $flags $name
}

proc JUICE {duration} {essJuice 0 $duration}

proc EM_INFO {} {
    return "0 2 128"
}
