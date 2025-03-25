# NAME
#   hapticvis_stim.tcl
#
# DESCRIPTION
#   match_to_sample with visual and graspomatic generated shapes
#
# REQUIRES
#   polygon
#   metagroup
#
# AUTHOR
#   DLS
#

# find the shader dir
set stimdir [file dir [info nameofexecutable]]
set paths {
    /usr/local/stim2/shaders/
    /Applications/stim2.app/Contents/Resources/shaders/
}

foreach dir $paths {
    if { [file exists $dir] } { shaderSetPath $dir; break }
}

# for creating images using CImg and other image processing tools
package require impro

proc create_shape { shader id } {

    set scale [dl_get stimdg:shape_scale $id]
    set rotation [dl_get stimdg:shape_rot_deg_cw $id]
    set filled [dl_get stimdg:shape_filled $id]

    # target image sampler size should be set in stimdg
    set width 512; set width_2 [expr ${width}/2]
    set height 512; set height_2 [expr ${height}/2]
    set depth 4

    # shapes are stored in a coordinate system that doesn't fill a unit square
    #  this scale is designed to convert shapes to degrees visual angle 
    set shape_scale 2.5
    dl_local x [dl_mult stimdg:shape_coord_x:$id $shape_scale]
    dl_local y [dl_mult stimdg:shape_coord_y:$id $shape_scale]

    dl_local xscaled [dl_add [dl_mult $x $width] $width_2]
    dl_local yscaled [dl_add [dl_mult [dl_negate $y] $height] $height_2]
    set img [img_create -width $width -height $height -depth $depth]
    if { $filled } {
        set poly [img_fillPolygonOutside $img $xscaled $yscaled 255 255 255 255]
        img_invert $poly
    } else {
        set poly [img_drawPolyline $img $xscaled $yscaled 255 255 255 255]
    }
    dl_local pix [img_imgtolist $poly]
    img_delete $poly $img

    set sobj [shaderObj $shader]
    set img [shaderImageCreate $pix $width $height linear]
    shaderObjSetSampler $sobj [shaderImageID $img] 0

    scaleObj $sobj $scale

    # clockwise rotation
    rotateObj $sobj $rotation 0 0 -1

    return $sobj
}

proc create_circle { r g b { a 1 } } {
    set c [polygon]
    polycirc $c 1
    polycolor $c $r $g $b $a
    return $c
}

proc create_noise { id } {
    set noise_mg [metagroup]
    for { set i 0 } { $i < [dl_length stimdg:noise_elements:$id] } { incr i } {
        lassign [dl_tcllist [dl_get stimdg:noise_elements:$id $i]] x y r
        set c [create_circle 0 0 0]
        translateObj $c $x $y
        scaleObj $c $r
        metagroupAdd $noise_mg $c
    }
    return $noise_mg
}

proc nexttrial { id } {
    resetObjList         ;# unload existing objects
    shaderImageReset;        ;# free any shader textures
    shaderDeleteAll;         ;# reset any shader objects
    glistInit 2

    set shader_file image    ;# shader file is image.glsl
    set shader [shaderBuild $shader_file]

    set ::current_trial $id
    set trialtype [dl_get stimdg:trial_type $id]
    set scale [dl_get stimdg:choice_scale $id]
    set nchoices [dl_get stimdg:n_choices $id]

    # add the visual sample for VV trials, no visual sample for HV trials
    if { $trialtype == "visual" } {
        set ::sample [metagroup]
        set shape [create_shape $shader $id]
        metagroupAdd $::sample $shape

        # add noise to metagroup if specified
        if { [dl_length stimdg:noise_elements:$id] } {
            set noise [create_noise $id]
            metagroupAdd $::sample $noise
        }

        glistAddObject $::sample 0
        setVisible $::sample 0
    } else {

    }

    # choice circles
    set mg [metagroup]

    for { set i 0 } { $i < $nchoices } { incr i } {
        set s [create_circle 1 1 1 0.3]
        translateObj $s {*}[dl_tcllist stimdg:choice_centers:$id:$i]
        scaleObj $s [dl_get stimdg:choice_scale $id]
        metagroupAdd $mg $s
    }
    glistAddObject $mg 0
    setVisible $mg 0
    set ::choice_array $mg

    # gray selecting circle
    set s [create_circle .9 .9 .9 0.9]
    scaleObj $s [expr {0.8*[dl_get stimdg:choice_scale $id]}]
    setVisible $s 0
    glistAddObject $s 0
    set ::feedback(selecting) $s

    # green correct circle
    set s [create_circle .1 .8 0 0.9]
    scaleObj $s [expr {0.8*[dl_get stimdg:choice_scale $id]}]
    setVisible $s 0
    glistAddObject $s 0
    set ::feedback(correct) $s

    # red incorrect circle
    set s [create_circle .9 .1 .1 0.9]
    scaleObj $s [expr {0.8*[dl_get stimdg:choice_scale $id]}]
    setVisible $s 0
    glistAddObject $s 0
    set ::feedback(incorrect) $s
}

proc highlight_response { p } {
    set id $::current_trial
    set n_choices [dl_get stimdg:n_choices $id]
    if { $n_choices == 4 } {
	# ur=9(1) ul=5(2) dl=6(3) dr=10(4)
	set mapdict { 0 -1 9 0 5 1 6 2 10 3 }
    } elseif { $n_choices == 6 } {
	# u=1(1)  d=2(4) u-l=5(2) u-r=9(0) d-l=6(3) d-r=10(5)
	set mapdict { 0 -1 1 1 2 4 5 2 9 0 6 3 10 5}
    } else {
	# up=1(2)   down=2(6)  left=4(4)   right=8(0)
	# up-left=5(3) up-right=9(1) d-l=6(5) d-r=10(7)
	set mapdict { 0 -1 1 2 2 6 4 4 8 0 5 3 9 1 6 5 10 7 }
    }
    # if this is not an allowable position return
    if { ![dict exists $mapdict $p] } {
	return -1
    }
    
    # map joy_position to slot
    set slot [dict get $mapdict $p]
    set s $::feedback(selecting)

    if { $slot == -1 } { 
	setVisible $s 0
	redraw
    } else {
	translateObj $s {*}[dl_tcllist stimdg:choice_centers:$id:$slot]
	setVisible $s 1
	redraw
    }
}

proc feedback_on { type x y } {
    translateObj $::feedback($type) $x $y
    setVisible $::feedback($type) 1
    redraw
}

proc feedback_off { type } {
    if { $type == "all" } {
        foreach t [array names ::feedback] {
            setVisible $::feedback($t) 0
        }
    } else {
        setVisible $::feedback($type) 0
    }
    redraw
}

proc stim_on {} {
    glistSetCurGroup 0
    glistSetVisible 1
    redraw
}

proc stim_off {} {
    glistSetVisible 0
    redraw
}

proc sample_on {} {
    setVisible $::sample 1
    redraw
}

proc sample_off {} {
    setVisible $::sample 0
    redraw
}

proc choices_on {} {
    setVisible $::choice_array 1
    redraw
}

proc choices_off {} {
    setVisible $::choice_array 0
    redraw
}

proc reset { } {
    glistSetVisible 0; redraw;
}

proc clearscreen { } {
    glistSetVisible 0; redraw;
}


