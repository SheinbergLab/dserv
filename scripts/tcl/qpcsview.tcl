#
# Procs for interacting with a datahub
#


set dlshlib [file join /usr/local dlsh dlsh.zip]
if [file exists $dlshlib] {
    set base [file join [zipfs root] dlsh]
   zipfs mount $dlshlib $base
   set auto_path [linsert $auto_path [set auto_path 0] $base/lib]
}

package require Tk
package require qpcs

font create myDefaultFont -family {Helvetica} -size 11
option add *font myDefaultFont

proc server_cmd { server cmd } {
    set sock [socket $server 2570]
    fconfigure $sock -buffering line
    puts $sock $cmd
    set result [gets $sock]
    close $sock
    return $result
}

namespace eval dh {
    set connected 0

    set datahub 127.0.0.1
    set domain  ess/*

    set ::qpcs::qnxport 2570
    
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

    proc initialize_vars { server } {
	set stateinfo [qpcs::dsGet $server ess/state]
	set ::dh::status(state) [string totitle [lindex $stateinfo 5]]
	set cmap { Running green Stopped red Inactive black }
	$::dh::widgets(status) configure -foreground \
	    [dict get $cmap $::dh::status(state)]

	set val [lindex [qpcs::dsGet $server ess/obs_id] 5]
	set ::dh::status(obs_id) $val

	set val [lindex [qpcs::dsGet $server ess/obs_total] 5]
	set ::dh::status(obs_total) $val
	set ::dh::status(obs_info) "$::dh::status(obs_id)/$::dh::status(obs_total)"

	set val [lindex [qpcs::dsGet $server ess/obs_active] 5]
	set ::dh::status(obs_active) $val
	set bg [$::dh::widgets(obstitle) cget -background]
	set colors "$bg red"
	$::dh::widgets(obslabel) configure \
	    -background [lindex $colors $::dh::status(obs_active)]
		
	if { $::dh::status(obs_active) == 0 } {
	    set ::dh::status(cur_stimtype) {}
	}

	set ::dh::status(fixwin) \
	    [lindex [lindex [qpcs::dsGet $server ess/em_region_status] 5] 1]
	if { $::dh::status(fixwin) == "" } { set ::dh::status(fixwin) 0 }
	update_fixwin_indicators
	
	set dpoint [qpcs::dsGet $server ess/dio]
	set levels [lindex [lindex $dpoint 5] 1]
	set ::dh::status(dio_a) [expr {$levels>>16}]
	set ::dh::status(dio_b) [expr {$levels>>24}]
	update_dio_indicators
	
	set ::dh::status(subject) [lindex [qpcs::dsGet $server ess/subject] 5]
	set ::dh::status(name) [lindex [qpcs::dsGet $server ess/name] 5]
	set ::dh::status(datafile) [lindex [qpcs::dsGet $server ess/datafile] 5]
    }

    proc connect_to_server { server ess } {
	initialize_vars $server
	if { [qpcs::dsRegister $server] != 1 } {
	    error "Unable to register with $server"
	}
	qpcs::dsAddCallback ::dh::process_data
	qpcs::dsAddMatch $server ess/*
	set ::dh::connected 1
    }

    proc disconnect_from_server {} {
	qpcs::dsUnregister 
    }

    proc reconnect {} {
	disconnect_from_server
	connect_to_server $::dh::datahub $::dh::domain
    }

    proc update_em_regions {} {
	foreach reg "0 1 2 3 4 5 6 7" {
	    server_cmd $::dh::datahub "ainGetRegionInfo $reg"
	}
    }
    
    proc resize_disp_canvas { w h } {
	set ::dh::status(disp_canvas_width) $w 
	set ::dh::status(disp_canvas_height) $h
	::dh::update_eye_marker $::dh::status(eye_hor) $::dh::status(eye_ver)	
    }

    proc add_fixwin_indicators {} {
	for { set i 0 } { $i < 8 } { incr i } {
	    $::dh::widgets(fixwin_canvas) \
		create oval 0 0 1 1 -fill black -tag reg$i
	}
	position_fixwin_indicators
    }

    proc position_fixwin_indicators {} {
	set msize 5
	set hwidth [expr $::dh::status(fixwin_canvas_width)/2]
	set hheight [expr $::dh::status(fixwin_canvas_height)/2]
	
	set x [expr {8+($hwidth+$msize)/2}]
	set inc [expr ($hwidth-(2*(8-$msize)))/8.]
	for { set i 0 } { $i < 8 } { incr i } {
	    $::dh::widgets(fixwin_canvas) coords reg[expr 7-$i] \
		[expr $x-$msize] [expr $hheight-$msize] \
		[expr $x+$msize] [expr $hheight+$msize]
	    set x [expr $x+$inc]
	}
    }

    
    proc update_fixwin_indicators {} {
	for { set i 0 } { $i < 8 } { incr i } {
	    if { [expr $::dh::status(fixwin)&(1<<$i)] != 0 } {
		$::dh::widgets(fixwin_canvas) itemconfigure reg$i -fill yellow
	    } else {
		$::dh::widgets(fixwin_canvas) itemconfigure reg$i -fill black
	    }
	}
    }


    
    proc add_dio_indicators {} {
	for { set i 0 } { $i < 8 } { incr i } {
	    $::dh::widgets(dio_canvas) create oval 0 0 1 1 -fill red -tag A$i
	}

	for { set i 0 } { $i < 8 } { incr i } {
	    $::dh::widgets(dio_canvas) create oval 0 0 1 1 -fill red -tag B$i
	}
	position_dio_indicators
    }
    
    proc position_dio_indicators {} {
	set msize 5
	set hwidth [expr $::dh::status(dio_canvas_width)/2]
	set hheight [expr $::dh::status(dio_canvas_height)/2]
	
	set x 8
	set inc [expr ($hwidth-(2*($x-$msize)))/8.]
	for { set i 0 } { $i < 8 } { incr i } {
	    $::dh::widgets(dio_canvas) coords A[expr 7-$i] \
		[expr $x-$msize] [expr $hheight-$msize] \
		[expr $x+$msize] [expr $hheight+$msize]
	    set x [expr $x+$inc]
	}

	set x [expr $hwidth+8]
	for { set i 0 } { $i < 8 } { incr i } {
	    $::dh::widgets(dio_canvas) coords B[expr 7-$i] \
		[expr $x-$msize] [expr $hheight-$msize] \
		[expr $x+$msize] [expr $hheight+$msize]
	    set x [expr $x+$inc]
	}
			     
    }

    proc update_dio_indicators {} {
	for { set i 0 } { $i < 8 } { incr i } {
	    if { [expr $::dh::status(dio_a)&(1<<$i)] != 0 } {
		$::dh::widgets(dio_canvas) itemconfigure A$i -fill green
	    } else {
		$::dh::widgets(dio_canvas) itemconfigure A$i -fill red
	    }
	}
	for { set i 0 } { $i < 8 } { incr i } {
	    if { [expr $::dh::status(dio_b)&(1<<$i)] != 0 } {
		$::dh::widgets(dio_canvas) itemconfigure B$i -fill green
	    } else {
		$::dh::widgets(dio_canvas) itemconfigure B$i -fill red
	    }
	}
    }

    proc resize_fixwin_canvas { w h } {
	set ::dh::status(fixwin_canvas_width) $w 
	set ::dh::status(fixwin_canvas_height) $h
	position_fixwin_indicators
    }

    proc resize_dio_canvas { w h } {
	set ::dh::status(dio_canvas_width) $w 
	set ::dh::status(dio_canvas_height) $h
	position_dio_indicators
    }
    
    proc update_em_region_setting { reg active state type cx cy dx dy args } {
	set sx [expr ($cx-2048)/200.]
	set sy [expr -1*($cy-2048)/200.]
	set w $::dh::status(disp_canvas_width)
	set h $::dh::status(disp_canvas_height)
	set aspect [expr {1.0*$h/$w}]
	set range_h 20.0
	set hrange_h [expr {0.5*$range_h}]
	set range_v [expr {$range_h*$aspect}]
	set hrange_v [expr {0.5*$range_v}]
	set pix_per_deg_h [expr $w/$range_h]
	set pix_per_deg_v [expr $h/$range_v]
	set msize_x [expr {($dx/200.)*$pix_per_deg_h}]
	set msize_y [expr {($dy/200.)*$pix_per_deg_v}]
	set csize 2
	set hw [expr $w/2]
	set hh [expr $h/2]
	set x0 [expr (($sx/$hrange_h)*$hw)+$hw]
	set y0 [expr $hh-(($sy/$hrange_v)*$hh)]
	if { $active == 1 } { set cstate normal } { set cstate hidden }
	if  { $type == 1 } { 
	    $::dh::widgets(disp_canvas) \
		coords $::dh::widgets(em_reg_ellipse_$reg) \
		[expr $x0-$msize_x] [expr $y0-$msize_y] \
		[expr $x0+$msize_x] [expr $y0+$msize_y]
	    $::dh::widgets(disp_canvas) \
		coords $::dh::widgets(em_reg_center_$reg) \
		[expr $x0-$csize] [expr $y0-$csize] \
		[expr $x0+$csize] [expr $y0+$csize]
	    $::dh::widgets(disp_canvas) itemconfigure regE$reg -state $cstate
	    $::dh::widgets(disp_canvas) itemconfigure regC$reg -state $cstate
	    $::dh::widgets(disp_canvas) itemconfigure regR$reg -state hidden
	} else {
	    $::dh::widgets(disp_canvas) \
		coords $::dh::widgets(em_reg_rect_$reg) \
		[expr $x0-$msize_x] [expr $y0-$msize_y] \
		[expr $x0+$msize_x] [expr $y0+$msize_y]
	    $::dh::widgets(disp_canvas) \
		coords $::dh::widgets(em_reg_center_$reg) \
		[expr $x0-$csize] [expr $y0-$csize] \
		[expr $x0+$csize] [expr $y0+$csize]
	    $::dh::widgets(disp_canvas) itemconfigure regR$reg -state $cstate
	    $::dh::widgets(disp_canvas) itemconfigure regC$reg -state $cstate
	    $::dh::widgets(disp_canvas) itemconfigure regE$reg -state hidden
	}
    }

    proc update_eye_marker { x y } {
	set sx [expr ($x-2048)/200.]
	set sy [expr -1*($y-2048)/200.]
	set range 20.0
	set w $::dh::status(disp_canvas_width)
	set h $::dh::status(disp_canvas_height)
	set aspect [expr {1.0*$h/$w}]
	set hrange_h 10.0
	set hrange_v [expr $hrange_h*$aspect]
	set msize 3
	set hw [expr $w/2]
	set hh [expr $h/2]
	set x0 [expr (($sx/$hrange_h)*$hw)+$hw]
	set y0 [expr $hh-(($sy/$hrange_v)*$hh)]
	$::dh::widgets(disp_canvas) \
	    coords $::dh::widgets(em_marker) [expr $x0-$msize] [expr $y0-$msize] \
	    [expr $x0+$msize] [expr $y0+$msize] 

    }

    
    proc process_data { ev args } {
	set name [lindex $args 0]
	set val [lindex $args 4]
	switch -glob $name {
	    ess/em_pos {
		set ::dh::status(eye_hor) [lindex $val 0]
		set ::dh::status(eye_ver) [lindex $val 1]
		update_eye_marker $::dh::status(eye_hor) $::dh::status(eye_ver)	
	    }
	    ess/state {
		set ::dh::status(state) [string totitle $val]
		set cmap { Running green Stopped red Inactive black }
		$::dh::widgets(status) configure -foreground \
		    [dict get $cmap $::dh::status(state)]
	    }
	    ess/obs_id {
		set ::dh::status(obs_id) [expr $val+1]
		set ::dh::status(obs_info) \
		    "$::dh::status(obs_id)/$::dh::status(obs_total)"
	    }
	    ess/obs_total {
		set ::dh::status(obs_total) $val
		set ::dh::status(obs_info) \
		    "$::dh::status(obs_id)/$::dh::status(obs_total)"
	    }
	    ess/obs_active {
		set ::dh::status(obs_active) $val
		set bg [$::dh::widgets(obstitle) cget -background]
		set colors "$bg red"
		$::dh::widgets(obslabel) configure \
		    -background [lindex $colors $::dh::status(obs_active)]
		
		if { $::dh::status(obs_active) == 0 } {
		    set ::dh::status(cur_stimtype) {}
		}
	    }
	    ess/joystick {
		switch $val {
		    0  { set ::dh::status(joystick_info) None }
		    8  { set ::dh::status(joystick_info) Up }
		    4  { set ::dh::status(joystick_info) Down }
		    1  { set ::dh::status(joystick_info) Left }
		    16 { set ::dh::status(joystick_info) Right }
		    2  { set ::dh::status(joystick_info) Press }
		}
	    }
	    ess/dio {
		set levels [lindex $val 1]
		set ::dh::status(dio_a) [expr {$levels>>16}]
		set ::dh::status(dio_b) [expr {$levels>>24}]
		update_dio_indicators
	    }

	    ess/subject { set ::dh::status(subject) $val }
	    ess/system { set ::dh::status(name) $val }
	    ess/datafile { set ::dh::status(datafile) $val }

	    ess/em_region_setting {
		update_em_region_setting {*}$val
	    }
	    ess/em_region_status {
		set ::dh::status(fixwin) [lindex $val 1]
		update_fixwin_indicators
	    }
	}
    }
    
    proc setup_view {} {
	wm title . "QPCS Viewer"

	labelframe .lfconn -text "Connection"
	set lf .lfconn
	label $lf.datahublabel -text Datahub: -anchor e -width 8
	ttk::combobox $lf.datahub -textvariable ::dh::datahub
	$lf.datahub configure -values [list 127.0.0.1]
	grid $lf.datahublabel $lf.datahub -padx 3
	label $lf.domainlabel -text Domain: -anchor e -width 8
	ttk::combobox $lf.domain -textvariable ::dh::domain
	$lf.domain configure -values [list ess/*]
	grid $lf.domainlabel $lf.domain -padx 3
	grid $lf -sticky new


	bind $lf.datahub <<ComboboxSelected>> { ::dh::reconnect }
	bind $lf.domain <<ComboboxSelected>>  { ::dh::reconnect }


	labelframe .lfess -text "ESS"
	set lf .lfess
	label $lf.statuslabel -text Status: -anchor e -width 8
	label $lf.statusvalue -textvariable ::dh::status(state) -anchor w -width 22
	grid $lf.statuslabel $lf.statusvalue -padx 3
	set ::dh::widgets(status) $lf.statusvalue

	label $lf.subjlabel -text Subject: -anchor e -width 8
	label $lf.subjvalue -textvariable ::dh::status(subject) -anchor w -width 22
	grid $lf.subjlabel $lf.subjvalue -padx 3 

	label $lf.syslabel -text System: -anchor e -width 8
	label $lf.sysvalue -textvariable ::dh::status(name) -anchor w -width 22
	grid $lf.syslabel $lf.sysvalue -padx 3 

	label $lf.filelabel -text Filename: -anchor e -width 8
	label $lf.filename -textvariable ::dh::status(datafile) \
	    -anchor w -width 22
	grid $lf.filelabel $lf.filename -padx 3
	bind $lf.filelabel <Double-1> {
	    clipboard clear; clipboard append $::dh::status(datafile)
	}

	grid $lf -sticky new

	labelframe .lfobs -text "Trial Info"
	set lf .lfobs
	set ::dh::widgets(obstitle) $lf

	set ::dh::widgets(obslabel) $lf.obslabel
	label $lf.obslabel -text Obs: -anchor e -width 8
	label $lf.obsvalue -textvariable ::dh::status(obs_info) -anchor w -width 12
	grid $lf.obslabel $lf.obsvalue -padx 3 

	label $lf.stimlabel -text StimID: -anchor e -width 8
	label $lf.stimname -textvariable ::dh::status(cur_stimtype) -anchor w -width 12
	grid $lf.stimlabel $lf.stimname -padx 3 

	grid $lf -sticky new

	set ::dh::status(disp_canvas_width) 200
	set ::dh::status(disp_canvas_height) 200
	set ::dh::widgets(disp_canvas) \
	    [canvas .dispc \
		 -width $::dh::status(disp_canvas_width) \
		 -height $::dh::status(disp_canvas_height) -background black]
	set ::dh::widgets(em_marker) \
	    [.dispc create oval 94 94 106 106 -outline white]

	foreach reg "0 1 2 3 4 5 6 7" {
	    set ::dh::widgets(em_reg_ellipse_$reg) \
		[.dispc create oval 90 90 110 110 -outline red -state hidden -tag regE$reg]
	    set ::dh::widgets(em_reg_rect_$reg) \
		[.dispc create rect 90 90 110 110 -outline red -state hidden -tag regR$reg]
	    set ::dh::widgets(em_reg_center_$reg) \
		[.dispc create oval 90 90 110 110 -outline red -state hidden -tag regC$reg]
	    
	}
	
	bind .dispc <Configure> { ::dh::resize_disp_canvas %w %h } 
	grid .dispc -sticky nsew


	set ::dh::status(fixwin_canvas_width) 200
	set ::dh::status(fixwin_canvas_height) 35

	set ::dh::widgets(fixwin_canvas) \
	    [canvas .fixc -width $::dh::status(fixwin_canvas_width) \
		 -height $::dh::status(fixwin_canvas_height) -background lightgray]

	add_fixwin_indicators

	bind .fixc <Configure> { ::dh::resize_fixwin_canvas %w %h } 
	grid .fixc -sticky nsew

	
	set ::dh::status(dio_canvas_width) 200
	set ::dh::status(dio_canvas_height) 35

	set ::dh::widgets(dio_canvas) \
	    [canvas .dioc -width $::dh::status(dio_canvas_width) \
		 -height $::dh::status(dio_canvas_height) -background lightgray]

	add_dio_indicators

	bind .dioc <Configure> { ::dh::resize_dio_canvas %w %h } 
	grid .dioc -sticky nsew

	frame .lfjoy
	set lf .lfjoy
	
	label $lf.joylabel -text Joystick: -anchor e -width 8
	label $lf.joyvalue -textvariable ::dh::status(joystick_info) \
	    -anchor w -width 12
	grid $lf.joylabel $lf.joyvalue -padx 3 
	grid $lf -sticky new
	
	grid columnconfigure . 0 -weight 1
	grid rowconfigure . 3 -weight 1
	
	bind . <Control-h> {console show}
    }
}

if { $argc > 0 } { set ::dh::datahub [lindex $argv 0] }
::dh::setup_view
::dh::connect_to_server $::dh::datahub $::dh::domain

after 300 ::dh::update_em_regions

