# -*- mode: tcl -*-

package require dlsh
package provide ess 2.0

catch { System destroy }

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
    
    constructor { name } {
        set _systemname $name
	set _protocolname {}
	set _variantname {}
        set _status stopped
	set _start_state {}
	set _end_state {}
	set _states [dict create]
	set _evt_info $ess::evt_info
	set _evt_type_ids $ess::evt_type_ids
	set _evt_type_names $ess::evt_type_names
	set _evt_subtype_ids $ess::evt_subtype_ids
	set _evt_subtype_names $ess::evt_subtype_names
	set _evt_ptype_ids $ess::evt_ptype_ids
	set _vars {}
	set _variants {}
    }

    destructor { my deinit }
    
    method name { } { return $_systemname }
    method get_system { } { return $_systemname }

    method set_protocol { p } { set _protocolname $p }
    method get_protocol { } { return $_protocolname }

    method set_variants { vdict } { set _variants $vdict }
    method get_variants { } { return $_variants }
    
    method set_variant { v } { set _variantname $v }
    method get_variant { } { return $_variantname }

    method add_variant { name method params } {
	dict set _variants $name [list $method $params]
    }
    
    method configure_stim { host } {

	foreach var "screen_halfx screen_halfy screen_w screen_h" {
	    oo::objdefine [self] variable $var
	}
	
	rmtOpen $host
	variable screen_halfx [rmtSend "screen_set HalfScreenDegreeX"]
	variable screen_halfy [rmtSend "screen_set HalfScreenDegreeY"]
	set scale_x [rmtSend "screen_set ScaleX"]
	set scale_y [rmtSend "screen_set ScaleY"]
	variable screen_w [expr [rmtSend "screen_set WinWidth"]/$scale_x]
	variable screen_h [expr [rmtSend "screen_set WinHeight"]/$scale_y]
	if { $screen_halfx == "" } {
	    variable screen_halfx 16.0
	    variable screen_halfy 9.0
	    variable screen_w 1024
	    variable screen_h 600
	}
	
	rmtSend "set dservhost [dservGet qpcs/ipaddr]"
	
	# source this protocol's stim functions
	set stimfile [file join [set ess::system_path] \
			  $_systemname $_protocolname ${_protocolname}_stim.tcl]
	if { ![catch {set f [open $stimfile]}] } {
	    set script [read $f]
	    close $f
	    rmtSend $script
	}
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
	dservSet ess/state [dict get $_current_state name]_a
    	set state_time [now]
	my [dict get $_current_state name]_a
    }
    
    method do_transition {} {
	dservSet ess/state [dict get $_current_state name]_t
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
	foreach var [dict keys $_default_param_vals] \
	    val [dict values $_default_param_vals] {
		my set_param $var $val
	}
    }
    
    method add_param { pname val type ptype } {
	set t [dict get $ess::param_types [string toupper $type]]
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
    }
    
    method add_variable { var { val {} } } {
	set pos [lsearch $_vars $var]
	if { $pos < 0 } {
	    my variable $var
	    lappend _vars $var
	    oo::objdefine [self] variable $var
	}
	if { $val != {} } {
	    set $var $val
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

    method start {} {
        if { $_status == "running" } return
	set _status running
	ess::evt_put SYSTEM_STATE RUNNING [now]	
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
	set _status stopped
	ess::evt_put SYSTEM_STATE STOPPED [now]	
	if { [info exists _callbacks(quit)] } {
	    my $_callbacks(quit)
	}
	return
    }
    
    method end {} {
	set _status stopped
	ess::evt_put SYSTEM_STATE STOPPED [now]	
	if { [info exists _callbacks(end)] } {
	    my $_callbacks(end)
	}
	return
    }

    method file_open { f } {
        if { $_status == "running" } return
	if { [info exists _callbacks(file_open)] } {
	    my $_callbacks(file_open) $f
	}
	return
    }

    method file_close { f } {
        if { $_status == "running" } return
	if { [info exists _callbacks(file_close)] } {
	    my $_callbacks(file_close) $f
	}
	return
    }

    method set_subject {} {
        if { $_status == "running" } return
	if { [info exists _callbacks(subject)] } {
	    my $_callbacks(subject)
	}
	return
    }
    
    method status {} { return $_status }

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
    set current(open_system)   0
    set current(open_protocol) 0
    set current(open_variant)  0

    set current(system) {}
    set current(protocol) {}
    set current(variant) {}
    
    variable param_types { ANY 0 TIME 1 VARIABLE 2 SUBJECT 3 STORAGE 4 STIM 5 }
    variable param_datatypes { null 0 char 1 short 2 long 3 \
				   int 3 bool 3 float 4 \
				   double 5 long_array 6 \
				   float_array 7 string 8 }
    variable evt_datatypes { unknown 0 null 1 string 2 short 3 \
				 long 4 float 5 double 6 }
    variable open_datafile {}
    variable subject_id {}

    variable obs_pin 26
    
    proc create_system { name } {
	# only allow one instance of a system with this name
	foreach s [info class instances System] {
	    if { [$s name] == $name } {
		$s destroy
	    }
	}
	set s [System new $name]
	return $s
    }

    proc clear_systems { name } {
	foreach s [info class instances System] {
	    if { [$s name] == $name } {
		$s destroy
	    }
	}
    }
    
    proc find_system { name } {
	foreach s [info class instances System] {
	    if { [$s name] == $name } {
		return $s
	    }
	}
    }

    proc unload_system {} {
	variable current
	set sname $current(system)
	if { $current(open_system) } {
	    set s [find_system $sname]
	    $s deinit
	    set current(open_system) 0
	    set current(system) {}
	}
    }

    proc load_system { { system {} } { protocol {} } { variant {} } } {
	variable current

	catch unload_system
	
	set systems [find_systems]
	if { $system == "" } {
	    set current(system) [lindex $systems 0]
	} else {
	    if { [lsearch $systems $system] < 0 } {
		error "system $system not found"
	    }
	    set current(system) $system
	}
	
	[set current(system)]::create
	
	system_init $current(system)

	set protocols [find_protocols $current(system)]
	if { $protocol == "" } {
	    set current(protocol) [lindex $protocols 0]
	} else {
	    if { [lsearch $protocols $protocol] < 0 } {
		error "protocol $protocol not found in $current(system)"
	    }
	    set current(protocol) $protocol
	}

	set variants [find_variants $current(system) $current(protocol)]
	if { $variant == "" } {
	    set current(variant) [lindex $variants 0]
	} else {
	    if { [lsearch $variants $variant] < 0 } {
		set s_p "$current(system)/$current(protocol)"
		error "variant $variant not found in $s_p"
	    }
	    set current(variant) $variant
	}

	protocol_init $current(system) $current(protocol)

	# set the variant for the current system
	set s [find_system $current(system)]
	$s set_variant $current(variant)

	# initialize the variant by calling the appropriate loader 
	variant_init $current(system) $current(protocol) $current(variant)
    }

    proc set_system { system } {
	variable current
	catch unload_system
	set s [find_system $system]
	if { $s == "" } {
		error "system $system not found"
	}
	$s init
	set current(system) $system
	set current(state_system) $s
    }
    
    proc init {} {
    	dservRemoveAllMatches
        dpointRemoveAllScripts
	for { set i 0 } { $i < $::nTimers } { incr i } {
	    timerSetScript $i "[namespace current]::do_update"
	}
    }

    proc do_update { } {
	variable current
	if { $current(state_system) != ""} {
	    $current(state_system) update
	}
    }

    proc start {} {
	variable current
	if { $current(system) == "" } { print "no system set"; return }
	set current(state_system) [find_system $current(system)]
	if { $current(state_system) != ""} {
	    ess::evt_put USER START [now]
	    $current(state_system) start
	}
	return
    }
    
    proc stop {} {
	variable current
	if { $current(system) == "" } { print "no system set"; return }
	set current(state_system) [find_system $current(system)]
	if { $current(state_system) != ""} {
	    $current(state_system) stop
	    ess::evt_put USER QUIT [now]
	}
	return
    }
    
    proc reset {} {
	variable current
	if { $current(system) == "" } { print "no system set"; return }
	set current(state_system) [find_system $current(system)]
	if { $current(state_system) != ""} {
	    $current(state_system) reset
	    ess::evt_put USER RESET [now]
	}
	return
    }

    proc set_obs_pin { pin } {
	variable obs_pin
	set obs_pin $pin
    }

    proc begin_obs { current total } {
	variable obs_pin
	rpioPinOn $obs_pin
	ess::evt_put BEGINOBS INFO [now] $current $total
    }
    
    proc end_obs { { status 1 } } {
	variable obs_pin
	rpioPinOff $obs_pin
	ess::evt_put ENDOBS $status [now]
    }
    
    proc query_state {} {
        variable current
        return [$current(state_system) status]
    }

    proc query_system {} {
        return $current(system)
    } 

    proc query_system_by_index { index } {
	set sysnames [find_systems]
	if { $index >= [llength $sysnames] } { error {} }
        return [dict get $systems [lindex $sysnames $index]]
    }

    proc query_system_name {} {
        variable current
        return $current(system)
    } 

    proc query_remote {} {
	return [rmtName]
    }
    
    proc query_system_name_by_index { index } {
        variable systems
	set sysnames [dict keys $systems]
	if { $index >= [llength $sysnames] } { error {} }
	return [lindex $sysnames $index]
    }

    proc query_param { index type } { 
	variable current
	variable param_datatypes
	set params [$current(state_system) get_params]
	set inds [lsearch -index 1 -all [dict values $params] $type]	
	if { $index >= [llength $inds] } { error {} }
	set actual_index [lindex $inds $index]
	set name [lindex [dict keys $params] $actual_index]
	set vals [lindex [dict values $params] $actual_index]
	set dtypeinfo [lindex $vals 2]
	set colon [string first : $dtypeinfo]
	if { $colon >= 0 } {
	    set dtypename [string range $dtypeinfo 0 [expr {$colon-1}]]
	} else {
	    set dtypename $dtypeinfo
	}
	set dtype [dict get $param_datatypes $dtypename]
	return "{$name} {[lindex $vals 0]} [expr {$index+1}] $type $dtype"
    }
    
    proc get_params {} {
	variable current
	$current(state_system) get_params
    }

    proc get_param { p } {
	variable current
	$current(state_system) get_param $p
    }

    proc set_param { param val } {
	variable current
	$current(state_system) set_param $param $val
	ess::evt_put PARAM NAME [now] $param
	ess::evt_put PARAM VAL  [now] $val
    }

    proc set_params { args } {
	set nargs [llength $args]
	if { [expr {$nargs%2}] } {
	    error "invalid param/setting args"
	}
	for { set i 0 } { $i < $nargs } { incr i 2 } {
	    set_param [lindex $args $i] [lindex $args [expr {$i+1}]]
	}
    }
    
    proc evt_put { type subtype time args } {
	variable current
	if { [string is int $type] } {
	    set type_id $type
	} else {
	    set type_id [dict get [set $current(state_system)::_evt_type_ids] $type]
	}
	if { [string is int $subtype] } {
	    set subtype_id $subtype
	} else {
	    set subtype_id [dict get [set $current(state_system)::_evt_subtype_ids] \
				$type $subtype]
	}
	set ptype [dict get [set $current(state_system)::_evt_ptype_ids] $type]
	eval evtPut $type_id $subtype_id $time $ptype $args
    }

    proc store_evt_names {} {
	variable current
	
	# output the current event names
	dict for { k v } [set $current(state_system)::_evt_type_names] {
	    set ptype_ids [set $current(state_system)::_evt_ptype_ids]
	    if { [dict exists $ptype_ids $v] } {
		set p [dict get $ptype_ids $v]
	    } else {
		set p 0
	    }
	    ess::evt_name_set $k $v $p
	}
	dict for { k v } [set $current(state_system)::_evt_info] {
	    set subinfo [lindex $v 3]
	    if { $subinfo != "" } {
		set id [lindex $v 0]
		ess::evt_put SUBTYPES $id [now] $subinfo
	    }
	}
    }
        
    proc evt_name_set { type name ptype } {
	variable evt_datatypes
	if { [string is int $ptype] } {
	    set ptype_id $ptype
	} else {
	    set ptype_id [dict get $evt_datatypes $ptype]
	}
	eval evtNameSet $type $name $ptype_id
    }
	       
    proc file_open { f { overwrite 0 } } {
	variable current
        variable open_datafile
	variable subject_id
	variable data_dir

	if { $open_datafile != "" } {
	    print "datafile $open_datafile not closed"
	    return
	}
	
	set filename [file join $data_dir $f.ess]
	if { !$overwrite } {
	    if [file exists $filename] {
		print "file $f already exists in directory $data_dir"
		return
	    }
	}
        dservLoggerOpen $filename 1
	set open_datafile $filename

	dservLoggerAddMatch $filename eventlog/events
	dservLoggerAddMatch $filename eventlog/names
	dservLoggerAddMatch $filename stimdg
#	dservLoggerAddMatch $filename em_coeffDG

	dservLoggerResume $filename	

	# reset system and output event names
	ess::reset

	# output info for interpreting events
	ess::store_evt_names

	dservTouch stimdg
	
	ess::evt_put ID ESS      [now] $current(system)
	ess::evt_put ID PROTOCOL [now] $current(protocol)
	ess::evt_put ID VARIANT  [now] $current(variant)
	ess::evt_put ID SUBJECT  [now] $subject_id
	
	dict for { pname pval } [ess::get_params] {
	    ess::evt_put PARAM NAME [now] $pname
	    ess::evt_put PARAM VAL  [now] [lindex $pval 0]
	}

	# call the system's specific file_open callback
	$current(state_system) file_open $f
	
#	dservTouch em_coeffDG
	return
    }

    proc file_close {} {
	variable current
        variable open_datafile
	if { $open_datafile != "" } {
	    
	    # could put pre_close callback here
	    
    	    dservLoggerClose $open_datafile
	    
	    # call the system's specific file_close callback
	    catch {$current(state_system) file_close $open_datafile} ioerror
	}
	set open_datafile {}
    }
}

namespace eval ess {
    variable em_windows
    set em_windows(processor) "windows"
    set em_windows(dpoint) "proc/windows"

    proc em_update_setting { win } {
	ainSetIndexedParam $win settings 0
    }

    proc em_window_process { dpoint } {
	variable em_windows
	lassign [dservGet $dpoint] \
	    em_windows(changes) em_windows(states) \
	    em_windows(hpos) em_windows(vpos)
	set em_windows(timestamp) [dservTimestamp $dpoint]
	do_update
    }
    
    proc em_init {} {
	variable em_windows
	ainSetProcessor $em_windows(processor)
	ainSetParam dpoint $em_windows(dpoint)
	dservAddExactMatch $em_windows(dpoint)/status
	dpointSetScript $em_windows(dpoint)/status \
	    [list ess::em_window_process \
		 $em_windows(dpoint)/status]
	for { set win 0 } { $win < 8 } { incr win } {
	    ainSetIndexedParam $win active 0
	}
	set em_windows(states) 0

	# these should be loaded per subject
	variable em_scale_h 200
	variable em_scale_v 200
    }

    proc em_check_state { win } {
	variable em_windows
	set state [ainGetIndexedParam $win state]
	if { $state } {
	    set em_windows(states) [expr $em_windows(states) | (1<<$win)]
	} else {
	    set em_windows(states) [expr $em_windows(states) & ~(1<<$win)]
	}
	return $state
    }
    
    proc em_region_on { win } {
	ainSetIndexedParam $win active 1
	em_check_state $win
	em_update_setting $win
    }

    proc em_region_off { win } {
	ainSetIndexedParam $win active 0
	em_check_state $win
	em_update_setting $win
    }

    proc em_region_set { win type center_x center_y
			 plusminus_x plusminus_y } {
	ainSetIndexedParams $win type $type \
	    center_x $center_x center_y $center_y \
	    plusminus_x $plusminus_x plusminus_y $plusminus_y
	em_check_state $win
	em_update_setting $win
    }

    proc em_fixwin_set { win cx cy r { type 1 } } {
	set x [expr {int($cx*$ess::em_scale_h)+2048}]
	set y [expr {-1*int($cy*$ess::em_scale_v)+2048}]
	set pm_x [expr {$r*$ess::em_scale_h}]
	set pm_y [expr {$r*$ess::em_scale_v}]
	em_region_set $win 1 $x $y $pm_x $pm_y
    }

    proc em_eye_in_region { win } {
	variable em_windows
	return [expr {($em_windows(states) & (1<<$win)) != 0}]
    }
}

namespace eval ess {
    variable touch_windows
    set touch_windows(processor) "touch_windows"
    set touch_windows(dpoint) "proc/touch_windows"

    proc touch_update_setting { win } {
	touchSetIndexedParam $win settings 0
    }

    proc touch_window_process { dpoint } {
	variable touch_windows
	lassign [dservGet $dpoint] \
	    touch_windows(changes) touch_windows(states) \
	    touch_windows(hpos) touch_windows(vpos)
	set touch_windows(timestamp) [dservTimestamp $dpoint]
	do_update
    }

    proc touch_reset {} {
	dservTouch mtouch/touchvals
    }
    
    proc touch_init {} {
	variable touch_windows
	variable current
	touchSetProcessor $touch_windows(processor)
	touchSetParam dpoint $touch_windows(dpoint)
	dservAddExactMatch $touch_windows(dpoint)/status
	dpointSetScript $touch_windows(dpoint)/status \
	    [list ess::touch_window_process \
		 $touch_windows(dpoint)/status]
	for { set win 0 } { $win < 8 } { incr win } {
	    touchSetIndexedParam $win active 0
	}
	set touch_windows(states) 0

	set s $current(state_system)

	set w [set ${s}::screen_w]
	set h [set ${s}::screen_h]
	set w_deg [expr 2*[set ${s}::screen_halfx]]
	set h_deg [expr 2*[set ${s}::screen_halfy]]
	variable touch_win_w $w
	variable touch_win_h $h
	variable touch_win_half_w [expr $w/2]
	variable touch_win_half_h [expr $h/2]
	variable touch_win_deg_w $w_deg
	variable touch_win_deg_h $h_deg
	variable touch_win_scale_w [expr {$touch_win_w/$touch_win_deg_w}]
	variable touch_win_scale_h [expr {$touch_win_h/$touch_win_deg_h}]
    }

    proc touch_check_state { win } {
	variable touch_windows
	set state [touchGetIndexedParam $win state]
	if { $state } {
	    set touch_windows(states) [expr $touch_windows(states) | (1<<$win)]
	} else {
	    set touch_windows(states) [expr $touch_windows(states) & ~(1<<$win)]
	}
	return $state
    }

    proc touch_region_on { win } {
	touchSetIndexedParam $win active 1
	touch_check_state $win
	touch_update_setting $win
    }

    proc touch_region_off { win } {
	touchSetIndexedParam $win active 0
	touch_check_state $win
	touch_update_setting $win
    }

    proc touch_region_set { win type center_x center_y
			 plusminus_x plusminus_y } {
	touchSetIndexedParams $win type $type \
	    center_x $center_x center_y $center_y \
	    plusminus_x $plusminus_x plusminus_y $plusminus_y
	touch_check_state $win
	touch_update_setting $win
    }

    proc touch_win_set { win cx cy r { type 1 } } {
	set x [expr {int($cx*$ess::touch_win_scale_w)+
		     $ess::touch_win_half_w}]
	set y [expr {-1*int($cy*$ess::touch_win_scale_h)+
		     $ess::touch_win_half_h}]
	set pm_x [expr {$r*$ess::touch_win_scale_w}]
	set pm_y [expr {$r*$ess::touch_win_scale_h}]
	touch_region_set $win $type $x $y $pm_x $pm_y
    }

    proc touch_in_win { win } {
	variable touch_windows
	set in [expr {($touch_windows(states) & (1<<$win)) != 0}]
	return $in
    }

    proc touch_in_region { win } {
	return [touch_in_win $win]
    }
}

namespace eval json {
proc is_dict {d} {
    try {
        dict size $d
    } on error {} {
        return false
    }
}

proc range {a {b ""}} {
    if {$b eq ""} {
        set b $a
        set a 0
    }
    for {set r {}} {$a<$b} {incr a} {
        lappend r $a
    }
    return $r
}

proc prefix_matches {prefix keys} {
    lmap key $keys {
        if {[string match $prefix* $key]} {
            set key
        } else {
            continue
        }
    }
}

proc unique_prefix {prefix keys} {
    set m [prefix_matches $prefix $keys]
    if {[llength $m] == 1} {
        lindex $m 0
    } else {
        return ""
    }
}

proc dict_glob {dict key} {
    set idx {}
    foreach elem $key {
        if {[dict exists $dict {*}$idx $elem]} {
            lappend idx $elem
        } elseif {[dict exists $dict {*}$idx *]} {
            lappend idx *
        } else {
            return ""
        }
    }
    return $idx
}

proc stringify_string {s} {
    # FIXME: more quoting?
    return "\"[string map {{"} {\"}} $s]\""
    # "\" vim gets confused!
}


proc stringify {dict {schema {}} {key {}}} {
    if {$key ne ""} {
        if {![dict exists $dict {*}$key]} {
            error "this can't happen"
        }
        set value [dict get $dict {*}$key]
        if {[dict exists $schema {*}$key]} {
            set type [dict get $schema {*}$key]
        } else {
            set realkey [dict_glob $schema $key]
            if {$realkey ne ""} {
                set type [dict get $schema {*}$realkey]
            } else {
                set type ""
            }
        }
    } else {
        set value $dict
        set type $schema
    }
    if {$type ne ""} {
        set type [unique_prefix $type {number string boolean null object array}]
    }
    if {$type eq ""} {
        if {![is_dict $value]} {
            if {[string is double -strict $value]} {
                set type "number"
            } else {
                set type "string"
            }
        } else {
            if {[dict keys $value] eq [range [dict size $value]]} {
                set type "array"
            } else {
                set type "dict"
            }
        }
    }
    
    switch -exact $type {
        "number" {
            return "$value"
        }
        "null" {
            return "null"
        }
        "boolean" {
            return [expr {$value ? "true" : "false"}]
        }
        "string" {
            # FIXME: quote quotes.  And maybe other things?
            return [stringify_string $value]
        }
        "dict" {
            set i 0
            set body [lmap {k v} $value {
                set res [stringify_string $k]
                append res ":"
                append res [stringify $dict $schema [list {*}$key $k]]
            }]
            return "{[join $body ,]}"
        }
        "array" {
            set body {}
            for {set i 0} {$i < [dict size $value]} {incr i} {
                lappend body [dict get $value $i]
            }
            return "\[[join $body ,]\]"
        }
    }
}
}

namespace eval ess {
    variable current
    set current(state_system) {}

    #
    # system variables
    #
    #  ESS_SYSTEM_PATH: where to find systems
    #  ESS_RMT_HOST:    ip address of stim
    #  ESS_DATA_DIR:    folder to store raw data files
    #
    if { [info exists ::env(ESS_SYSTEM_PATH)] } {
        variable system_path $::env(ESS_SYSTEM_PATH)
    } else {
        variable system_path /usr/local/share/systems
    }

    if { [info exists ::env(ESS_RMT_HOST)] } {
        variable rmt_host $::env(ESS_RMT_HOST)
    } else {
        variable rmt_host localhost
    }

    if { [info exists ::env(ESS_DATA_DIR)] } {
        variable data_dir $::env(ESS_DATA_DIR)
    } else {
	variable data_dir /shared/qpcs/data/incage
    }    


    proc system_init { system } {
	variable current

	set s [find_system $system]
	set current(state_system) $s

	# initialize the system
	$s init
	
	#::io::init
	#::screen::init

	# publish this system's event name table
	ess::store_evt_names
	ess::evt_put ID ESS [now] $system
	set current(open_system) 1
    }

    proc protocol_init { system protocol } {
	variable current
	set s [ess::find_system $system]

	# initialize the protocol
	ess::${system}::${protocol}::protocol_init $current(state_system)

	# initialize this protocol's variants
	${s} set_variants [set ${system}::${protocol}::variants]	
	ess::${system}::${protocol}::variants_init $current(state_system)

	${s} protocol_init
	
	ess::evt_put ID PROTOCOL [now] $current(system):$protocol
	set current(protocol) $protocol
	set current(open_protocol) 1
    }

    proc variant_loader_command { system protocol variant } {
	set s [ess::find_system $system]
	
	set vinfo [dict get [$s get_variants] $variant]
	set loader_proc [lindex $vinfo 0]
	set variant_dict_name [lindex $vinfo 1]
	set loader_default_args [set ${system}::${protocol}::${loader_proc}_defaults]
	set loader_variant_args [set ${system}::${protocol}::${loader_proc}_${variant_dict_name}]
	set loader_args_dict [dict merge $loader_default_args $loader_variant_args]

	# now build list of args for this particular loader_proc
	set loader_arg_names [lindex [info object definition $s $loader_proc] 0]
	set loader_args {}
	foreach a $loader_arg_names {
	    lappend loader_args [dict get $loader_args_dict $a]
	}
	return "$loader_proc $loader_args"
    }
    
    proc variant_init { system protocol variant } {
	variable current
	set s [ess::find_system $system]

	# get loader info for this variant and call
	$s {*}[variant_loader_command $system $protocol $variant]

	# push new stimdg to dataserver
	$s update_stimdg
	
	ess::evt_put ID VARIANT [now] $system:$protocol:$variant
	set current(variant) $variant
	set current(open_variant) 1
    }
    
    proc find_systems {} {
	set systems {}
	foreach f [glob $ess::system_path/*] {
	    if  { [file isdirectory $f] &&
		  [file exists $f/[file tail $f].tcl] } {
		catch { namespace delete ::ess::[file tail $f] }
		source $f/[file tail $f].tcl
		lappend systems [file tail $f]
		
	    }
	}
	return $systems
    }

    proc find_protocols { s } {
	set protocols {}
	foreach f [glob [file join $ess::system_path $s]/*] {
	    if { [file isdirectory $f] &&
		 [file exists $f/[file tail $f].tcl] } {
		catch { namespace delete ::ess::${s}::[file tail $f] }
		source $f/[file tail $f].tcl
		lappend protocols [file tail $f]
	    }
	}
	return $protocols	
    }

    proc find_variants { s p } {
	set f [file join $ess::system_path $s ${p} ${p}_variants.tcl]
	source $f
	return [dict keys [set ::ess::${s}::${p}::variants]]
    }

    # These getters could be changed to not re-source perhaps
    proc get_systems { } {
	set systems {}
	foreach f [glob $ess::system_path/*] {
	    if  { [file isdirectory $f] &&
		  [file exists $f/[file tail $f].tcl] } {
		lappend systems [file tail $f]
	    }
	}
	return $systems
    }

    proc get_protocols { system } {
	set protocols {}
	foreach f [glob [file join $ess::system_path $system]/*] {
	    if { [file isdirectory $f] &&
		 [file exists $f/[file tail $f].tcl] } {
		lappend protocols [file tail $f]
	    }
	}
	return $protocols	
    }
    
    proc get_variants { sysname protocol } {
	return [find_variants $sysname $protocol]
    }

    proc get_system_dict {} {
	variable current
	set d [dict create]
	foreach s [get_systems] {
	    set sdict [dict create]
	    if { $s == $current(system) } {
		foreach p [get_protocols $s] {
		    dict set sdict $p [get_variants $s $p]
		}
	    }
	    dict set d $s $sdict
	}
	return $d
    }
    
    proc get_system_json {} {
	return [json::stringify [get_system_dict]]
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
    # 16-127	System events
    # 128-255	User events

    dict set evt_info MAGIC     [list 0  {Magic Number}         null]
    dict set evt_info NAME      [list 1  {Event Name}           string]
    dict set evt_info FILE      [list 2  {File I/O}             string]

    set subtypes [dict create START 0 QUIT 1 RESET 2]
    dict set evt_info USER      [list 3  {User Interaction}     null $subtypes]

    set subtypes [dict create ACT 0 TRANS 1]
    dict set evt_info TRACE     [list 4  {State System Trace}   string]

    set subtypes [dict create NAME 0 VAL 1]
    dict set evt_info PARAM     [list 5  {Parameter Set}       string $subtypes]

    dict set evt_info SUBTYPES  [list 6  {Subtype Names}       string]

    set subtypes [dict create STOPPED 0 RUNNING 1 INACTIVE 2]
    dict set evt_info SYSTEM_STATE  [list 7  {System State}    string $subtypes]

    dict set evt_info FSPIKE    [list 16 {Time Stamped Spike}   long]
    dict set evt_info HSPIKE    [list 17 {DIS-1 Hardware Spike} long]

    set subtypes [dict create ESS 0 SUBJECT 1 PROTOCOL 2 VARIANT 3]
    dict set evt_info ID        [list 18 {Name}                string $subtypes]

    set subtypes [dict create INFO 0]
    dict set evt_info BEGINOBS  [list 19 {Start Obs Period}     long $subtypes]

    set subtypes [dict create INCOMPLETE 0 COMPLETE 1 QUIT 2 ABORT 3]
    dict set evt_info ENDOBS    [list 20 {End Obs Period}       long $subtypes]

    dict set evt_info ISI       [list 21 {ISI}                  long]
    dict set evt_info TRIALTYPE [list 22 {Trial Type}           long]
    dict set evt_info OBSTYPE   [list 23 {Obs Period Type}      long]

    dict set evt_info EMLOG     [list 24 {EM Log}               long]

    set subtypes [dict create OFF 0 ON 1 SET 2]
    dict set evt_info FIXSPOT   [list 25 {Fixspot}              float $subtypes]

    set subtypes [dict create SCALE 0 CIRC 1 RECT 2]
    dict set evt_info EMPARAMS  [list 26 {EM Params}            float $subtypes]
    
    set subtypes [dict create OFF 0 ON 1 SET 2]
    dict set evt_info STIMULUS  [list 27 {Stimulus}             long $subtypes]

    set subtypes [dict create OFF 0 ON 1 SWAP 2]
    dict set evt_info PATTERN   [list 28 {Pattern}              long $subtypes]

    set subtypes [dict create STIMID 1]
    dict set evt_info STIMTYPE  [list 29 {Stimulus Type}        long $subtypes]

    set subtypes [dict create OFF 0 ON 1]
    dict set evt_info SAMPLE    [list 30 {Sample}               long $subtypes]
    dict set evt_info PROBE     [list 31 {Probe}                long $subtypes]
    dict set evt_info CUE       [list 32 {Cue}                  long $subtypes]
    dict set evt_info TARGET    [list 33 {Target}               long $subtypes]
    dict set evt_info DISTRACTOR [list 34 {Distractor}          long $subtypes]
    dict set evt_info SOUND     [list 35 {Sound Event}          long $subtypes]
    dict set evt_info CHOICES   [list 49 {Choices}              long $subtypes]
    
    set subtypes [dict create OUT 0 IN 1 REFIXATE 2]
    dict set evt_info FIXATE    [list 36 {Fixation}             long $subtypes]

    set subtypes [dict create NONE 0 LEFT 1 RIGHT 2 BOTH 3]
    dict set evt_info RESP      [list 37 {Response}             long $subtypes]
    dict set evt_info SACCADE   [list 38 {Saccade}              long]
    dict set evt_info DECIDE    [list 39 {Decide}               long]

    set subtypes [dict create INCORRECT 0 CORRECT 1 ABORT 2]
    dict set evt_info ENDTRIAL  [list 40 {EOT}                  long $subtypes]

    set subtypes [dict create EYE 0 LEVER 1 NORESPONSE 2 STIM 3]
    dict set evt_info ABORT     [list 41 {Abort}                long $subtypes]

    set subtypes [dict create DURATION 0 TYPE 1]
    dict set evt_info REWARD    [list 42 {Reward}               long $subtypes]
    
    dict set evt_info DELAY     [list 43 {Delay}                long]
    dict set evt_info PUNISH    [list 44 {Punish}               long]
    
    dict set evt_info PHYS      [list 45 {Physio Params}        float]
    dict set evt_info MRI       [list 46 {Mri}                  long] 
    
    dict set evt_info STIMULATOR [list 47 {Stimulator Signal}   long]

    set subtypes [dict create PRESS 0 RELEASE 1 MOVE 2]
    dict set evt_info TOUCH [list 48 {Touchscreen Press}   float $subtypes]
    
    dict set evt_info TARGNAME     [list 128 {Target Name} string]
    dict set evt_info SCENENAME    [list 129 {Scene Name} string]
    dict set evt_info SACCADE_INFO [list 130 {Saccade Data} float]
    dict set evt_info STIM_TRIGGER [list 131 {Stimulus Trigger} float]
    dict set evt_info MOVIENAME    [list 132 {Movie Name} string]
    dict set evt_info STIMULATION  [list 133 {Electrical Stimulation} long]
    dict set evt_info SECOND_CHANCE [list 134 {Second Chance} long]
    dict set evt_info SECOND_RESP  [list 135 {Second Response} long]

    set subtypes [dict create PREPCALL 0 PREPRETURN 1 \
		      SWAPTRIG 2 MAXTRIG 3 SWAPTRETURN 4]
    dict set evt_info SWAPBUFFER   [list 136 {Swap Buffer} float $subtypes]

    dict set evt_info STIM_DATA    [list 137 {Stim Data} string]
    dict set evt_info DIGITAL_LINES [list 138 {Digital Input Status} long]

    # initialize evt_type_names
    for { set i 0 } { $i < 16 } { incr i } {
	dict set evt_type_names $i Reserved[format %03d $i]
    }
    for { set i 16 } { $i < 128 } { incr i } {
	dict set evt_type_names $i System[format %03d $i]
    }
    for { set i 128 } { $i < 255 } { incr i } {
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
	if { $subtypes != "" } {
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

set ESS_QUERY(STATE)       0
set ESS_QUERY(NAME)        1
set ESS_QUERY(DATAFILE)    3
set ESS_QUERY(DETAILS)     10
set ESS_QUERY(REMOTE)      11
set ESS_QUERY(EXECNAME)    12
set ESS_QUERY(NAME_INDEX)  13

# USER_funcs

proc USER_START {} { ess::start }
proc USER_STOP  {} { ess::stop }
proc USER_RESET {} { ess::reset }

proc USER_SET_SYSTEM { s } { ess::set_system $s }
proc USER_SET_TRACE { l } { }

# qpcs::sendToQNX $server USER_SET_EYES $arg1 $arg2 $arg2 $arg4
# qpcs::sendToQNX $server USER_SET_LEVERS $arg1 $arg2
proc USER_SET_PARAMS { name valstr } { ess::set_param $name $valstr }

proc USER_QUERY_STATE {} { return [ess::query_state] }
proc USER_QUERY_DETAILS {} { return "24 25 5" }
proc USER_QUERY_NAME {} { return [ess::query_system_name] }
proc USER_QUERY_NAME_INDEX { ndx } { return [ess::query_system_name_by_index $ndx] }

proc USER_QUERY_DATAFILE  {} { return "" }
proc USER_QUERY_REMOTE {} { return [ess::query_remote] }

# qpcs::sendToQNX $server USER_QUERY_EXECNAME 
proc USER_QUERY_PARAM { index type } { ess::query_param $index $type }
proc USER_QUERY_PARAMS {} { ess::get_params } 

# qpcs::sendToQNX $server USER_CHECK_DISKSPACE

proc USER_FILEIO { type op flags name } {
    essFileIO $type $op $flags $name
}

proc JUICE { duration } { essJuice 0 $duration }

proc EM_INFO {} {
    return "0 2 128"
}



