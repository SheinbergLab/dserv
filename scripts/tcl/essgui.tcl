package require Tk
package require qpcs
package require mdns

set status(state) Stopped
set status(eye_hor) 0
set status(eye_ver) 0
set status(obs_id) 0
set status(obs_total) 0
set status(obs_info) 0/0
set status(obs_active) 0
set status(dio_a)  0
set status(dio_b)  0
set status(fixwin) 0
set status(joystick_info) None


proc process_data { ev args } {
    global widgets status current
    
    set name [lindex $args 0]
    set val [lindex $args 4]
    switch -glob $name {
	print { terminal_output $val }
	qpcs/em_pos {
	    set status(eye_hor) [lindex $val 0]
	    set status(eye_ver) [lindex $val 1]
	}
	qpcs/state {
	    set status(state) [string totitle $val]
	    set cmap { Running green Stopped red Inactive black }
	    $widgets(status) configure -foreground \
		[dict get $cmap $::status(state)]
	}
	qpcs/obs_id {
	    set status(obs_id) [expr $val+1]
	    set status(obs_info) \
		"$status(obs_id)/$status(obs_total)"
	}
	qpcs/reset {
	    set status(obs_info) {}
	}
	
	qpcs/obs_total {
	    set status(obs_total) $val
	    set status(obs_info) \
		"$status(obs_id)/$status(obs_total)"
	}
	qpcs/obs_active {
	    set status(obs_active) $val
	    set bg [$widgets(obstitle) cget -background]
	    set colors "$bg red"
	    $widgets(obslabel) configure \
		-background [lindex $colors $status(obs_active)]
	    
	    if { $status(obs_active) == 0 } {
		set status(cur_stimtype) {}
	    }
	}
	qpcs/joystick {
	    switch $val {
		0  { set status(joystick_info) None }
		8  { set status(joystick_info) Up }
		4  { set status(joystick_info) Down }
		1  { set status(joystick_info) Left }
		16 { set status(joystick_info) Right }
		2  { set status(joystick_info) Press }
	    }
	}
	qpcs/dio {
	    set levels [lindex $val 1]
	    set status(dio_a) [expr {$levels>>16}]
	    set status(dio_b) [expr {$levels>>24}]
	    update_dio_indicators
	}

	qpcs/system { set current(system) $val; update_system_combos $current(server) }
	qpcs/variant { set current(variant) $val }
	qpcs/protocol { set current(protocol) $val }
	
	qpcs/subject { set status(subject) $val }
	qpcs/datafile { set status(datafile) $val }
    }
}

proc setup {} {
    global status widgets esshosts current

    labelframe .server -text Server
    set f [frame .server.f]
    pack [label $f.text -text "ESS Host:" -width 10] \
	-side left -padx 2 -anchor w
    set widgets(esshost) [ttk::combobox $f.esshost -values $::esshosts -width 16]
    bind $widgets(esshost) <<ComboboxSelected>> [list open_connection %W]
    pack $widgets(esshost) -side left -padx 4 -pady 3
    pack [button $f.refresh -bd 0 -image refreshicon -command refresh_esshosts] -side left -padx 4
    pack $f -side top -pady 2
    pack .server

    labelframe .ess -text "System"
    set f [frame .ess.f -width 70]
    foreach s { system protocol variant } {
	frame $f.$s
	pack [label $f.$s.text -text "[string totitle $s]:" -width 10 -anchor e] \
	    -side left -padx 2 -anchor e
	set widgets(${s}_combo) [ttk::combobox $f.$s.$s  \
				     -width 16 -textvariable current($s)]
	bind $widgets(${s}_combo) <<ComboboxSelected>> [list set_${s} %W]
	pack $widgets(${s}_combo) -side left -padx 4 -pady 3
	pack [button $f.$s.refresh_${s} -bd 0 -image refreshicon -command refresh_${s}] -side left -padx 4
	pack $f.$s
    }
    pack $f -side top -pady 2
    pack .ess

    
    labelframe .control -text "ESS Control"
    set f [frame .control.buttons]
    ttk::button $f.go -text "Go" -command [list server_cmd ess USER_START]
    ttk::button $f.stop -text "Stop" -command [list server_cmd ess USER_STOP]
    ttk::button $f.reset -text "Reset" -command [list server_cmd ess USER_RESET]
    pack $f.go $f.stop $f.reset -side left -expand true -fill x
    pack $f -side bottom -pady 2
    pack .control

    set lf [frame .control.info]
    label $lf.statuslabel -text Status: -anchor e -width 8
    label $lf.statusvalue -textvariable status(state) -anchor w -width 22
    grid $lf.statuslabel $lf.statusvalue -padx 3
    set widgets(status) $lf.statusvalue

    label $lf.subjlabel -text Subject: -anchor e -width 8
    label $lf.subjvalue -textvariable status(subject) -anchor w -width 22
    grid $lf.subjlabel $lf.subjvalue -padx 3 
    
    label $lf.filelabel -text Filename: -anchor e -width 8
    label $lf.filename -textvariable status(datafile) \
	-anchor w -width 22
    grid $lf.filelabel $lf.filename -padx 3
    bind $lf.filelabel <Double-1> {
	clipboard clear; clipboard append $status(datafile)
    }
    pack $lf $f -side top
    

    labelframe .lfobs -text "Trial Info"
    set lfo .lfobs
    set widgets(obstitle) $lfo
    
    set widgets(obslabel) $lfo.obslabel
    label $lfo.obslabel -text Obs: -anchor e -width 8
    label $lfo.obsvalue -textvariable status(obs_info) -anchor w -width 12
    grid $lfo.obslabel $lfo.obsvalue -padx 3 
    
    label $lfo.stimlabel -text StimID: -anchor e -width 8
    label $lfo.stimname -textvariable status(cur_stimtype) -anchor w -width 12
    grid $lfo.stimlabel $lfo.stimname -padx 3 

    
    pack .control .lfobs -fill x -expand true -anchor n
    
    labelframe .essterm -text "Ess Command"
    ttk::entry .essterm.cmd -textvariable ess_command_txt
    text .essterm.output -width 30 -height 6 -takefocus 0 -state disabled
    pack .essterm.cmd -fill x -expand true 
    pack .essterm.output -fill both -expand true
    pack .essterm -fill x -expand true -anchor n
    
    bind .essterm.cmd <Return> { send_cmd .essterm.cmd }
    bind .essterm.cmd <Up> { previous_cmd .essterm.cmd }
    bind .essterm.cmd <Down> { next_cmd .essterm.cmd }

    labelframe .guiterm -text "Essgui Command"
    ttk::entry .guiterm.cmd -textvariable gui_command_txt
    text .guiterm.output -width 30 -height 6 -takefocus 0 -state disabled
    pack .guiterm.cmd -fill x -expand true 
    pack .guiterm.output -fill both -expand true
    pack .guiterm -fill x -expand true -anchor n
    
    bind .guiterm.cmd <Return> { send_cmd .guiterm.cmd }
    bind .guiterm.cmd <Up> { previous_cmd .guiterm.cmd }
    bind .guiterm.cmd <Down> { next_cmd .guiterm.cmd }
    
    set ::ess_command_history {}
    set ::ess_command_index -1
    set ::gui_command_history {}
    set ::gui_command_index -1
}

proc initialize_vars { server } {
    global widgets status
    set stateinfo [qpcs::dsGet $server qpcs/state]
    set status(state) [string totitle [lindex $stateinfo 5]]
    set cmap { Running green Stopped red Inactive black }
    $widgets(status) configure -foreground \
	[dict get $cmap $::status(state)]
    
    set val [lindex [qpcs::dsGet $server qpcs/obs_id] 5]
    set status(obs_id) $val
    
    set val [lindex [qpcs::dsGet $server qpcs/obs_total] 5]
    set status(obs_total) $val
    set status(obs_info) "$status(obs_id)/$status(obs_total)"
    
    set val [lindex [qpcs::dsGet $server qpcs/obs_active] 5]
    set status(obs_active) $val
    set bg [$widgets(obstitle) cget -background]
    set colors "$bg red"
    $widgets(obslabel) configure \
	-background [lindex $colors $status(obs_active)]
    
    if { $status(obs_active) == 0 } {
	set status(cur_stimtype) {}
    }
    
    set status(fixwin) [lindex [lindex [qpcs::dsGet $server qpcs/em_region_status] 5] 1]
    if { $status(fixwin) == "" } { set status(fixwin) 0 }
#    update_fixwin_indicators
    
    set dpoint [qpcs::dsGet $server qpcs/dio]
    set levels [lindex [lindex $dpoint 5] 1]
    set status(dio_a) [expr {$levels>>16}]
    set status(dio_b) [expr {$levels>>24}]
#    update_dio_indicators
    
    set status(subject) [lindex [qpcs::dsGet $server qpcs/subject] 5]
    set status(name) [lindex [qpcs::dsGet $server qpcs/name] 5]
    set status(datafile) [lindex [qpcs::dsGet $server qpcs/datafile] 5]
}

proc update_vars { server } {
    foreach v { system protocol variant } { qpcs::dsTouch $server qpcs/$v }
}

proc connect_to_server { server } {
    global current
    
    initialize_vars $server
    if { [qpcs::dsRegister $server] != 1 } {
	error "Unable to register with $server"
    }
    qpcs::dsAddCallback process_data
    qpcs::dsAddMatch $server qpcs/*
    qpcs::dsAddMatch $server print

    set current(server) $server
    update_vars $server
    
    set connected 1
}

proc ess_cmd { args } {
    global current
    set sock [socket $current(server) 2570]
    fconfigure $sock -buffering line
    puts $sock $args
    set result [gets $sock]
    close $sock
    return $result
}

proc set_system { w } { ess_cmd ess::load_system [$w get] }
proc set_protocol { w } {
    global current
    ess_cmd ess::load_system $current(system) [$w get]
}

proc set_variant { w } {
    global current
    ess_cmd ess::load_system $current(system) $current(protocol) [$w get]
}

proc update_system_combos { server } {
    global current widgets
    set result [ess_cmd ess::get_system_dict]
    foreach v "system protocol variant" { set current(${v}_list) {} }
    dict for { sys prot_var } $result {
	lappend current(system_list) $sys
	if { $prot_var != "" } {
	    dict for { prot var } $prot_var {
		lappend current(protocol_list) $prot
		if { $var != "" } {
		    foreach v $var {
			lappend current(variant_list) $v
		    }
		}
	    }
	}
    }
    $widgets(system_combo) configure -values $current(system_list)
    $widgets(protocol_combo) configure -values $current(protocol_list)
    $widgets(variant_combo) configure -values $current(variant_list)
}

proc disconnect_from_server {} {
    qpcs::dsUnregister 
}

proc add_to_history { server cmd } {
    if { $server == "ess" } {
	set command_history ::ess_command_history
	set command_index ::ess_command_index
    } else {
	set command_history ::gui_command_history
	set command_index ::gui_command_index
    }
    set lastcmd [lindex [set $command_history] end]
    if { ![string match $cmd $lastcmd] } {
	lappend $command_history $cmd
    }
    set $command_index [llength [set $command_history]]
}

proc previous_cmd { w } {
    if { $w == ".essterm.cmd" } {
	set command_history ::ess_command_history
	set command_index ::ess_command_index
	set command_txt ::ess_command_txt
    } else {
	set command_history ::gui_command_history
	set command_index ::gui_command_index
	set command_txt ::gui_command_txt
    }
    if { [set $command_index] >= 0 } {
	incr $command_index -1
	set cmd [lindex [set $command_history] [set $command_index]]
	set $command_txt $cmd
	$w icursor end
    }
}

proc next_cmd { w } {
    if { $w == ".essterm.cmd" } {
	set command_history ::ess_command_history
	set command_index ::ess_command_index
	set command_txt ::ess_command_txt
    } else {
	set command_history ::gui_command_history
	set command_index ::gui_command_index
	set command_txt ::gui_command_txt
    }
    if { [set $command_index] < [expr [llength [set $command_history]]-1] } {
	incr $command_index 1
	set cmd [lindex [set $command_history] [set $command_index]]
	set $command_txt $cmd
	$w icursor end
    }
}

proc send_cmd { w } {
    set new_command [$w get]
    if { $w == ".essterm.cmd" } {
	set in ess_command_txt
	set out .essterm.output
	set server ess
    } else {
	set in gui_command_txt
	set out .guiterm.output
	set server gui
    }

    $out configure -state normal
    $out delete 1.0 end
    $out configure -state disabled
    server_cmd $server $new_command 1

    global $in
    set $in {}
}


proc terminal_output { line } {
    .essterm.output configure -state normal
    .essterm.output insert end $line
    .essterm.output insert end \n
    .essterm.output configure -state disabled
}

proc echo_line { s } {
    set line [gets $s]
    terminal_output $line
}

proc server_open { { host 127.0.0.1 } } {
    global sock
    set sock [socket $host 2570]
    fconfigure $sock -buffering line
    fileevent $sock readable [list echo_line $sock]
}

proc server_close {} {
    global sock
    if { $sock != {} } { close $sock }
}

proc server_cmd { server cmd { add 0 } } {
    if { $server == "ess" } {
	global sock
	if { $sock != {} } { puts $sock $cmd }
    } else {
	set result [namespace inscope :: eval $cmd]
	.guiterm.output configure -state normal
	.guiterm.output insert end $result
	.guiterm.output insert end \n
	.guiterm.output configure -state disabled	
    }
    if { $add } { add_to_history $server $cmd }
}


proc find_esshosts {} {
    global esshosts esshostinfo
    set esshosts {}
    array set esshostinfo {}
    
    set hosts [mdns::find _dserv._tcp 200]
    foreach hinfo $hosts {
	lassign $hinfo host info
	lappend esshosts $host
	set esshostinfo($host) $info
    }
}

proc refresh_esshosts {} {
    find_esshosts
    $::widgets(esshost) configure -values $::esshosts
    if { $::dserv_server != {} } {
	if { [lsearch $::esshosts $::dserv_server] == -1 } {
	    disconnect
	} else {
	    $::widgets(esshost) set $::dserv_server
	}
    }
}

set dir [file dirname [info script]]
set iconfile [file join $dir  play.png]
image create photo essicon -file $iconfile

set refreshfile [file join $dir refresh_small.png]
image create photo refreshicon -file $refreshfile

wm title . "Experimental Control"
wm iconphoto . -default essicon

# hold onto host information
set esshosts {}
array set esshostinfo {}

foreach s "system protocol variant" {
    set current(${s}) {}
    set current(${s}_list) {}
}

find_esshosts
setup

proc open_connection { w } {
    if { $::dserv_server != "" } { disconnect }
    connect [$w get]
}

proc connect { server } {
    set ::dserv_server $server
    server_open $server
    connect_to_server $server
}

proc disconnect {} {
    global dserv_server
    disconnect_from_server
    server_close
    set dserv_server {}
}

set dserv_server {}

#if { [llength $argv] > 0 } { set server [lindex $argv 0] } { set server 127.0.0.1 }
#connect $server
