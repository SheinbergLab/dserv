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
    if {[catch {set layers [::trial_video::layers $filepath]} err]} {
        return [_json_error $err]
    }

    set json [yajl create #auto]
    $json map_open
    $json string status string ok
    $json string layers array_open
    foreach name $layers {
        $json string $name
    }
    $json array_close
    $json map_close
    set result [$json get]
    $json delete
    return $result
}
