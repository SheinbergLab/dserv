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
            $json map_open
            $json string name string $name
            if {[regexp {trial_(\d+)\.mp4} $name -> n]} {
                $json string trial number $n
            }
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

proc trial_video_make {filename trial} {
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
    foreach key {clock_held reset_frames} {
        if {[dict exists $info $key]} {
            dict set ok $key [dict get $info $key]
        }
    }
    return [_json_ok $ok]
}
