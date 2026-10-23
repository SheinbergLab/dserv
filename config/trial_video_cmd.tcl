# trial video commands for the df subprocess.
# Videos live in $export_destination/video/<basename>/trial_<N>.mp4.
# A symlink $dspath/www/trial_video points at that directory so the
# trial viewer page can play them.

proc trial_video_export_root {} {
    variable export_destination
    if {![info exists export_destination] || $export_destination eq ""} {
        return ""
    }
    return [file join $export_destination video]
}

proc trial_video_basename {filename} {
    return [file rootname [file tail $filename]]
}

proc trial_video_load {} {
    if {[info exists ::trial_video::tape_ready]} {
        return
    }
    set candidates [list /home/lab/systems/dserv/lib/trial_video-1.0.tm]
    global dspath
    if {[info exists dspath] && $dspath ne ""} {
        lappend candidates [file join $dspath lib trial_video-1.0.tm]
    }
    foreach path $candidates {
        if {[file exists $path]} {
            source $path
            return
        }
    }
    error "trial_video package not found"
}

# Return "" on success, or a JSON error string.
proc trial_video_ensure_link {video_root} {
    global dspath
    if {![info exists dspath] || $dspath eq ""} {
        return [_json_error "dspath is not set"]
    }
    file mkdir $video_root
    set link [file join $dspath www trial_video]
    set target [file normalize $video_root]
    if {![catch {file lstat $link st}]} {
        if {$st(type) ne "link"} {
            return [_json_error "$link exists and is not a symlink"]
        }
        set raw [file readlink $link]
        if {[file pathtype $raw] eq "absolute"} {
            set cur [file normalize $raw]
        } else {
            set cur [file normalize [file join [file dirname $link] $raw]]
        }
        if {$cur ne $target} {
            file delete $link
            file link -symbolic $link $target
        }
    } else {
        file link -symbolic $link $target
    }
    return ""
}

proc trial_video_list {filename} {
    set root [trial_video_export_root]
    if {$root eq ""} {
        return [_json_error "Export is not configured"]
    }
    set link_err [trial_video_ensure_link $root]
    if {$link_err ne ""} {
        return $link_err
    }
    set base [trial_video_basename $filename]
    set dir [file join $root $base]

    set json [yajl create #auto]
    $json map_open
    $json string status string ok
    $json string basename string $base
    $json string videos array_open
    if {[file isdirectory $dir]} {
        foreach f [lsort -dictionary [glob -nocomplain -directory $dir trial_*.mp4]] {
            set name [file tail $f]
            if {![regexp {^trial_(\d+)\.mp4$} $name -> n]} continue
            $json map_open
            $json string name string $name
            $json string trial number $n
            $json string url string "trial_video/$base/$name"
            $json map_close
        }
    }
    $json array_close
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# Saved-trial numbers for one block, from a trials table that already
# exists. No re-extract. saved is null when there is no table, or the
# table has no obsid column. Otherwise each attempt maps to the inclusive
# span of extracted trial ids that came from it.
proc trial_video_trials_path {filename} {
    variable export_destination
    set trials_path [dfdb eval {
        SELECT trials_path FROM datafiles WHERE filename = $filename LIMIT 1
    }]
    if {$trials_path ne "" && [file exists $trials_path]} {
        return $trials_path
    }
    if {[info exists export_destination] && $export_destination ne ""} {
        set base [trial_video_basename $filename]
        set exported [file join $export_destination trials ${base}.trials.dgz]
        if {[file exists $exported]} {
            return $exported
        }
    }
    return ""
}

proc trial_video_saved {filename} {
    set path [trial_video_trials_path $filename]
    if {$path eq ""} {
        return [_json_saved ""]
    }
    set g ""
    if {[catch {
        set g [dg_read $path]
        set names [dg_tclListnames $g]
        if {[lsearch -exact $names obsid] < 0} {
            set obsids {}
            set ids {}
        } else {
            set obsids [dl_tcllist $g:obsid]
            if {[lsearch -exact $names trialid] >= 0} {
                set ids [dl_tcllist $g:trialid]
            } else {
                set ids {}
                set i 0
                foreach _ $obsids {
                    lappend ids $i
                    incr i
                }
            }
        }
    } err]} {
        catch {if {$g ne ""} { dg_delete $g }}
        return [_json_error $err]
    }
    catch {if {$g ne ""} { dg_delete $g }}
    if {[llength $obsids] == 0} {
        return [_json_saved ""]
    }

    array set span {}
    foreach obs $obsids id $ids {
        if {![string is integer -strict $obs] || ![string is integer -strict $id]} {
            continue
        }
        if {![info exists span($obs)]} {
            set span($obs) [list $id $id]
        } else {
            lassign $span($obs) lo hi
            if {$id < $lo} { set lo $id }
            if {$id > $hi} { set hi $id }
            set span($obs) [list $lo $hi]
        }
    }
    if {[array size span] == 0} {
        return [_json_saved ""]
    }
    set parts {}
    foreach obs [lsort -integer [array names span]] {
        lassign $span($obs) lo hi
        lappend parts "\"$obs\":\[$lo,$hi\]"
    }
    return [_json_saved [join $parts ,]]
}

# spans is a JSON object body, or "" for null.
proc _json_saved {spans} {
    if {$spans eq ""} {
        set saved null
    } else {
        set saved "{$spans}"
    }
    return "{\"status\":\"ok\",\"saved\":$saved}"
}

# Filenames whose catalog row includes a stim/inbox tape. The viewer
# treats every other block as if it were not there.
proc taped_datafiles {} {
    set json [yajl create #auto]
    $json array_open
    dfdb eval {
        SELECT DISTINCT d.filename AS filename
        FROM datafiles d
        JOIN file_columns c ON c.datafile_id = d.id
        WHERE c.column_name = '<ds>stim/inbox'
    } {
        $json string $filename
    }
    $json array_close
    set result [$json get]
    $json delete
    return $result
}

proc trial_video_make {filename trial} {
    set root [trial_video_export_root]
    if {$root eq ""} {
        return [_json_error "Export is not configured"]
    }
    if {![string is integer -strict $trial] || $trial < 0} {
        return [_json_error "trial must be a non-negative integer"]
    }
    if {![catch {dservGet ess/status} ess_status]} {
        set ess_status [string trim $ess_status]
        if {$ess_status eq "running"} {
            return [_json_error "A task is running. Stop it before creating a video."]
        }
        if {$ess_status eq "loading"} {
            return [_json_error "The system is still loading."]
        }
    }
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        return [_json_error "No datafile $filename"]
    }
    set link_err [trial_video_ensure_link $root]
    if {$link_err ne ""} {
        return $link_err
    }
    if {[catch {trial_video_load} load_err]} {
        return [_json_error $load_err]
    }

    set base [trial_video_basename $filename]
    set dir [file join $root $base]
    file mkdir $dir
    set out [file join $dir trial_${trial}.mp4]

    if {[catch {set info [::trial_video::render $filepath $trial $out]} render_err]} {
        catch {file delete -force $out}
        return [_json_error $render_err]
    }

    set ok [dict create \
        path $out \
        url "trial_video/$base/trial_${trial}.mp4" \
        trial $trial \
        frames [dict get $info frames]]
    foreach key {clock_held reset_frames background_missing} {
        if {[dict exists $info $key]} {
            dict set ok $key [dict get $info $key]
        }
    }
    return [_json_ok $ok]
}

# Source the package again so a long-lived df sees new procs.
proc trial_video_reload {} {
    set candidates [list /home/lab/systems/dserv/lib/trial_video-1.0.tm]
    global dspath
    if {[info exists dspath] && $dspath ne ""} {
        lappend candidates [file join $dspath lib trial_video-1.0.tm]
    }
    foreach path $candidates {
        if {[file exists $path]} {
            source $path
            return
        }
    }
    error "trial_video package not found"
}

# Write the replay files and return their URLs. Does not use stim2.
proc trial_video_replay {filename trial} {
    set root [trial_video_export_root]
    if {$root eq ""} {
        return [_json_error "Export is not configured"]
    }
    if {![string is integer -strict $trial] || $trial < 0} {
        return [_json_error "trial must be a non-negative integer"]
    }
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        return [_json_error "No datafile $filename"]
    }
    set link_err [trial_video_ensure_link $root]
    if {$link_err ne ""} {
        return $link_err
    }
    if {[catch {trial_video_reload} load_err]} {
        return [_json_error $load_err]
    }

    set base [trial_video_basename $filename]
    set dir [file join $root $base _replay]
    file delete -force $dir
    file mkdir $dir
    if {[catch {set info [::trial_video::replay_bundle $filepath $trial $dir]} err]} {
        catch {file delete -force $dir}
        return [_json_error $err]
    }
    set ok [dict create \
                replay "trial_video/$base/_replay/replay.json" \
                stim "trial_video/$base/_replay/stim.tcl" \
                stimdg "trial_video/$base/_replay/stimdg.bin" \
                frames [dict get $info frames] \
                background_missing [dict get $info background_missing] \
                width [dict get $info width] \
                height [dict get $info height]]
    return [_json_ok $ok]
}

proc trial_video_save_path {filename trial} {
    set root [trial_video_export_root]
    if {$root eq ""} {
        return -code error "Export is not configured"
    }
    if {![string is integer -strict $trial] || $trial < 0} {
        return -code error "trial must be a non-negative integer"
    }
    set base [trial_video_basename $filename]
    set dir [file join $root $base]
    file mkdir $dir
    return [file join $dir trial_${trial}.mp4]
}

# Start a chunked write of trial_N.mp4. A previous bake of this trial,
# including layered copies, is removed.
proc trial_video_save_begin {filename trial} {
    if {[catch {set path [trial_video_save_path $filename trial]} err]} {
        return [_json_error $err]
    }
    if {[catch {trial_video_reload} load_err]} {
        return [_json_error $load_err]
    }
    set link_err [trial_video_ensure_link [trial_video_export_root]]
    if {$link_err ne ""} {
        return $link_err
    }
    ::trial_video::forget_layered $path
    catch {file delete -force $path}
    if {[info exists ::trial_video_save_fd] && $::trial_video_save_fd ne ""} {
        catch {close $::trial_video_save_fd}
    }
    set ::trial_video_save_fd [open $path wb]
    fconfigure $::trial_video_save_fd -translation binary
    set ::trial_video_save_path $path
    return [_json_ok [dict create path $path]]
}

proc trial_video_save_chunk {filename trial data} {
    if {![info exists ::trial_video_save_fd] || $::trial_video_save_fd eq ""} {
        return [_json_error "no save in progress"]
    }
    if {[catch {puts -nonewline $::trial_video_save_fd [binary decode base64 $data]} err]} {
        return [_json_error $err]
    }
    return [_json_ok [dict create]]
}

proc trial_video_save_cancel {filename trial} {
    if {[info exists ::trial_video_save_fd] && $::trial_video_save_fd ne ""} {
        catch {close $::trial_video_save_fd}
    }
    set ::trial_video_save_fd ""
    if {[info exists ::trial_video_save_path] && $::trial_video_save_path ne ""} {
        catch {file delete -force $::trial_video_save_path}
    }
    set ::trial_video_save_path ""
    return [_json_ok [dict create]]
}

# Browsers on plain HTTP have no VideoEncoder. They upload one JPEG per
# picture and ffmpeg writes the same mp4 the WebCodecs path would.
proc trial_video_jpeg_begin {filename trial w h} {
    if {[catch {set path [trial_video_save_path $filename $trial]} err]} {
        return [_json_error $err]
    }
    if {![string is integer -strict $w] || $w < 1 ||
        ![string is integer -strict $h] || $h < 1} {
        return [_json_error "bad frame size"]
    }
    set dir [file join [file dirname $path] _jpeg]
    file delete -force $dir
    file mkdir $dir
    if {[info exists ::tv_jpeg_fd] && $::tv_jpeg_fd ne ""} {
        catch {close $::tv_jpeg_fd}
    }
    set ::tv_jpeg_dir $dir
    set ::tv_jpeg_out $path
    set ::tv_jpeg_index 0
    set ::tv_jpeg_fd ""
    return [_json_ok [dict create path $path]]
}

proc trial_video_jpeg_frame {filename trial nrep data} {
    if {![info exists ::tv_jpeg_dir] || $::tv_jpeg_dir eq ""} {
        return [_json_error "no jpeg encode in progress"]
    }
    if {![string is integer -strict $nrep] || $nrep < 1} {
        return [_json_error "nrep must be a positive integer"]
    }
    if {[catch {set bytes [binary decode base64 $data]} err]} {
        return [_json_error $err]
    }
    set first $::tv_jpeg_index
    set path [file join $::tv_jpeg_dir [format frame_%06d.jpg $first]]
    set fd [open $path wb]
    fconfigure $fd -translation binary
    puts -nonewline $fd $bytes
    close $fd
    for {set k 1} {$k < $nrep} {incr k} {
        file copy -force $path [file join $::tv_jpeg_dir \
            [format frame_%06d.jpg [expr {$first + $k}]]]
    }
    incr ::tv_jpeg_index $nrep
    return [_json_ok [dict create n $::tv_jpeg_index]]
}

proc trial_video_jpeg_cancel {filename trial} {
    if {[info exists ::tv_jpeg_fd] && $::tv_jpeg_fd ne ""} {
        catch {close $::tv_jpeg_fd}
    }
    set ::tv_jpeg_fd ""
    if {[info exists ::tv_jpeg_dir] && $::tv_jpeg_dir ne ""} {
        catch {file delete -force $::tv_jpeg_dir}
    }
    set ::tv_jpeg_dir ""
    if {[info exists ::tv_jpeg_out] && $::tv_jpeg_out ne ""} {
        catch {file delete -force $::tv_jpeg_out}
    }
    set ::tv_jpeg_out ""
    set ::tv_jpeg_index 0
    return [_json_ok [dict create]]
}

proc trial_video_jpeg_finish {filename trial frames background_missing clock_held reset_frames} {
    if {![string is integer -strict $frames] || $frames < 1} {
        return [_json_error "frames must be a positive integer"]
    }
    foreach n [list $background_missing $clock_held $reset_frames] {
        if {![string is integer -strict $n] || $n < 0} {
            return [_json_error "save counters must be non-negative integers"]
        }
    }
    if {![info exists ::tv_jpeg_dir] || $::tv_jpeg_dir eq "" ||
        ![info exists ::tv_jpeg_out] || $::tv_jpeg_out eq ""} {
        return [_json_error "no jpeg encode in progress"]
    }
    if {[catch {trial_video_reload} load_err]} {
        return [_json_error $load_err]
    }
    set dir $::tv_jpeg_dir
    set out $::tv_jpeg_out
    ::trial_video::forget_layered $out
    catch {file delete -force $out}
    set log [file join $dir ffmpeg.log]
    set pattern [file join $dir frame_%06d.jpg]
    if {[catch {
        exec ffmpeg -y -loglevel error -framerate 60 -start_number 0 -i $pattern \
            -c:v libx264 -pix_fmt yuv420p \
            -bf 0 -g 60 -keyint_min 60 -sc_threshold 0 \
            -use_editlist 0 -movie_timescale 15360 \
            -movflags +faststart $out 2> $log
    } err]} {
        set detail $err
        if {[file exists $log]} {
            catch {
                set fd [open $log r]
                set detail "$err\n[read $fd]"
                close $fd
            }
        }
        catch {file delete -force $out}
        return [_json_error $detail]
    }
    file delete -force $dir
    set ::tv_jpeg_dir ""
    set ::tv_jpeg_out ""
    set ::tv_jpeg_index 0
    set base [trial_video_basename $filename]
    set ok [dict create \
                path $out \
                url "trial_video/$base/trial_${trial}.mp4" \
                trial $trial \
                frames $frames \
                clock_held $clock_held \
                reset_frames $reset_frames \
                background_missing $background_missing]
    return [_json_ok $ok]
}

# Close the mp4 and return the same object trial_video_make returns.
proc trial_video_save_finish {filename trial frames background_missing clock_held reset_frames} {
    if {![string is integer -strict $frames] || $frames < 1} {
        return [_json_error "frames must be a positive integer"]
    }
    foreach n [list $background_missing $clock_held $reset_frames] {
        if {![string is integer -strict $n] || $n < 0} {
            return [_json_error "save counters must be non-negative integers"]
        }
    }
    if {![info exists ::trial_video_save_fd] || $::trial_video_save_fd eq ""} {
        return [_json_error "no save in progress"]
    }
    if {[catch {close $::trial_video_save_fd} err]} {
        set ::trial_video_save_fd ""
        return [_json_error $err]
    }
    set ::trial_video_save_fd ""
    set path $::trial_video_save_path
    set base [trial_video_basename $filename]
    set ok [dict create \
                path $path \
                url "trial_video/$base/trial_${trial}.mp4" \
                trial $trial \
                frames $frames \
                clock_held $clock_held \
                reset_frames $reset_frames \
                background_missing $background_missing]
    return [_json_ok $ok]
}

# Whether the tape set a background by the start of this trial.
# missing 1 means the capture starts from black and the viewer says so.
proc trial_video_background {filename trial} {
    if {![string is integer -strict $trial] || $trial < 0} {
        return [_json_error "trial must be a non-negative integer"]
    }
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        return [_json_error "No datafile $filename"]
    }
    if {[catch {trial_video_load} load_err]} {
        return [_json_error $load_err]
    }
    if {[catch {set info [::trial_video::background $filepath $trial]} err]} {
        return [_json_error $err]
    }
    set ok [dict create missing [dict get $info missing]]
    if {[dict exists $info color]} {
        dict set ok color [dict get $info color]
    }
    return [_json_ok $ok]
}

# Paint the requested layers onto a copy of an existing trial video.
# Layer names follow the trial as separate words (touch, eye, or both).
# An empty layer list returns the clean file. Touch with no marks does too.
proc trial_video_burn {filename trial args} {
    set layers $args
    set root [trial_video_export_root]
    if {$root eq ""} {
        return [_json_error "Export is not configured"]
    }
    if {![string is integer -strict $trial] || $trial < 0} {
        return [_json_error "trial must be a non-negative integer"]
    }
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        return [_json_error "No datafile $filename"]
    }
    set base [trial_video_basename $filename]
    set clean [file join $root $base trial_${trial}.mp4]
    if {![file exists $clean]} {
        return [_json_error "No video for trial $trial"]
    }

    set wanted {}
    foreach name $layers {
        if {$name eq ""} continue
        if {$name ne "touch" && $name ne "eye"} {
            return [_json_error "Unknown layer $name"]
        }
        if {$name ni $wanted} {
            lappend wanted $name
        }
    }

    if {[llength $wanted] == 0} {
        set out $clean
        set layered 0
    } else {
        if {[catch {trial_video_load} load_err]} {
            return [_json_error $load_err]
        }
        if {[catch {set info [::trial_video::burn $filepath $trial $clean $wanted]} err]} {
            return [_json_error $err]
        }
        set out [dict get $info out]
        set layered [dict get $info layered]
    }

    set ok [dict create \
        path $out \
        url "trial_video/$base/[file tail $out]" \
        trial $trial \
        layered $layered]
    return [_json_ok $ok]
}

# Eye samples for one observation, in video seconds and frame pixels.
# The stim mp4 is unchanged; the viewer draws the dot itself.
proc trial_video_eyes {filename trial} {
    if {![string is integer -strict $trial] || $trial < 0} {
        return [_json_error "trial must be a non-negative integer"]
    }
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        return [_json_error "No datafile $filename"]
    }
    if {[catch {trial_video_load} load_err]} {
        return [_json_error $load_err]
    }
    if {[info commands ::trial_video::eyes] eq ""} {
        return [_json_error "trial_video eye reader is not loaded"]
    }
    if {[catch {set info [::trial_video::eyes $filepath $trial]} err]} {
        return [_json_error $err]
    }

    set json [yajl create #auto]
    $json map_open
    $json string status string ok
    $json string width number [dict get $info width]
    $json string height number [dict get $info height]
    $json string eyes array_open
    foreach ev [dict get $info samples] {
        $json map_open
        $json string t number [format %.6f [dict get $ev t]]
        $json string x number [format %.2f [dict get $ev x]]
        $json string y number [format %.2f [dict get $ev y]]
        $json map_close
    }
    $json array_close
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# Presses and releases for one observation, in video seconds and frame pixels.
# The stim mp4 is unchanged; the viewer draws these itself.
proc trial_video_touches {filename trial} {
    if {![string is integer -strict $trial] || $trial < 0} {
        return [_json_error "trial must be a non-negative integer"]
    }
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        return [_json_error "No datafile $filename"]
    }
    if {[catch {trial_video_load} load_err]} {
        return [_json_error $load_err]
    }
    if {[info commands ::trial_video::touches] eq ""} {
        return [_json_error "trial_video touches reader is not loaded"]
    }
    if {[catch {set info [::trial_video::touches $filepath $trial]} err]} {
        return [_json_error $err]
    }

    set json [yajl create #auto]
    $json map_open
    $json string status string ok
    $json string width number [dict get $info width]
    $json string height number [dict get $info height]
    $json string touches array_open
    foreach ev [dict get $info events] {
        $json map_open
        $json string kind string [dict get $ev kind]
        $json string t number [format %.6f [dict get $ev t]]
        $json string x number [format %.2f [dict get $ev x]]
        $json string y number [format %.2f [dict get $ev y]]
        $json map_close
    }
    $json array_close
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# Half-angles for one datafile. null when either was not stored.
proc trial_video_screen {filename} {
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        return [_json_error "No datafile $filename"]
    }
    if {[catch {trial_video_load} load_err]} {
        return [_json_error $load_err]
    }
    if {[info commands ::trial_video::screen] eq ""} {
        return [_json_error "trial_video screen reader is not loaded"]
    }
    if {[catch {set info [::trial_video::screen $filepath]} err]} {
        return [_json_error $err]
    }
    set halfx [dict get $info halfx]
    set halfy [dict get $info halfy]
    if {$halfx eq "" || $halfy eq ""} {
        return "{\"status\":\"ok\",\"halfx\":null,\"halfy\":null}"
    }
    return "{\"status\":\"ok\",\"halfx\":$halfx,\"halfy\":$halfy}"
}

# Layer names present in one datafile. The viewer only offers these.
proc trial_video_layers {filename} {
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        return [_json_error "No datafile $filename"]
    }
    if {[catch {trial_video_load} load_err]} {
        return [_json_error $load_err]
    }
    if {[info commands ::trial_video::layers] eq ""} {
        return [_json_error "trial_video layers reader is not loaded"]
    }
    if {[catch {set info [::trial_video::layers $filepath]} err]} {
        return [_json_error $err]
    }

    set json [yajl create #auto]
    $json map_open
    $json string status string ok
    $json string eye_virtual number [dict get $info virtual]
    $json string layers array_open
    foreach name [dict get $info names] {
        $json string $name
    }
    $json array_close
    $json map_close
    set result [$json get]
    $json delete
    return $result
}
