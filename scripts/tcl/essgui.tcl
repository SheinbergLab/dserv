#
# essgui.tcl
#


if {[tk windowingsystem] eq "aqua"} {
    lappend auto_path /usr/local/lib /usr/local/lib/tcltk
}

package require Tk
package require qpcs
package require mdns
package require dlsh
package require tablelist

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
	qpcs/protocol { set current(protocol) $val; update_system_combos $current(server) }
	qpcs/variant { set current(variant) $val; update_system_combos $current(server) }
	
	qpcs/subject { set status(subject) $val }
	qpcs/datafile { set status(datafile) $val }
	stimdg { dg_fromString64 $val; if [winfo exists .stimdg] { dg_view stimdg .stimdg }  }
    }
}

proc goCommand {} { server_cmd ess USER_START }
proc stopCommand {} { server_cmd ess USER_STOP }
proc resetCommand {} { server_cmd ess USER_RESET }


# This is a helper procedure to check if the datafile exists when
# opening it
proc openWithCheck { overwrite } {

    # TODO
    # implement ess::file_exists for this...
    
    set exists [file exists $::workingDataFile]
    if {$exists && $overwrite } {
	overwriteDataFileDialog
    } else {
	# then we have opened a datafile
	set ::currentDatafile $::workingDataFile
	clipboard clear
	clipboard append $::currentDatafile
	
	# Add to sqlite3 database
#	add_current_file_to_database	    
	
	# update block counter
	set this_block [string range $::currentDatafile \
			    [expr [string length $::currentDatafile]-3] end]
	set ::blockCounter [string trimleft $this_block 0]

	server_cmd ess [list ess::file_open $::currentDatafile]
    }
    if [winfo exists .opendatafile] {
	destroy .opendatafile
    }
    if {$overwrite == 0} {
	destroy .overwritedatafile
    }
}

# This procedure opens the dialog for a data file overwrite
proc overwriteDataFileDialog { } {
    set dfiletoplevel [toplevel .overwritedatafile]
    wm resizable $dfiletoplevel 0 0
    wm title $dfiletoplevel "Load Data File" 
    
    bind $dfiletoplevel <Escape> [list destroy $dfiletoplevel ]
    bind $dfiletoplevel <Return> {openWithCheck 0}
    
    pack [label $dfiletoplevel.label -text "File Exists ($::workingDataFile): Overwrite?"] \
	-side top -pady 3 -padx 3
    pack [set buttonframe [frame $dfiletoplevel.buttonframe]] \
	-side top -pady 3
    pack [button $buttonframe.ok -text "Ok" -command \
	      [list stimgui::openWithCheck 0] -width 8 -height 1 ] \
	-side left 	
    pack [button $buttonframe.cancel -text "Cancel" -command \
	      [list destroy $dfiletoplevel] -width 8 -height 1 ] -side left
    
    shared::placeWin $dfiletoplevel 50 25
    
    focus $dfiletoplevel
    grab $dfiletoplevel
    tkwait window $dfiletoplevel
}

# This procedure opens the dialog for a data file openeing
proc openDataFileDialog { } {
    set dfiletoplevel [toplevel .opendatafile]
    wm resizable $dfiletoplevel 0 0
    wm title $dfiletoplevel "Open Data File" 
    
    bind $dfiletoplevel <Escape> [list destroy $dfiletoplevel ]
    bind $dfiletoplevel <Return> {openWithCheck 1}
    
    set data_entry [entry $dfiletoplevel.filename \
			-textvariable ::workingDataFile -width 30]

    $data_entry icursor end
    pack $data_entry -side left -padx 3 -pady 3
    pack [button $dfiletoplevel.open -text "Open" -command \
	      [list openWithCheck 1 ]] \
	-side left -padx 3 -pady 3
    pack [button $dfiletoplevel.cancel -text "Cancel" -command \
	      [list destroy $dfiletoplevel]] -side left -padx 3 -pady 3
    
    pack [set suggest [button $dfiletoplevel.suggest -text "Suggest File" \
			   -command stimgui::suggestDataFile]] \
	-side left -padx 3 -pady 3
    
    set ::overwriteSuggestionButton \
	[button $dfiletoplevel.overwrite -text "Overwrite Suggestion" \
	     -command ::overwriteSuggestion -state disabled]
    pack $::overwriteSuggestionButton -side left -padx 3 -pady 3
    
#    shared::placeWin $dfiletoplevel 25 25
    
    focus $dfiletoplevel.filename
    grab $dfiletoplevel
    tkwait window $dfiletoplevel
}

proc openDatafile {} {
    openDataFileDialog
}

proc closeDatafile {} {
    if { $::currentDatafile != {} } {
	server_cmd ess [list ess::file_close]
	set ::currentDatafile {}
    }
}

proc setBindings { t} {
    #if you add a binding please update the stimgui::bindings variable
    #  you can see examples below.
    
    set ::bindings ""
    
    if { $t == "" } { set t .}
    
    bind $t <Control-h> { if [winfo exists .cli] { wm deiconify .cli } { command_window } }
    
    append ::bindings "[format %-10s {Ctrl-h:}] open console\n"
    bind $t <Control-x> { exitCommand }
    append ::bindings "[format %-10s {Ctrl-x:}] exit stimgui\n"
    bind $t <Control-d> { setupDebug }
    append ::bindings "[format %-10s {Ctrl-d:}] open debugging tools window\n"
    #	bind $t <Control-s> { toggleStatusFrame }
    append ::bindings "[format %-10s {Ctrl-s:}] toggle status frame\n"
    
    bind $t <Control-v> {viewStimdg }
    append ::bindings "[format %-10s {Ctrl-v:}] view current stimdg\n"
    bind $t <Control-i> {viewStimdg info}
    append ::bindings "[format %-10s {Ctrl-i:}] info about current stimdg\n"
    
    # experimental controls
    bind $t <g> { goCommand }
    bind $t <G> { goCommand }
    append ::bindings "[format %-10s {G or g:}] Go\n"
    bind $t <q> { stopCommand }
    bind $t <Q> { stopCommand }
    append ::bindings "[format %-10s {Q or q:}] Quit\n"
    bind $t <r> { resetCommand }
    bind $t <R> { resetCommand }
    append ::bindings "[format %-10s {R or r:}] Reset\n"
    # this command is not valid yet; needs more testing to be implimented
    bind $t <j> { juiceCommand }
    bind $t <J> { juiceCommand }
    append ::bindings "[format %-10s {J or j:}] Juice\n"
    
    # param file editing
#    bind $t <p> { if {$ESSConnectionExists && \
#			  !$systemRunning} {editParams} }
#    bind $t <P> { if {$ESSConnectionExists && \
#			  !$systemRunning} {editParams} }
#    append ::bindings "[format %-10s {P or p:}] open params editor\n"
    
    #this commented code includes shortcuts taken from QNX Essgui, but
    #  they aren't as useful since P/p (params viewer) incorporates all
    # 	bind $t <v> { if {$ESSConnectionExists && \
	# 		!$systemRunning} {editParams} }
    # 	bind $t <V> { if {$ESSConnectionExists && \
	# 		!$systemRunning} {editParams} }
    # 	bind $t <t> { if {$ESSConnectionExists && \
	# 		!$systemRunning} {editParams} }
    # 	bind $t <T> { if {$ESSConnectionExists && \
	# 		!$systemRunning} {editParams} }
    
    
    # data file commands
    bind $t <Control-o> { if {$::currentDatafile == ""} \
			      { openDatafile } }
    append ::bindings "[format %-10s {Ctrl-o:}] open data file\n"
    bind $t <Control-c> { if {$::currentDatafile != ""} \
			      { closeDatafile } }
    append ::bindings "[format %-10s {Ctrl-c:}] close data file\n"
    bind $t <Control-r> { if {$::currentDatafile != ""} \
			      { closeDatafileReload } }
    append ::bindings "[format %-10s {Ctrl-c:}] close data file\n"
    bind $t <Control-N> { quickNextFile }
    append ::bindings "[format %-10s {Ctrl-Shift-N:}] quick next file\n"
    
    
    # i/o settings
#    bind $t <e> { if {$ESSConnectionExists && \
#			  !$systemRunning} {buildIOMenu} }
#    bind $t <E> { if {$ESSConnectionExists && \
#			  !$systemRunning} {buildIOMenu} }
#    append ::bindings "[format %-10s {E or e:}] edit I/O options\n"
}

proc exitCommand { } {
    set top [toplevel .closedialouge]
    wm resizable $top 0 0
    wm title $top "Close essgui?" 
    
    bind $top <Escape> [list destroy $top]
    bind $top <Return> { exit }
    
    pack [label $top.label -text "Close essgui?" -fg red] \
	-side top -pady 3 -padx 3
    pack [set buttonframe [frame $top.buttonframe]] \
	-side top -pady 3 -padx 3
    pack [button $buttonframe.ok -text "Ok" -command \
	      exit -width 8 -height 1 ] \
	-side left
    pack [button $buttonframe.cancel -text "Cancel" -command \
	      [list destroy $top] -width 8 -height 1 ] -side left
    
#    shared::placeWin $top 50 50
    
    focus $top
    grab $top
    tkwait window $top
}

proc viewStimdg { {action view} } {
    if { ![dg_exists stimdg] } {
	dg_rename [qpcs::dsGetDG $::current(server) stimdg] stimdg
    }
    switch -exact $action {
	info {
	    set m ""
	    if [dl_exists stimdg:version] {
		set m "$m [dl_tcllist stimdg:version]\n"
	    }
	    if [dl_exists stimdg:remaining] {
		set m "$m NObs = [dl_length stimdg:remaining]\n"
	    } elseif [dl_exists stimdg:id] {
		set m "$m NObs = [dl_length stimdg:remaining]\n"
	    }
	    stimgui::messageWin $m info "Stimdg - NObs"
	}
	view {
	    dg_view stimdg .stimdg
	}
    }
}

##################
# command_window
##################

proc command_window {} {
    if { [winfo exists .cli] } { wm deiconify .cli; return }
    toplevel .cli
    wm title .cli "Command Window"
    wm geometry .cli 600x300
    
    labelframe .cli.essterm -text "Ess Command"
    ttk::entry .cli.essterm.cmd -textvariable ess_command_txt
    text .cli.essterm.output -width 30 -height 6 -takefocus 0 -state disabled
    pack .cli.essterm.cmd -fill x -expand true 
    pack .cli.essterm.output -fill both -expand true
    pack .cli.essterm -fill x -expand true -anchor n
    
    bind .cli.essterm.cmd <Return> { send_cmd .cli.essterm.cmd }
    bind .cli.essterm.cmd <Up> { previous_cmd .cli.essterm.cmd }
    bind .cli.essterm.cmd <Down> { next_cmd cli.essterm.cmd }

    labelframe .cli.guiterm -text "Essgui Command"
    ttk::entry .cli.guiterm.cmd -textvariable gui_command_txt
    text .cli.guiterm.output -width 30 -height 6 -takefocus 0 -state disabled
    pack .cli.guiterm.cmd -fill x -expand true 
    pack .cli.guiterm.output -fill both -expand true
    pack .cli.guiterm -fill x -expand true -anchor n
    
    bind .cli.guiterm.cmd <Return> { send_cmd .cli.guiterm.cmd }
    bind .cli.guiterm.cmd <Up> { previous_cmd .cli.guiterm.cmd }
    bind .cli.guiterm.cmd <Down> { next_cmd .cli.guiterm.cmd }
    
    set ::ess_command_history {}
    set ::ess_command_index -1
    set ::gui_command_history {}
    set ::gui_command_index -1

    wm withdraw .cli
}


proc setup {} {
    global status widgets esshosts current

    # A menubar with some options (server info...etc)
    set widgets(Menu) [set menu [menu .menu \
				     -tearoff 0]]
    . configure -menu $menu
    foreach m {File Edit View Actions Help} {
	set $m [menu $menu.menu$m -tearoff 0]
	$menu add cascade -label $m -menu $menu.menu$m
	set widgets($m) .menu.menu$m
    }
    
    $File add cascade -label "Data File" -menu $File.datacascade
    set widgets(DataCascade) [set DataCascade \
					   [menu $File.datacascade -tearoff 0]]
    $DataCascade add command -label "Open" -command \
	openDatafile -accelerator (Ctrl-o)
    $DataCascade add command -label "Close" -command \
	closeDatafile -accelerator (Ctrl-c)
    $DataCascade add command -label "Close and reload stimdg" -command \
	closeDatafileReload -accelerator (Ctrl-r)
    
    $File add cascade -label "Param File" -menu $File.paramcascade
    set widgets(ParamCascade) [set ParamCascade \
					    [menu $File.paramcascade -tearoff 0]]
    $ParamCascade add command -label "Load" -command \
	loadParamsFileWin
    $ParamCascade add command -label "For Save, see edit menu" -command return -state disabled
    $File add separator
    
    $File add command -label "Save Subject Defaults" \
	-command updateDefaultsFile
    
    $File add separator
    $File add command -label "Exit" -command exitCommand \
	-accelerator (Ctrl-x)


    $View add command -label "Current stimdg" -command viewStimdg -accelerator (Ctrl-v)
    $View add command -label "Current stimdg Info" -command {stimgui::viewStimdg info} -accelerator (Ctrl-i)
    $View add separator

    $View add command -label "QPCS Viewer" -command \
	{exec wish8.6 /usr/local/dserv/scripts/tcl/qpcsview.tcl $::current(server) &}
    
    $View add command -label "Event Viewer" -command \
	{exec wish8.6 /usr/local/dserv/scripts/tcl/essview.tcl $::current(server) &}

    $View add command -label "Trace Viewer" -command \
	{openTraceViewer}
    $View add separator
    $View add command -label "Virtual Inputs" -command \
	{exec wish8.6 /usr/local/dserv/scripts/tcl/essinput.tcl $::current(server) &}
    
    $View add separator
    $View add command -label "Datafile Suggestion Database" \
	-command {
	    set g [dg_read interface/datafile_suggestions]
	    dg_view [dg_sort $g system protocol variant]
	    dg_delete $g
	}
    $View add separator
    $View add check -label "Play Protocol Sound" \
	-variable stimgui::playProtSound

    $View add separator
    $View add command -label "Debugging Tools" -command \
	stimgui::setupDebug -accelerator (Ctrl-d)
    $View add command -label "Console" -command \
	{ command_window } -accelerator (Ctrl-h)
    $View add check -label "Display Update Status Time?" \
	-variable ::stimgui::showUpdateStatusEveryTime
    #	$View add command -label "Show Status" -command stimgui::toggleStatusFrame -accelerator (Ctrl-s)
    $Help add command -label "Shortcut Commands" -command \
	stimgui::shortcutWin
    $Help add command -label "Params File Sourcing" -command \
	{stimgui::paramFileHelp .}
    $Help add separator
    $Help add command -label "About" \
	-command {shared::aboutWin stimgui $stimgui::codeVersion \
		      "REHBM, DLS" 2001-2021}
    


    labelframe .server -text Server
    set f [frame .server.f]
    pack [label $f.text -text "ESS Host:" -width 10] \
	-side left -padx 2 -anchor w
    set widgets(esshost) \
	[ttk::combobox $f.esshost -state readonly \
	     -values $::esshosts -width 16 -textvariable ::esshost]
    bind $widgets(esshost) <<ComboboxSelected>> [list open_connection %W]
    pack $widgets(esshost) -side left -padx 4 -pady 3
    pack [button $f.refresh -bd 0 -image refreshicon -command refresh_esshosts] -side left -padx 4
    pack $f -side top -pady 2
    pack .server -fill x -expand true -anchor n

    labelframe .ess -text "System"
    set f [frame .ess.f -width 70]
    foreach s { system protocol variant } {
	frame $f.$s
	pack [label $f.$s.text -text "[string totitle $s]:" -width 10 -anchor e] \
	    -side left -padx 2 -anchor e
	set widgets(${s}_combo) [ttk::combobox $f.$s.$s -state readonly \
				     -width 16 -textvariable current($s)]
	bind $widgets(${s}_combo) <<ComboboxSelected>> [list set_${s} %W]
	pack $widgets(${s}_combo) -side left -padx 4 -pady 3
	pack [button $f.$s.refresh_${s} -bd 0 ] -side left -padx 4
	pack $f.$s
    }
    pack $f -side top -pady 2
    pack .ess -fill x -expand true -anchor n

    
    labelframe .control -text "ESS Control"
    set f [frame .control.buttons]
    ttk::button $f.go -text "Go" -command goCommand
    ttk::button $f.stop -text "Stop" -command stopCommand
    ttk::button $f.reset -text "Reset" -command resetCommand
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

    # open the command window for sending commands to ess/stim/local
    command_window
    
    setBindings {}
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
    qpcs::dsAddMatch $server stimdg

    set current(server) $server
    update_vars $server

    update_system_combos $server
    
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
		if { $var != "" && $current(protocol) == $prot } {
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
    if { $w == ".cli.essterm.cmd" } {
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
    if { $w == ".cli.essterm.cmd" } {
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
    if { $w == ".cli.essterm.cmd" } {
	set in ess_command_txt
	set out .cli.essterm.output
	set server ess
    } else {
	set in gui_command_txt
	set out .cli.guiterm.output
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
    .cli.essterm.output configure -state normal
    .cli.essterm.output insert end $line
    .cli.essterm.output insert end \n
    .cli.essterm.output configure -state disabled
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
	.cli.guiterm.output configure -state normal
	.cli.guiterm.output insert end $result
	.cli.guiterm.output insert end \n
	.cli.guiterm.output configure -state disabled	
    }
    if { $add } { add_to_history $server $cmd }
}


proc find_esshosts {} {
    global esshosts esshostinfo
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
set esshost ""
set currentDatafile {}

foreach s "system protocol variant" {
    set current(${s}) {}
    set current(${s}_list) {}
}

#find_esshosts
setup

proc open_connection { w } {
    if { $::dserv_server != "" } {
	disconnect
	if [dg_exists stimdg] { dg_delete stimdg }
	if [winfo exists .stimdg] { destroy .stimdg }
    }
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

if { [llength $argv] > 0 } {
    set server [lindex $argv 0]
    lappend ::esshosts $server    
    set ::esshost $server
    connect $server
}


#########
# dgview
#########

proc dg_view { dg { top {} } } {
    #
    # Create a toplevel widget of the class DgView
    #
    if { $top == "" } {
	set top .dgView
	for {set n 2} {[winfo exists $top]} {incr n} {
	    set top .dgView$n
	}
	toplevel $top -class DgView
	wm title $top $dg
    } else {
	if [winfo exists $top] {
	    destroy $top.tf
	} else {
	    toplevel $top -class DgView
	    wm title $top $dg
	}

    }
    
    #
    # Create a vertically scrolled tablelist widget with dynamic-width
    # columns and interactive sort capability within the toplevel
    #
    set tf $top.tf
    frame $tf
    set tbl $tf.tbl
    set vsb $tf.vsb
    set hsb $tf.hsb

    set colinfo {}
    set maxrows 0

    #
    # Create column list and determine lengths
    #
    set colnames [dg_tclListnames $dg]
    foreach c $colnames {
	set colinfo "$colinfo 0 $c left"
	set collen [dl_length $dg:$c]
	if { $collen > $maxrows } { set maxrows $collen }
    }

    set ncols [llength $colnames]
    set nrows $maxrows

    tablelist::tablelist $tbl \
        -columns $colinfo \
        -xscrollcommand [list $hsb set] -yscrollcommand [list $vsb set] \
	-setgrid no -width 0
    if {[$tbl cget -selectborderwidth] == 0} {
        $tbl configure -spacing 1
    }

    scrollbar $vsb -orient vertical -command [list $tbl yview]    
    scrollbar $hsb -orient horizontal -command [list $tbl xview]

    for { set i 0 } { $i < $ncols } { incr i } {
	$tbl columnconfigure $i -maxwidth 18
    }
    

    #
    # Manage the widgets
    #
    grid $tbl -row 0 -rowspan 2 -column 0 -sticky news
    if {[tk windowingsystem] eq "win32"} {
        grid $vsb -row 0 -rowspan 2 -column 1 -sticky ns
    } else {
        grid [$tbl cornerpath] -row 0 -column 1 -sticky ew
        grid $vsb              -row 1 -column 1 -sticky ns
    }
    grid $hsb -row 2 -column 0 -sticky ew
    grid rowconfigure    $tf 1 -weight 1
    grid columnconfigure $tf 0 -weight 1
    pack $tf -side top -expand yes -fill both


    #
    # Fill the table
    #
    for { set i 0 } { $i < $nrows } { incr i } {
	set row {}
	for { set j 0 } { $j < $ncols } { incr j } {
	    set c [lindex $colnames $j]
	    set clen [dl_length $dg:$c]
	    if { $i < $clen } {  
		set entry [dl_tcllist $dg:$c:$i]
	    } else {
		set entry {}
	    }
	    if { $entry != {} } { lappend row $entry } { lappend row {} }
	}
	$tbl insert end $row
    }

    return $tbl
}

