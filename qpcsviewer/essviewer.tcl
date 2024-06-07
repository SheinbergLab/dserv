#
# Procs for interacting with a datahub
#

package require Tk
package require qpcs

namespace eval dh {
    set connected 0

    set datahub 100.0.0.40
    set domain  qpcs

    set status(state) Stopped
    set status(eye_hor) 0
    set status(eye_ver) 0
    set status(obs_id) 0
    set status(obs_total) 0
    set status(obs_info) 0/0
    set status(obs_active) 0
    set status(dio_a) 0
    set status(dio_b) 0

    proc process_data { ev args } {
	set name [lindex $args 0]
	set val [lindex $args 4]
	switch -glob $name {
	    qpcs:em_pos {
		set dh::status(eye_hor) [lindex $args 6]
		set dh::status(eye_ver) [lindex $args 7]
		update_eye_marker $dh::status(eye_hor) $dh::status(eye_ver)
	    }
	    qpcs:state {
		set dh::status(state) [string totitle $val]
		set cmap { Running green Stopped red Inactive black }
		$dh::widgets(status) configure -foreground \
		    [dict get $cmap $::dh::status(state)]
	    }
	    qpcs:obs_id {
		set dh::status(obs_id) $val
		set dh::status(obs_info) \
		    "$dh::status(obs_id)/$dh::status(obs_total)"
	    }
	    qpcs:obs_total {
		set dh::status(obs_total) $val
		set dh::status(obs_info) \
		    "$dh::status(obs_id)/$dh::status(obs_total)"
	    }
	    qpcs:obs_active {
		set dh::status(obs_active) $val
		set bg [$dh::widgets(obstitle) cget -background]
		set colors "$bg red"
		$dh::widgets(obslabel) configure \
		    -background [lindex $colors $dh::status(obs_active)]
		
		if { $dh::status(obs_active) == 0 } {
		    set dh::status(cur_stimtype) {}
		}
	    }
	    qpcs:dio { 
		set dh::status(dio_a) [expr [lindex $args 4]]
		set dh::status(dio_b) [expr [lindex $args 5]]
		update_dio_indicators
	    }
	    qpcs:subject { set dh::status(subject) $val }
	    qpcs:name { set dh::status(name) $val }
	    qpcs:datafile { set dh::status(datafile) $val }
	}
    }


    proc initialize_vars { server } {
	set val [qpcs::dsGet $server qpcs:state]
	set dh::status(state) [string totitle $val]
	set cmap { Running green Stopped red Inactive black }
	$dh::widgets(status) configure -foreground \
	    [dict get $cmap $::dh::status(state)]

	set val [qpcs::dsGet $server qpcs:obs_id]
	set dh::status(obs_id) $val

	set val [qpcs::dsGet $server qpcs:obs_total]
	set dh::status(obs_total) $val
	set dh::status(obs_info) "$dh::status(obs_id)/$dh::status(obs_total)"

	set val [qpcs::dsGet $server qpcs:obs_active]
	set dh::status(obs_active) $val
	set bg [$dh::widgets(obstitle) cget -background]
	set colors "$bg red"
	$dh::widgets(obslabel) configure \
	    -background [lindex $colors $dh::status(obs_active)]
		
	if { $dh::status(obs_active) == 0 } {
	    set dh::status(cur_stimtype) {}
	}

	set val [qpcs::dsGet $server qpcs:dio]
	set dh::status(dio_a) [expr [lindex $val 0]]
	set dh::status(dio_b) [expr [lindex $val 1]]
	update_dio_indicators

	set dh::status(subject) [qpcs::dsGet $server qpcs:subject]
	set dh::status(name) [qpcs::dsGet $server qpcs:name]
	set dh::status(datafile) [qpcs::dsGet $server qpcs:datafile]
    }

    proc connect_to_server { server ess { port 4502 } } {
	initialize_vars $server
	if { [qpcs::dsRegister $server] != 1 } {
	    error "Unable to register with $server"
	}
	qpcs::addCallback dh::process_data ds
	qpcs::dsAddMatch $server qpcs:*
	set dh::connected 1
    }

    proc disconnect_from_server {} {
	qpcs::dsUnregister 
    }

    proc reconnect {} {
	disconnect_from_server
	connect_to_server $dh::datahub $dh::domain
    }

    proc resize_disp_canvas { w h } {
	set dh::status(disp_canvas_width) $w 
	set dh::status(disp_canvas_height) $h
	dh::update_eye_marker $dh::status(eye_hor) $dh::status(eye_ver)	
    }

    proc add_dio_indicators {} {
	for { set i 0 } { $i < 8 } { incr i } {
	    $dh::widgets(dio_canvas) create oval 0 0 1 1 -fill red -tag A$i
	}

	for { set i 0 } { $i < 8 } { incr i } {
	    $dh::widgets(dio_canvas) create oval 0 0 1 1 -fill red -tag B$i
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
	    $dh::widgets(dio_canvas) coords A[expr 7-$i] \
		[expr $x-$msize] [expr $hheight-$msize] \
		[expr $x+$msize] [expr $hheight+$msize]
	    set x [expr $x+$inc]
	}

	set x [expr $hwidth+8]
	for { set i 0 } { $i < 8 } { incr i } {
	    $dh::widgets(dio_canvas) coords B[expr 7-$i] \
		[expr $x-$msize] [expr $hheight-$msize] \
		[expr $x+$msize] [expr $hheight+$msize]
	    set x [expr $x+$inc]
	}
			     
    }

    proc update_dio_indicators {} {
	for { set i 0 } { $i < 8 } { incr i } {
	    if { [expr $dh::status(dio_a)&(1<<$i)] != 0 } {
		$dh::widgets(dio_canvas) itemconfigure A$i -fill green
	    } else {
		$dh::widgets(dio_canvas) itemconfigure A$i -fill red
	    }
	}
	for { set i 0 } { $i < 8 } { incr i } {
	    if { [expr $dh::status(dio_b)&(1<<$i)] != 0 } {
		$dh::widgets(dio_canvas) itemconfigure B$i -fill green
	    } else {
		$dh::widgets(dio_canvas) itemconfigure B$i -fill red
	    }
	}
    }

    proc resize_dio_canvas { w h } {
	set dh::status(dio_canvas_width) $w 
	set dh::status(dio_canvas_height) $h
	position_dio_indicators
    }

    proc update_eye_marker { x y } {
	set marker $dh::widgets(em_marker)
	set marker $dh::widgets(disp_canvas)
	set range 20.0
	set hrange 10.0
	set msize 5
	set w $dh::status(disp_canvas_width)
	set h $dh::status(disp_canvas_height)
	set hw [expr $w/2]
	set hh [expr $h/2]
	set x0 [expr (($x/$hrange)*$hw)+$hw]
	set y0 [expr $hh-(($y/$hrange)*$hh)]
	$dh::widgets(disp_canvas) \
	    coords $dh::widgets(em_marker) [expr $x0-$msize] [expr $y0-$msize] \
	    [expr $x0+$msize] [expr $y0+$msize] 

    }

    proc setup_view {} {
	wm title . "ESS Viewer"

	labelframe .lfconn -text "Connection"
	set lf .lfconn
	label $lf.datahublabel -text Datahub: -anchor e -width 8
	ttk::combobox $lf.datahub -textvariable dh::datahub
	$lf.datahub configure -values [list beast.neuro.brown.edu qnx1.neuro.brown.edu qnx2.neuro.brown.edu qnx3.neuro.brown.edu 192.168.0.150]
	grid $lf.datahublabel $lf.datahub -padx 3
	label $lf.domainlabel -text Domain: -anchor e -width 8
	ttk::combobox $lf.domain -textvariable dh::domain
	$lf.domain configure -values [list qnx1 qnx2 qnx3 beast ess]
	grid $lf.domainlabel $lf.domain -padx 3
	grid $lf -sticky new


	bind $lf.datahub <<ComboboxSelected>> { dh::reconnect }
	bind $lf.domain <<ComboboxSelected>>  { dh::reconnect }


	labelframe .lfess -text "ESS"
	set lf .lfess
	label $lf.statuslabel -text Status: -anchor e -width 8
	label $lf.statusvalue -textvariable dh::status(state) -anchor w -width 22
	grid $lf.statuslabel $lf.statusvalue -padx 3
	set ::dh::widgets(status) $lf.statusvalue

	label $lf.subjlabel -text Subject: -anchor e -width 8
	label $lf.subjvalue -textvariable dh::status(subject) -anchor w -width 22
	grid $lf.subjlabel $lf.subjvalue -padx 3 

	label $lf.syslabel -text System: -anchor e -width 8
	label $lf.sysvalue -textvariable dh::status(name) -anchor w -width 22
	grid $lf.syslabel $lf.sysvalue -padx 3 

	label $lf.filelabel -text Filename: -anchor e -width 8
	label $lf.filename -textvariable dh::status(datafile) \
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
	label $lf.obsvalue -textvariable dh::status(obs_info) -anchor w -width 12
	grid $lf.obslabel $lf.obsvalue -padx 3 

	label $lf.stimlabel -text StimID: -anchor e -width 8
	label $lf.stimname -textvariable dh::status(cur_stimtype) -anchor w -width 12
	grid $lf.stimlabel $lf.stimname -padx 3 

	grid $lf -sticky new

	set dh::status(disp_canvas_width) 200
	set dh::status(disp_canvas_height) 200
	set dh::widgets(disp_canvas) \
	    [canvas .dispc \
		 -width $::dh::status(disp_canvas_width) \
		 -height $dh::status(disp_canvas_height) -background black]
	set dh::widgets(em_marker) \
	    [.dispc create oval 94 94 106 106 -outline white]

	bind .dispc <Configure> { dh::resize_disp_canvas %w %h } 
	grid .dispc -sticky nsew

	set dh::status(dio_canvas_width) 200
	set dh::status(dio_canvas_height) 35
	set dh::widgets(dio_canvas) \
	    [canvas .dioc -width $dh::status(dio_canvas_width) \
		 -height $dh::status(dio_canvas_height) -background lightgray]
	add_dio_indicators

	bind .dioc <Configure> { dh::resize_dio_canvas %w %h } 
	grid .dioc -sticky nsew

	grid columnconfigure . 0 -weight 1
	grid rowconfigure . 3 -weight 1
	
	bind . <Control-h> {console show}
    }
}

if { $argc > 0 } { set ::dh::datahub [lindex $argv 0] }
dh::setup_view
dh::connect_to_server $dh::datahub $dh::domain


