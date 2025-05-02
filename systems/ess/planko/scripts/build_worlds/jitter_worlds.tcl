#
# jitter_worlds.tcl
#

# Load the Thread package
package require Thread

set dlshlib [file join /usr/local/dlsh dlsh.zip]
set base [file join [zipfs root] dlsh]
if { ![file exists $base] && [file exists $dlshlib] } {
    zipfs mount $dlshlib $base
}
set ::auto_path [linsert $::auto_path [set auto_path 0] $base/lib]
tcl::tm::path add .

package require dlsh
package require box2d
package require planko
package require dtw;		# for dynamic time warping
package require gsl;		# for regression
package require dbscan;         # for clustering

proc get_world { dg id } {
    set w [dg_create]
    foreach l [dg_tclListnames $dg] {
	dl_set $w:$l $dg:$l:$id
    }
    return $w
}

proc run_simulation { world ball { simtime 6 } { frame_rate 59.9 } } {
    set step [expr ${frame_rate}/1000.]
    box2d::setBodyType $world $ball 2    

    dl_local contact_t [dl_flist]
    dl_local contact_bodies [dl_slist]
    dl_local ts [dl_flist]
    dl_local xs [dl_flist]
    dl_local ys [dl_flist]
    
    for { set t 0 } { $t < $simtime } { set t [expr $t+$step] } {
	box2d::step $world $step

	if { [set c [box2d::getContactBeginEventCount $world]] } {
	    set events [box2d::getContactBeginEvents $world]
	    for { set i 0 } { $i < $c } { incr i } { 
		dl_append $contact_t $t
		dl_append $contact_bodies [lindex $events $i]
	    }
	}
	lassign [box2d::getBodyInfo $world $ball] tx ty a
	dl_append $ts $t
	dl_append $xs $tx
	dl_append $ys $ty
    }

    set g [dg_create]
    dl_local out [dl_lt $ys [expr {-7.5+0.01}]]
    if { [dl_max $out] == 0 } {
	dl_local valid [dl_fromto 0 [dl_length $out]]
    } else {
	dl_local valid [dl_fromto 0 [dl_min [dl_indices $out]]]
    }
    dl_set $g:t [dl_llist [dl_choose $ts $valid]]
    dl_set $g:x [dl_llist [dl_choose $xs $valid]]
    dl_set $g:y [dl_llist [dl_choose $ys $valid]]
    dl_set $g:contact_bodies [dl_llist $contact_bodies]
    dl_set $g:contact_t [dl_llist $contact_t]
    return  $g
}

proc do_jitter { source dest } {
    set translate_scale 0.5
    set rotate_scale [expr {$::pi/180.*10}]
    dl_local is_plank [dl_regmatch $source:name plank*]
    set n [dl_length $is_plank]
    dl_local tx [dl_add $source:tx [dl_mult [dl_sub [dl_urand $n] 0.5] \
					$translate_scale $is_plank]]
    dl_local ty [dl_add $source:ty [dl_mult [dl_sub [dl_urand $n] 0.5] \
					$translate_scale $is_plank]]
    dl_local a [dl_add $source:angle [dl_mult [dl_sub [dl_urand $n] 0.5] \
					  $rotate_scale $is_plank]]
    dl_set $dest:tx $tx
    dl_set $dest:ty $ty
    dl_set $dest:angle $a
    return $dest
}

proc jitter_world { dg id n } {
    set w [get_world $dg $id]
    set jittered_world [dg_copy $w]

    for { set i 0 } { $i < $n } { incr i } {
	box2d::destroy all
	set jittered_world [do_jitter $w $jittered_world]
	lassign [planko::build_world $jittered_world] world ball
	set s [run_simulation $world $ball]

	dl_set $s:jx    [dl_llist $jittered_world:tx]
	dl_set $s:jy    [dl_llist $jittered_world:ty]
	dl_set $s:angle [dl_llist $jittered_world:angle]
	
	if { !$i } {
	    set sim_dg $s
	} else {
	    dg_append $sim_dg $s
	    dg_delete $s
	}
    }

    dg_delete $jittered_world
    dg_delete $w
    return $sim_dg
}

proc world_jitter { dg njitter from to { skip 1 } } {
    set out [dg_create]
    foreach c "t x y jx jy angle distance distance_matrix" {
	dl_set $out:$c [dl_llist]
    }
    dl_set $out:nhit [dl_ilist]
    
    for { set i $from } { $i < $to } { incr i } {
	set j [jitter_world $dg $i $njitter]
	foreach jc "t x y jx jy angle" {
	    dl_append $out:$jc $j:$jc
	}
	dl_local actual_traj [dl_llist $dg:ball_x:$i $dg:ball_y:$i]
	dl_local jittered_trajs [dl_transpose [dl_llist $j:x $j:y]]
	dl_append $out:distance [dtwDistanceOnly $actual_traj $jittered_trajs]

	# add complete distance matrix between trajs
	dl_local sel [dl_llist [dl_repeat "1 0" "1 $skip"]]
	dl_local x [dl_select $j:x $sel]
	dl_local y [dl_select $j:y $sel]
	dl_local t [dl_transpose [dl_llist $x $y]]
	dl_append $out:distance_matrix [dtwDistanceOnly $t]
	
	dl_append $out:nhit [dl_get $dg:nhit $i]
    }
    return $out
}

proc jitter_regression_table { j id { nplanks 10 } } {
    set r [dg_create]

    # concatenate the jitter xs, ys, angles for each trial into list
    dl_local xs \
	[dl_transpose \
	     [dl_combine \
		  [dl_transpose $j:jx:$id] \
		  [dl_transpose $j:jy:$id] \
		  [dl_transpose $j:angle:$id]]]

    set n [dl_length $j:jx:$id:0]
    dl_local p_ids \
	[dl_transpose \
	     [dl_combine \
		  [dl_llist [dl_fromto 0 $n]] \
		  [dl_llist [dl_fromto $n [expr $n*2]]] \
		  [dl_llist [dl_fromto [expr $n*2] [expr $n*3]]]]]
    dl_local p_ids [dl_choose [dl_collapse $p_ids] [dl_fromto 0 [expr $nplanks*3]]]
    
    
    dl_set $r:xs [dl_choose $xs [dl_llist $p_ids]]
    dl_set $r:ys [dl_sqrt $j:distance:$id]
    return $r
}

