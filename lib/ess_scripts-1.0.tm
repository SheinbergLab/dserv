# -*- mode: tcl -*-
#
# ess_scripts-1.0.tm - Script-management engine for the `scripts` subprocess
#
# Runs the registry-facing half of script management (preview, pull, push,
# diff, history) OUTSIDE the ess interp, so registry HTTP and tree hashing
# never stall the experiment interp or the main (client-serving) interp.
# Loaded by config/scriptsconf.tcl into the dedicated `scripts` subprocess.
#
# Every command returns JSON and also publishes it to a datapoint, so a
# client can either wait on `send scripts {...}` for the reply or fire the
# command with sendNoReply and consume the datapoint update:
#
#   scripts::sync_preview ?-fresh?             -> scripts/sync_preview
#   scripts::push_preview <system> ?-fresh?    -> scripts/push_preview
#   scripts::pull_all                          -> scripts/sync_result
#   scripts::pull <system> ?version?           -> scripts/sync_result
#   scripts::push <system> ?-user U? ?-comment C?
#                 ?-include_libs 0|1? ?-add 0|1?  -> scripts/push_result
#   scripts::diff <system> <protocol> <type>   -> scripts/diff
#   scripts::lib_diff <filename>               -> scripts/diff
#   scripts::history <system> <protocol> <type> ?limit? -> scripts/history
#   scripts::history_diff <system> <protocol> <type> <checksum> ?<checksum2>?
#                                              -> scripts/history_diff
#   scripts::status                            -> scripts/status
#
# Preview JSON matches the shape the ess_control Sync modal expects:
# per-group {to_pull[] skipped[] extra[] unchanged} where to_pull entries
# carry the 3-way decision (pull|conflict|cold) and skipped means
# keep_local. push_preview: {system systemExists changed[] new[] missing[]
# unchanged libs[]}.
#
# The 3-way base-manifest machinery, file writes, and displacement all
# come from ess_sync (shared with the ess interp and dservctl); this
# module adds the workgroup-manifest cache, preview classification, the
# push engine, diff/history, and datapoint publication.
#
# Requires: dlsh (json_to_dict / dict_to_json / json_get), yajltcl,
#           ess_paths, ess_sync, and the https_* / sha256 / dserv
#           builtins present in every TclServer interp.

package require tcljson
package require yajltcl
package require ess_paths
package require ess_sync

package provide ess_scripts 1.0

# ── ess-namespace support shims ─────────────────────────────────────
#
# ess_sync's procs live in namespace ess and call logging/ownership
# helpers that ess-2.0.tm normally provides. The scripts subprocess
# deliberately does not load ess-2.0, so supply minimal equivalents.
# Guarded so loading this module inside a full ess interp changes nothing.
#
namespace eval ess {
    if {[llength [info procs ::ess::ess_log]] == 0} {
        proc ess_log {level message {category general}} {
            if {[info exists ::env(ESS_CONSOLE_LOG)] && $::env(ESS_CONSOLE_LOG)} {
                puts stderr "SCRIPTS $level: $message"
            }
            set ts [clock format [clock seconds] -format "%H:%M:%S"]
            set formatted "\[$ts\] \[$category\] $message"
            switch -- $level {
                error   { dservSet ess/errorInfo $formatted }
                warning { dservSet ess/warningInfo $formatted }
                info    { dservSet ess/infoLog $formatted }
                debug   { }
                default { dservSet ess/generalLog $formatted }
            }
        }
        proc ess_error   {message {category system}} { ess_log error $message $category }
        proc ess_warning {message {category system}} { ess_log warning $message $category }
        proc ess_info    {message {category system}} { ess_log info $message $category }
        proc ess_debug   {message {category system}} { ess_log debug $message $category }
    }
    if {[llength [info procs ::ess::mkdir_matching_owner]] == 0} {
        proc mkdir_matching_owner {dir} { ess::paths::mkdir_matching_owner $dir }
        proc fix_file_ownership {filepath} { ess::paths::fix_file_ownership $filepath }
    }
}

namespace eval scripts {

    variable state idle              ;# idle | syncing | pushing
    variable manifest_cache {}
    variable manifest_cache_key {}
    variable manifest_cache_time 0
    variable manifest_cache_ttl 60000

    # ── Initialization ──────────────────────────────────────────────
    #
    # Feeds the ess:: state ess_sync's procs expect. Registry url and
    # workgroup were already read from ESS_REGISTRY_URL / ESS_WORKGROUP
    # by ess_sync at package load; datapoints (published by the ess
    # subprocess) are the fallback for both, re-checked at initial_sync
    # in case this subprocess started first.
    #
    proc init {} {
        if {[info exists ::env(ESS_SYSTEM_PATH)]} {
            set ::ess::system_path $::env(ESS_SYSTEM_PATH)
        } elseif {![info exists ::ess::system_path] || $::ess::system_path eq ""} {
            set ::ess::system_path /usr/local/dserv/systems
        }

        set project ess
        if {[info exists ::env(ESS_PROJECT)] && $::env(ESS_PROJECT) ne ""} {
            set project $::env(ESS_PROJECT)
        }
        catch {
            if {[dservExists ess/project]} {
                set p [dservGet ess/project]
                if {$p ne ""} { set project $p }
            }
        }
        set ::ess::current(project) $project
        if {![info exists ::ess::overlay_path]} { set ::ess::overlay_path "" }

        _registry_from_dserv

        ess::paths::configure -system_path $::ess::system_path \
            -project $project -overlay_path ""

        # Follow project switches made in the ess subprocess.
        dservAddExactMatch ess/project
        dpointSetScript ess/project scripts::_project_changed

        catch { dservSet scripts/project $project }
        _set_state idle init
        return "project=$project system_path=$::ess::system_path\
            registry=$::ess::registry_url wg=$::ess::registry_workgroup"
    }

    proc _registry_from_dserv {} {
        if {![info exists ::ess::registry_url] || $::ess::registry_url eq ""} {
            catch {
                if {[dservExists ess/registry/url]} {
                    set ::ess::registry_url [dservGet ess/registry/url]
                }
            }
        }
        if {![info exists ::ess::registry_workgroup] || $::ess::registry_workgroup eq ""} {
            catch {
                if {[dservExists ess/registry/workgroup]} {
                    set ::ess::registry_workgroup [dservGet ess/registry/workgroup]
                }
            }
        }
    }

    proc _project_changed {name value} {
        if {$value eq ""} return
        set ::ess::current(project) $value
        ess::paths::configure -project $value
        _manifest_invalidate
        catch { dservSet scripts/project $value }
    }

    # Deferred boot sync (armed by scriptsconf via dservAfter): dserv
    # boots on the on-disk tree; the workgroup pull lands here seconds
    # later without blocking boot, ess, or any client.
    proc initial_sync {} {
        _registry_from_dserv
        if {![info exists ::ess::registry_url] || $::ess::registry_url eq ""} {
            ess::ess_info "scripts: no registry configured — skipping initial sync" "sync"
            return
        }
        if {[catch { pull_all } err]} {
            ess::ess_error "scripts: initial sync failed: $err" "sync"
        }
    }

    # ── Guards / status ─────────────────────────────────────────────

    proc _require_registry {} {
        _registry_from_dserv
        if {![info exists ::ess::registry_url] || $::ess::registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {![info exists ::ess::registry_workgroup] || $::ess::registry_workgroup eq ""} {
            error "Workgroup not configured"
        }
    }

    proc _ess_running {} {
        set r 0
        catch {
            if {[dservExists ess/status]} {
                set r [expr {[dservGet ess/status] eq "running"}]
            }
        }
        return $r
    }

    proc _set_state {s op} {
        variable state
        set state $s
        set obj [yajl create #auto]
        $obj map_open
        $obj string state string $s
        $obj string op string $op
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/status $json }
        return $json
    }

    proc status {} {
        variable state
        return [_set_state $state status]
    }

    # ── Workgroup manifest cache ────────────────────────────────────
    #
    # GET /api/v1/ess/manifest/<wg> returns every system's script
    # checksums plus the lib checksums — everything both preview kinds
    # need — in one request. Cached with a short TTL; any operation
    # that changes registry state invalidates it.
    #
    proc _manifest_invalidate {} {
        variable manifest_cache {}
        variable manifest_cache_key {}
        variable manifest_cache_time 0
    }

    proc _manifest_fetch {{version main} {force 0}} {
        variable manifest_cache
        variable manifest_cache_key
        variable manifest_cache_time
        variable manifest_cache_ttl

        _require_registry
        set key "$::ess::registry_workgroup|$version"
        set now [clock milliseconds]
        if {!$force && $manifest_cache ne "" && $manifest_cache_key eq $key &&
            ($now - $manifest_cache_time) < $manifest_cache_ttl} {
            return $manifest_cache
        }
        set url "$::ess::registry_url/api/v1/ess/manifest/$::ess::registry_workgroup?version=$version"
        set response [https_get $url]
        # The manifest carries checksums only (no script content), so
        # json_to_dict is safe here.
        set data [json_to_dict $response]
        set manifest_cache $data
        set manifest_cache_key $key
        set manifest_cache_time $now
        return $data
    }

    proc _manifest_libs {data} {
        set out [dict create]
        if {![dict exists $data libs]} { return $out }
        foreach lib [dict get $data libs] {
            if {[dict exists $lib filename] && [dict exists $lib checksum]} {
                dict set out [dict get $lib filename] [dict get $lib checksum]
            }
        }
        return $out
    }

    proc _manifest_find_system {data system} {
        if {[dict exists $data systems]} {
            foreach sys [dict get $data systems] {
                if {[dict exists $sys system] && [dict get $sys system] eq $system} {
                    return [list 1 $sys]
                }
            }
        }
        return [list 0 {}]
    }

    # relkey -> {protocol type filename checksum} for one manifest entry.
    proc _manifest_scripts_of {sys_entry} {
        set out [dict create]
        if {![dict exists $sys_entry scripts]} { return $out }
        foreach s [dict get $sys_entry scripts] {
            set protocol ""; set stype ""; set filename ""; set checksum ""
            catch { set protocol [dict get $s protocol] }
            catch { set stype    [dict get $s type] }
            catch { set checksum [dict get $s checksum] }
            catch { set filename [dict get $s filename] }
            if {$filename eq ""} {
                if {$stype eq ""} continue
                set filename $stype
            }
            dict set out [_relkey $protocol $filename] [dict create \
                protocol $protocol type $stype filename $filename checksum $checksum]
        }
        return $out
    }

    # Same relkey convention as ess_sync's base manifests and dservctl:
    # bare filename for system-level, protocol/filename otherwise.
    proc _relkey {protocol filename} {
        if {$protocol eq "" || $protocol eq "_" || $protocol eq "_system"} {
            return $filename
        }
        return [file join $protocol $filename]
    }

    # ── Local tree scan ─────────────────────────────────────────────
    #
    # relkey -> absolute path for every script file (.tcl/.js) in a
    # system dir: system-level files plus one level of protocol dirs.
    # Dotfiles (including .sync_base.json) and editor backups excluded.
    #
    proc _scan_local {sys_dir} {
        set out [dict create]
        if {![file isdirectory $sys_dir]} { return $out }
        foreach entry [glob -nocomplain -directory $sys_dir *] {
            set tail [file tail $entry]
            if {[string match .* $tail]} continue
            if {[file isdirectory $entry]} {
                foreach sub [glob -nocomplain -directory $entry *] {
                    if {[file isdirectory $sub]} continue
                    set name [file tail $sub]
                    if {[string match .* $name]} continue
                    if {![regexp {\.(tcl|js)$} $name]} continue
                    dict set out [file join $tail $name] $sub
                }
            } else {
                if {![regexp {\.(tcl|js)$} $tail]} continue
                dict set out $tail $entry
            }
        }
        return $out
    }

    # ── Preview classification ──────────────────────────────────────

    proc _system_preview {system server_scripts} {
        set project $::ess::current(project)
        set sys_dir [file join $::ess::system_path $project $system]
        set local [_scan_local $sys_dir]
        set manifest [ess::_base_manifest_read [ess::_base_manifest_path $project $system]]

        set to_pull {}
        set skipped {}
        set extra {}
        set unchanged 0

        dict for {relkey info} $server_scripts {
            set reg_cs [dict get $info checksum]
            set local_cs ""
            if {[dict exists $local $relkey]} {
                set local_cs [sha256 -file [dict get $local $relkey]]
            }
            if {$local_cs eq $reg_cs} { incr unchanged; continue }
            # A missing local file is an unambiguous pull (nothing to lose).
            if {$local_cs eq ""} {
                set decision pull
            } else {
                set decision [ess::_base_decide \
                    [ess::_base_entry_get $manifest $relkey] $local_cs $reg_cs]
            }
            set entry [dict create relpath $relkey \
                protocol [dict get $info protocol] \
                type [dict get $info type] decision $decision]
            if {$decision eq "keep_local"} {
                lappend skipped $entry
            } else {
                lappend to_pull $entry
            }
        }
        dict for {relkey path} $local {
            if {![dict exists $server_scripts $relkey]} { lappend extra $relkey }
        }
        return [dict create to_pull $to_pull skipped $skipped extra $extra unchanged $unchanged]
    }

    proc _libs_preview {server_libs} {
        set project $::ess::current(project)
        set lib_dir [file join $::ess::system_path $project lib]
        set manifest [ess::_base_manifest_read [ess::_base_lib_manifest_path $project]]

        set to_pull {}
        set skipped {}
        set unchanged 0

        dict for {filename reg_cs} $server_libs {
            set f [file join $lib_dir $filename]
            set local_cs ""
            if {[file exists $f]} { set local_cs [sha256 -file $f] }
            if {$local_cs eq $reg_cs} { incr unchanged; continue }
            if {$local_cs eq ""} {
                set decision pull
            } else {
                set decision [ess::_base_decide \
                    [ess::_base_entry_get $manifest $filename] $local_cs $reg_cs]
            }
            if {$decision eq "keep_local"} {
                lappend skipped [dict create filename $filename decision $decision]
            } else {
                lappend to_pull [dict create filename $filename decision $decision]
            }
        }
        set extra {}
        foreach f [glob -nocomplain -directory $lib_dir *.tm] {
            set fname [file tail $f]
            if {![dict exists $server_libs $fname]} { lappend extra $fname }
        }
        return [dict create to_pull $to_pull skipped $skipped extra $extra unchanged $unchanged]
    }

    # ── JSON emission (yajl: dict_to_json cannot express arrays of
    #    objects, so previews are built explicitly) ─────────────────

    proc _obj_array {obj items} {
        $obj array_open
        foreach item $items {
            $obj map_open
            dict for {k v} $item {
                if {$k eq "canonical"} {
                    $obj string $k bool $v
                } else {
                    $obj string $k string $v
                }
            }
            $obj map_close
        }
        $obj array_close
    }

    proc _str_array {obj items} {
        $obj array_open
        foreach s $items { $obj string $s }
        $obj array_close
    }

    proc _group_json {obj p} {
        $obj map_open
        $obj string to_pull
        _obj_array $obj [dict get $p to_pull]
        $obj string skipped
        _obj_array $obj [dict get $p skipped]
        $obj string extra
        _str_array $obj [dict get $p extra]
        $obj string unchanged number [dict get $p unchanged]
        $obj map_close
    }

    # Generic flat result -> JSON + datapoint. Integer values and the
    # errors list are typed; everything else is a string.
    proc _publish_result {dpname d} {
        set obj [yajl create #auto]
        $obj map_open
        dict for {k v} $d {
            if {$k eq "errors"} {
                $obj string errors
                _str_array $obj $v
            } elseif {[string is integer -strict $v] && [string length $v] < 10} {
                $obj string $k number $v
            } else {
                $obj string $k string $v
            }
        }
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/$dpname $json }
        return $json
    }

    # ── Pull preview (whole workgroup: shared libs + every system) ──

    proc sync_preview {args} {
        set version main
        set force 0
        foreach a $args {
            switch -- $a {
                -fresh - -force { set force 1 }
                default         { set version $a }
            }
        }
        _require_registry
        set data [_manifest_fetch $version $force]

        set libs_p [_libs_preview [_manifest_libs $data]]
        set systems [dict create]
        if {[dict exists $data systems]} {
            foreach sys_m [dict get $data systems] {
                if {![dict exists $sys_m system]} continue
                set name [dict get $sys_m system]
                dict set systems $name \
                    [_system_preview $name [_manifest_scripts_of $sys_m]]
            }
        }

        set obj [yajl create #auto]
        $obj map_open
        $obj string workgroup string $::ess::registry_workgroup
        $obj string version string $version
        $obj string ts number [clock seconds]
        $obj string libs
        _group_json $obj $libs_p
        $obj string systems
        $obj map_open
        dict for {name p} $systems {
            $obj string $name
            _group_json $obj $p
        }
        $obj map_close
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/sync_preview $json }
        return $json
    }

    # ── Push scan / preview ─────────────────────────────────────────
    #
    # The registry names scripts by (protocol, type) and derives the
    # filename from convention, so only conventionally-named local files
    # can round-trip. New files that don't match the convention are
    # listed with canonical=false and skipped at push time.
    #
    proc _derive_proto_type {system relkey} {
        set parts [file split $relkey]
        if {[llength $parts] == 1} {
            set fname [lindex $parts 0]
            set base [file rootname $fname]
            if {[string match *_viewer $base]}  { return [list "" viewer] }
            if {[string match *_extract $base]} { return [list "" extract] }
            if {[string match *_analyze $base]} { return [list "" analyze] }
            return [list "" system]
        }
        lassign $parts proto fname
        set base [file rootname $fname]
        if {$base eq $proto} { return [list $proto protocol] }
        if {[string match *_viewer $base]} { return [list $proto viewer] }
        if {[string first "${proto}_" $base] == 0} {
            return [list $proto [string range $base [string length "${proto}_"] end]]
        }
        return [list $proto $base]
    }

    proc _push_scan {system {version main} {force 0}} {
        set data [_manifest_fetch $version $force]
        set project $::ess::current(project)
        set sys_dir [file join $::ess::system_path $project $system]

        lassign [_manifest_find_system $data $system] exists sys_entry
        set server_scripts [dict create]
        if {$exists} { set server_scripts [_manifest_scripts_of $sys_entry] }

        set local [_scan_local $sys_dir]
        set changed {}
        set new {}
        set missing {}
        set unchanged 0

        dict for {relkey info} $server_scripts {
            if {![dict exists $local $relkey]} {
                lappend missing $relkey
                continue
            }
            set local_cs [sha256 -file [dict get $local $relkey]]
            if {$local_cs eq [dict get $info checksum]} {
                incr unchanged
            } else {
                lappend changed [dict create relkey $relkey \
                    protocol [dict get $info protocol] \
                    type [dict get $info type] \
                    filename [dict get $info filename] \
                    checksum [dict get $info checksum]]
            }
        }

        set sys_complete [file exists [file join $sys_dir ${system}.tcl]]
        dict for {relkey path} $local {
            if {[dict exists $server_scripts $relkey]} continue
            # Fragments of incomplete protocols/systems are not push
            # candidates (they stay visible as local-only in the sync
            # preview): the protocol file must exist before its other
            # scripts can go up.
            if {!$sys_complete} continue
            set parts [file split $relkey]
            if {[llength $parts] == 2} {
                set p0 [lindex $parts 0]
                if {![file exists [file join $sys_dir $p0 ${p0}.tcl]]} continue
            }
            lassign [_derive_proto_type $system $relkey] proto stype
            set fname [file tail $relkey]
            set canonical [expr {[ess::_script_filename $system $proto $stype] eq $fname}]
            lappend new [dict create relkey $relkey protocol $proto type $stype \
                filename $fname checksum "" canonical $canonical]
        }

        # Dependent shared libs with local changes (see _lib_closure).
        set libs {}
        set closure [_lib_closure $system]
        if {[llength $closure]} {
            set lib_status [_lib_local_status [_manifest_libs $data]]
            foreach fname $closure {
                if {![dict exists $lib_status $fname]} continue
                set st [dict get $lib_status $fname]
                if {$st eq "modified" || $st eq "local_only"} {
                    lappend libs $fname
                }
            }
        }

        return [dict create exists $exists changed $changed new $new \
            missing $missing unchanged $unchanged libs $libs]
    }

    proc push_preview {system args} {
        set version main
        set force 0
        foreach a $args {
            switch -- $a {
                -fresh - -force { set force 1 }
                default         { set version $a }
            }
        }
        _require_registry
        set p [_push_scan $system $version $force]

        set obj [yajl create #auto]
        $obj map_open
        $obj string system string $system
        $obj string systemExists bool [dict get $p exists]
        $obj string changed
        _obj_array $obj [dict get $p changed]
        $obj string new
        _obj_array $obj [dict get $p new]
        $obj string missing
        _str_array $obj [dict get $p missing]
        $obj string unchanged number [dict get $p unchanged]
        $obj string libs
        _str_array $obj [dict get $p libs]
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/push_preview $json }
        return $json
    }

    # ── System -> shared-lib dependency closure ─────────────────────
    #
    # Push is scoped to one system but shared libs live in the project
    # lib/ dir. Offer only the libs the system actually loads: scan its
    # scripts for `package require`, keep names a project lib provides,
    # resolve each to the file Tcl would load, and recurse through those
    # libs' own requires (planko-3.0 requires thread_pool, which no
    # system names directly).
    #
    proc _lib_files_by_name {lib_dir} {
        set out [dict create]
        foreach f [glob -nocomplain -directory $lib_dir *.tm] {
            set fname [file tail $f]
            if {[regexp {^(.+)-(\d[\d.]*)\.tm$} $fname -> name version]} {
                dict lappend out $name [list $version $fname]
            }
        }
        return $out
    }

    # Tcl's rule: highest version with the same major that satisfies the
    # request (exact when -exact). Empty request takes the highest.
    proc _resolve_lib {versions req exact} {
        set best ""
        set best_v ""
        foreach entry $versions {
            lassign $entry v fname
            if {$req ne ""} {
                if {$exact} {
                    if {[package vcompare $v $req] != 0} continue
                } else {
                    if {[lindex [split $v .] 0] ne [lindex [split $req .] 0]} continue
                    if {[package vcompare $v $req] < 0} continue
                }
            }
            if {$best_v eq "" || [package vcompare $v $best_v] > 0} {
                set best_v $v
                set best $fname
            }
        }
        return $best
    }

    # {name version exact} triples; comment-only lines skipped so doc
    # examples don't register as dependencies.
    proc _scan_requires {path} {
        set found {}
        if {[catch {
            set f [open $path r]
            set content [read $f]
            close $f
        }]} { return $found }
        set re {package\s+require\s+(-exact\s+)?([A-Za-z_][\w:]*)\s*([0-9][\w.]*)?}
        foreach line [split $content \n] {
            if {[string index [string trimleft $line] 0] eq "#"} continue
            foreach {- exact name version} [regexp -all -inline $re $line] {
                lappend found [list $name $version [expr {$exact ne ""}]]
            }
        }
        return $found
    }

    proc _lib_closure {system} {
        set project $::ess::current(project)
        set lib_dir [file join $::ess::system_path $project lib]
        set by_name [_lib_files_by_name $lib_dir]
        if {[dict size $by_name] == 0} { return {} }

        set pending {}
        dict for {relkey path} [_scan_local [file join $::ess::system_path $project $system]] {
            if {[string match *.tcl $relkey]} {
                lappend pending {*}[_scan_requires $path]
            }
        }

        set resolved [dict create]
        set seen [dict create]
        while {[llength $pending]} {
            set req [lindex $pending 0]
            set pending [lrange $pending 1 end]
            lassign $req name version exact
            if {![dict exists $by_name $name]} continue
            set k "$name|$version|$exact"
            if {[dict exists $seen $k]} continue
            dict set seen $k 1
            set fname [_resolve_lib [dict get $by_name $name] $version $exact]
            if {$fname eq "" || [dict exists $resolved $fname]} continue
            dict set resolved $fname 1
            lappend pending {*}[_scan_requires [file join $lib_dir $fname]]
        }
        return [lsort [dict keys $resolved]]
    }

    # filename -> synced|modified|local_only for local lib files.
    proc _lib_local_status {server_libs} {
        set project $::ess::current(project)
        set lib_dir [file join $::ess::system_path $project lib]
        set out [dict create]
        foreach f [glob -nocomplain -directory $lib_dir *.tm] {
            set fname [file tail $f]
            if {[dict exists $server_libs $fname]} {
                if {[sha256 -file $f] eq [dict get $server_libs $fname]} {
                    dict set out $fname synced
                } else {
                    dict set out $fname modified
                }
            } else {
                dict set out $fname local_only
            }
        }
        return $out
    }

    # ── Pull operations ─────────────────────────────────────────────
    #
    # Pulls rewrite files under the loaded system, so they are refused
    # while an experiment is running (the same rule essctrl applies to
    # ess::sync_base). The actual sync work is ess_sync's, complete with
    # 3-way base decisions and displacement of conflicting local edits.
    #
    proc pull_all {} {
        variable state
        # Every invocation ends with exactly one scripts/sync_result
        # publication — guard refusals included — so datapoint-driven
        # clients never wait on an operation that already failed.
        if {[catch {
            _require_registry
            if {[_ess_running]} { error "Cannot pull while an experiment is running" }
            if {$state ne "idle"} { error "scripts subprocess busy ($state)" }
        } err]} {
            _publish_result sync_result [dict create op pull_all \
                pulled 0 unchanged 0 errors [list $err]]
            error $err
        }

        _set_state syncing pull_all
        if {[catch { set result [ess::sync_base] } err]} {
            _set_state idle pull_all
            _publish_result sync_result [dict create op pull_all \
                pulled 0 unchanged 0 errors [list $err]]
            error $err
        }
        _manifest_invalidate
        _set_state idle pull_all
        catch { dirty }
        catch { dservSet scripts/last_sync [clock seconds] }
        return [_publish_result sync_result [dict create op pull_all \
            pulled [dict get $result pulled] \
            unchanged [dict get $result unchanged] \
            errors [dict get $result errors]]]
    }

    proc pull {system {version main}} {
        variable state
        if {[catch {
            _require_registry
            if {[_ess_running]} { error "Cannot pull while an experiment is running" }
            if {$state ne "idle"} { error "scripts subprocess busy ($state)" }
        } err]} {
            _publish_result sync_result [dict create op pull system $system \
                pulled 0 unchanged 0 errors [list $err]]
            error $err
        }

        _set_state syncing pull
        if {[catch { set result [ess::sync_system $system $version] } err]} {
            _set_state idle pull
            _publish_result sync_result [dict create op pull system $system \
                pulled 0 unchanged 0 errors [list $err]]
            error $err
        }
        _manifest_invalidate
        _set_state idle pull
        catch { dirty }
        catch { dservSet scripts/last_sync [clock seconds] }
        return [_publish_result sync_result [dict create op pull system $system \
            pulled [dict get $result pulled] \
            unchanged [dict get $result unchanged] \
            errors [dict get $result errors]]]
    }

    # ── Push engine ─────────────────────────────────────────────────
    #
    # Pushes one system's changed scripts (and, with -add 1, new ones —
    # creating the system on the registry if needed), then dependent
    # changed libs when -include_libs 1. Each PUT carries the base-
    # manifest ancestor as expectedChecksum so a concurrent edit on the
    # registry surfaces as a per-file conflict (HTTP 409) instead of a
    # silent overwrite; successful pushes advance the base.
    #
    proc push {system args} {
        variable state
        # Like pull_all: exactly one scripts/push_result publication per
        # invocation, whether the push ran, was refused, or failed.
        if {[catch {
            _require_registry

            set user ""
            set comment ""
            set include_libs 0
            set add_new 0
            foreach {k v} $args {
                switch -- $k {
                    -user         { set user $v }
                    -comment      { set comment $v }
                    -include_libs { set include_libs $v }
                    -add          { set add_new $v }
                    default       { error "push: unknown option $k" }
                }
            }
            if {$user eq ""} {
                catch {
                    if {[dservExists ess/registry/user]} {
                        set user [dservGet ess/registry/user]
                    }
                }
            }
            if {$user eq "" && [info exists ::env(USER)]} { set user $::env(USER) }
            if {$user ne ""} {
                set role [ess::_get_user_role $user]
                if {$role eq "viewer"} {
                    error "User '$user' has role 'viewer' and cannot push"
                }
            }
            if {$comment eq ""} { set comment "pushed from dserv" }
            if {$state ne "idle"} { error "scripts subprocess busy ($state)" }
        } err]} {
            _publish_result push_result [dict create op push system $system \
                pushed 0 added 0 lib_pushed 0 errors [list $err]]
            error $err
        }

        _set_state pushing $system
        if {[catch {
            set result [_push_engine $system $user $comment $include_libs $add_new]
        } err]} {
            _set_state idle push
            _publish_result push_result [dict create op push system $system \
                pushed 0 added 0 lib_pushed 0 errors [list $err]]
            error $err
        }
        _manifest_invalidate
        _set_state idle push
        catch { dirty }
        return [_publish_result push_result $result]
    }

    proc _push_engine {system user comment include_libs add_new} {
        # Fresh scan so we never push from a stale view of the registry.
        set p [_push_scan $system main 1]
        set exists [dict get $p exists]
        set changed [dict get $p changed]
        set new [dict get $p new]

        set pushed 0
        set added 0
        set lib_pushed 0
        set errors {}

        if {!$exists} {
            if {!$add_new} {
                error "System '$system' not on registry — push with -add 1 to create it"
            }
            if {[llength $new] || [llength $changed]} {
                set first_proto "_"
                foreach e $new {
                    set pr [dict get $e protocol]
                    if {$pr ne "" && $pr ne "_"} { set first_proto $pr; break }
                }
                set url "$::ess::registry_url/api/v1/ess/scaffold/system"
                set body [dict_to_json [dict create \
                    workgroup $::ess::registry_workgroup system $system \
                    protocol $first_proto createdBy $user]]
                https_post $url $body
            }
        }

        # Non-canonical filenames (utility scripts, backups) can't round-trip
        # through the registry's (protocol, type) naming — they are skipped,
        # not errors: the push preview already lists them as not pushable.
        set skipped {}
        set to_push $changed
        if {$add_new} {
            foreach e $new {
                if {![dict get $e canonical]} {
                    lappend skipped [dict get $e relkey]
                    continue
                }
                lappend to_push $e
            }
        }

        set project $::ess::current(project)
        set mpath [ess::_base_manifest_path $project $system]
        set manifest [ess::_base_manifest_read $mpath]
        set dirty 0

        foreach entry $to_push {
            set relkey [dict get $entry relkey]
            if {[catch {
                set cs [_put_script $system $project $entry $user $comment $manifest]
            } err]} {
                lappend errors "$relkey: $err"
                continue
            }
            ess::_base_entry_set manifest $relkey $cs main $user
            set dirty 1
            if {[dict get $entry checksum] eq ""} { incr added } else { incr pushed }
        }
        if {$dirty} { ess::_base_manifest_write $mpath $manifest }

        if {$include_libs} {
            foreach fname [dict get $p libs] {
                if {[catch { ess::commit_lib $fname $comment $user } lerr]} {
                    lappend errors "lib/$fname: $lerr"
                } else {
                    incr lib_pushed
                }
            }
        }

        return [dict create op push system $system pushed $pushed added $added \
            lib_pushed $lib_pushed skipped [llength $skipped] errors $errors]
    }

    proc _put_script {system project entry user comment manifest} {
        set relkey [dict get $entry relkey]
        set protocol [dict get $entry protocol]
        set stype [dict get $entry type]

        set local_file [file join $::ess::system_path $project $system $relkey]
        if {![file exists $local_file]} { error "local file missing" }
        set f [open $local_file r]
        set content [read $f]
        close $f

        # Ancestor for optimistic concurrency: prefer the recorded base,
        # fall back to the registry checksum from the scan. Empty (a new
        # script) means no conflict check — the server has nothing yet.
        set expected [ess::_base_entry_get $manifest $relkey]
        if {$expected eq ""} { set expected [dict get $entry checksum] }

        set api_proto $protocol
        if {$api_proto eq "" || $api_proto eq "_system"} { set api_proto "_" }
        set url "$::ess::registry_url/api/v1/ess/script/$::ess::registry_workgroup/$system/$api_proto/$stype"
        set body [dict_to_json [dict create content $content updatedBy $user \
            comment $comment expectedChecksum $expected]]
        set response [https_put $url $body]

        set new_cs ""
        catch { set new_cs [json_get $response checksum] }
        if {$new_cs eq ""} { set new_cs [sha256 $content] }
        return $new_cs
    }

    # ── Per-file status / push (divergence mitigation) ──────────────
    #
    # file_status: the 3-way decision for ONE script — what a preview
    # computes per file, served from the manifest cache. The variant
    # editor uses it to warn before editing a stale/diverged file and
    # to decide whether auto-push is safe (only when "unchanged").
    #
    # push_file: push ONE script with expectedChecksum concurrency, for
    # the dialog's push-after-save. Never scaffolds, never touches
    # other files or libs — the whole point is a minimal, safe push.
    #

    proc file_status {system protocol type args} {
        set force 0
        foreach a $args { if {$a in {-fresh -force}} { set force 1 } }
        _require_registry
        set data [_manifest_fetch main $force]

        set filename [ess::_script_filename $system $protocol $type]
        set relkey [_relkey $protocol $filename]
        set project $::ess::current(project)
        set local_file [file join $::ess::system_path $project $system $relkey]

        lassign [_manifest_find_system $data $system] sys_exists sys_entry
        set reg_cs ""
        if {$sys_exists} {
            set server_scripts [_manifest_scripts_of $sys_entry]
            if {[dict exists $server_scripts $relkey]} {
                set reg_cs [dict get $server_scripts $relkey checksum]
            }
        }

        set local_cs ""
        if {[file exists $local_file]} { set local_cs [sha256 -file $local_file] }

        if {$reg_cs eq ""} {
            set decision [expr {$local_cs eq "" ? "missing" : "local_only"}]
        } elseif {$local_cs eq ""} {
            set decision pull
        } elseif {$local_cs eq $reg_cs} {
            set decision unchanged
        } else {
            set manifest [ess::_base_manifest_read \
                [ess::_base_manifest_path $project $system]]
            set decision [ess::_base_decide \
                [ess::_base_entry_get $manifest $relkey] $local_cs $reg_cs]
        }

        set obj [yajl create #auto]
        $obj map_open
        $obj string system string $system
        $obj string protocol string $protocol
        $obj string type string $type
        $obj string relkey string $relkey
        $obj string decision string $decision
        $obj string registryExists bool [expr {$reg_cs ne ""}]
        $obj string localExists bool [expr {$local_cs ne ""}]
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        return $json
    }

    proc push_file {system protocol type args} {
        _require_registry
        set user ""
        set comment ""
        foreach {k v} $args {
            switch -- $k {
                -user    { set user $v }
                -comment { set comment $v }
                default  { error "push_file: unknown option $k" }
            }
        }
        if {$user eq ""} {
            catch {
                if {[dservExists ess/registry/user]} {
                    set user [dservGet ess/registry/user]
                }
            }
        }
        if {$user eq "" && [info exists ::env(USER)]} { set user $::env(USER) }
        if {$user ne ""} {
            set role [ess::_get_user_role $user]
            if {$role eq "viewer"} {
                error "User '$user' has role 'viewer' and cannot push"
            }
        }
        if {$comment eq ""} { set comment "pushed from dserv" }

        set data [_manifest_fetch main 1]
        lassign [_manifest_find_system $data $system] sys_exists sys_entry
        if {!$sys_exists} {
            error "system '$system' not on the registry — push it from Sync Tasks first"
        }

        set filename [ess::_script_filename $system $protocol $type]
        set relkey [_relkey $protocol $filename]
        set project $::ess::current(project)
        set local_file [file join $::ess::system_path $project $system $relkey]
        if {![file exists $local_file]} { error "local file missing: $relkey" }

        set server_scripts [_manifest_scripts_of $sys_entry]
        set reg_cs ""
        if {[dict exists $server_scripts $relkey]} {
            set reg_cs [dict get $server_scripts $relkey checksum]
        }
        if {$reg_cs ne "" && [sha256 -file $local_file] eq $reg_cs} {
            return [_publish_result push_result [dict create op push_file \
                system $system relkey $relkey pushed 0 unchanged 1 errors {}]]
        }

        set entry [dict create relkey $relkey protocol $protocol type $type \
            filename $filename checksum $reg_cs]
        set mpath [ess::_base_manifest_path $project $system]
        set manifest [ess::_base_manifest_read $mpath]
        set cs [_put_script $system $project $entry $user $comment $manifest]
        ess::_base_entry_set manifest $relkey $cs main $user
        ess::_base_manifest_write $mpath $manifest

        _manifest_invalidate
        dirty
        ess::ess_info "Pushed $system/$relkey to registry" "sync"
        return [_publish_result push_result [dict create op push_file \
            system $system relkey $relkey pushed 1 unchanged 0 errors {}]]
    }

    # ── Unpushed-changes scan (the Sync badge) ──────────────────────
    #
    # Purely LOCAL: compares tracked files against their .sync_base
    # entries (modified/deleted since last sync or push) and counts
    # canonical local-new files (the pushable ones — utility scripts
    # and backups are excluded so the badge can reach zero). No
    # network. Publishes scripts/dirty; _dirty_periodic re-arms itself
    # so out-of-band edits (an LLM writing files) surface within a few
    # minutes.
    #

    proc _dirty_scan_system {system files_var} {
        upvar 1 $files_var files
        set project $::ess::current(project)
        set sys_dir [file join $::ess::system_path $project $system]
        set manifest [ess::_base_manifest_read \
            [ess::_base_manifest_path $project $system]]
        set entries [dict get $manifest entries]
        set local [_scan_local $sys_dir]

        dict for {relkey e} $entries {
            if {![dict exists $local $relkey]} {
                lappend files "$system/$relkey (deleted)"
            } elseif {[sha256 -file [dict get $local $relkey]] ne \
                          [dict get $e checksum]} {
                lappend files "$system/$relkey"
            }
        }
        # A new file counts only when it belongs to something loadable:
        # the system file must exist, and for protocol-level files the
        # protocol file too — a canonically-named fragment in an
        # incomplete protocol (a stray stim file, say) is clutter to
        # ignore, not work to push.
        set sys_complete [file exists [file join $sys_dir ${system}.tcl]]
        dict for {relkey path} $local {
            if {[dict exists $entries $relkey]} continue
            if {!$sys_complete} continue
            set parts [file split $relkey]
            if {[llength $parts] == 2} {
                set proto [lindex $parts 0]
                if {![file exists [file join $sys_dir $proto ${proto}.tcl]]} continue
            }
            lassign [_derive_proto_type $system $relkey] proto stype
            if {[ess::_script_filename $system $proto $stype] eq \
                    [file tail $relkey]} {
                lappend files "$system/$relkey (new)"
            }
        }
    }

    proc dirty {} {
        set project $::ess::current(project)
        set root [file join $::ess::system_path $project]
        set files {}

        foreach d [glob -nocomplain -type d [file join $root *]] {
            set name [file tail $d]
            if {[string match .* $name] || $name eq "lib"} continue
            catch { _dirty_scan_system $name files }
        }

        # shared libs vs their base manifest + canonical local-new libs
        catch {
            set lib_dir [file join $root lib]
            set manifest [ess::_base_manifest_read \
                [ess::_base_lib_manifest_path $project]]
            set entries [dict get $manifest entries]
            dict for {fname e} $entries {
                set f [file join $lib_dir $fname]
                if {![file exists $f]} {
                    lappend files "lib/$fname (deleted)"
                } elseif {[sha256 -file $f] ne [dict get $e checksum]} {
                    lappend files "lib/$fname"
                }
            }
            foreach f [glob -nocomplain -directory $lib_dir *.tm] {
                set fname [file tail $f]
                if {![dict exists $entries $fname] &&
                    [regexp {^(.+)-(\d[\d.]*)\.tm$} $fname]} {
                    lappend files "lib/$fname (new)"
                }
            }
        }

        set files [lsort $files]
        set obj [yajl create #auto]
        $obj map_open
        $obj string count number [llength $files]
        $obj string files
        _str_array $obj [lrange $files 0 49]
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/dirty $json }
        return $json
    }

    # Self-re-arming periodic dirty scan. Armed ONCE at boot (see
    # scriptsconf); scripts::dirty itself never re-arms, so on-demand
    # calls don't stack timers.
    proc _dirty_periodic {} {
        catch { dirty }
        catch { dservAfter 300000 scripts::_dirty_periodic }
    }

    # ── Local-first cloning (new protocol / new system) ─────────────
    #
    # Creation is LOCAL: clone an existing protocol or system directory
    # with the identifier renamed throughout (plain global substring
    # replace — the same transform used when hand-copying a protocol).
    # Only canonical files are cloned; utility scripts and backups in
    # the source are reported as skipped. Nothing touches the registry:
    # the clone appears as local-new files in the sync preview and is
    # pushed (with -add) once tested. The GUI follows a successful
    # clone with a guarded ess::load_system to land on the new copy.
    #

    proc _valid_script_name {name} {
        # existing corpus includes digit-first names (9point)
        return [regexp {^[A-Za-z0-9][A-Za-z0-9_-]*$} $name]
    }

    proc _clone_read_transform_write {src dst from to} {
        set f [open $src r]
        set content [read $f]
        close $f
        set f [open $dst w]
        puts -nonewline $f [string map [list $from $to] $content]
        close $f
        ess::paths::fix_file_ownership $dst
    }

    proc _clone_result_json {op fields files skipped} {
        set obj [yajl create #auto]
        $obj map_open
        $obj string op string $op
        foreach {k v} $fields { $obj string $k string $v }
        $obj string files
        _str_array $obj $files
        $obj string skipped
        _str_array $obj $skipped
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/clone_result $json }
        return $json
    }

    proc clone_protocol {system from_proto new_proto} {
        set project $::ess::current(project)
        set sys_dir [file join $::ess::system_path $project $system]
        if {![file isdirectory $sys_dir]} { error "system '$system' not found" }
        set src [file join $sys_dir $from_proto]
        if {![file isdirectory $src]} {
            error "protocol '$from_proto' not found in $system"
        }
        if {![_valid_script_name $new_proto]} {
            error "invalid protocol name '$new_proto' (letters, digits, _ and - only)"
        }
        if {$new_proto eq $from_proto} { error "new name equals source name" }
        set dst [file join $sys_dir $new_proto]
        if {[file exists $dst]} { error "protocol '$new_proto' already exists" }

        set canonical [dict create]
        foreach suffix {"" _loaders _variants _stim _extract} {
            dict set canonical ${from_proto}${suffix}.tcl ${new_proto}${suffix}.tcl
        }
        dict set canonical ${from_proto}_viewer.js ${new_proto}_viewer.js

        ess::paths::mkdir_matching_owner $dst
        set files {}
        set skipped {}
        foreach f [glob -nocomplain -directory $src *] {
            if {[file isdirectory $f]} continue
            set tail [file tail $f]
            if {[dict exists $canonical $tail]} {
                set newname [dict get $canonical $tail]
                _clone_read_transform_write $f [file join $dst $newname] \
                    $from_proto $new_proto
                lappend files $newname
            } else {
                lappend skipped $tail
            }
        }
        if {![llength $files]} {
            file delete -force $dst
            error "no canonical protocol files ([join [dict keys $canonical] {, }])\
                found in $from_proto"
        }
        ess::ess_info "Cloned protocol $system/$from_proto -> $new_proto\
            ([llength $files] files)" "scripts"
        return [_clone_result_json clone_protocol \
            [list system $system from $from_proto new $new_proto] \
            [lsort $files] [lsort $skipped]]
    }

    proc clone_system {from_system new_system {protocols {}}} {
        set project $::ess::current(project)
        set src [file join $::ess::system_path $project $from_system]
        if {![file isdirectory $src]} { error "system '$from_system' not found" }
        if {![_valid_script_name $new_system]} {
            error "invalid system name '$new_system' (letters, digits, _ and - only)"
        }
        if {$new_system eq $from_system} { error "new name equals source name" }
        set dst [file join $::ess::system_path $project $new_system]
        if {[file exists $dst]} { error "system '$new_system' already exists" }

        set sys_canonical [dict create]
        foreach suffix {"" _extract _analyze} {
            dict set sys_canonical ${from_system}${suffix}.tcl ${new_system}${suffix}.tcl
        }
        dict set sys_canonical ${from_system}_viewer.js ${new_system}_viewer.js

        ess::paths::mkdir_matching_owner $dst
        set files {}
        set skipped {}

        foreach f [glob -nocomplain -directory $src *] {
            set tail [file tail $f]
            if {[file isdirectory $f]} {
                # protocol subdir: keep the protocol's own name, transform
                # the system identifier inside its canonical files
                if {[llength $protocols] && [lsearch -exact $protocols $tail] < 0} {
                    lappend skipped "$tail/"
                    continue
                }
                set proto $tail
                set pdst [file join $dst $proto]
                foreach pf [glob -nocomplain -directory $f *] {
                    if {[file isdirectory $pf]} continue
                    set ptail [file tail $pf]
                    set ok 0
                    foreach suffix {"" _loaders _variants _stim _extract} {
                        if {$ptail eq "${proto}${suffix}.tcl"} { set ok 1; break }
                    }
                    if {$ptail eq "${proto}_viewer.js"} { set ok 1 }
                    if {$ok} {
                        if {![file exists $pdst]} {
                            ess::paths::mkdir_matching_owner $pdst
                        }
                        _clone_read_transform_write $pf [file join $pdst $ptail] \
                            $from_system $new_system
                        lappend files $proto/$ptail
                    } else {
                        lappend skipped $proto/$ptail
                    }
                }
            } else {
                if {[dict exists $sys_canonical $tail]} {
                    set newname [dict get $sys_canonical $tail]
                    _clone_read_transform_write $f [file join $dst $newname] \
                        $from_system $new_system
                    lappend files $newname
                } else {
                    lappend skipped $tail
                }
            }
        }
        if {![llength $files]} {
            file delete -force $dst
            error "no canonical system files found in $from_system"
        }
        ess::ess_info "Cloned system $from_system -> $new_system\
            ([llength $files] files)" "scripts"
        return [_clone_result_json clone_system \
            [list from $from_system new $new_system] \
            [lsort $files] [lsort $skipped]]
    }

    # ── Workgroup user roster (registry passthrough) ────────────────
    #
    # Routed through this subprocess (rather than browser → registry
    # directly) so the GUI works even when the browser can't reach the
    # registry host — the rig's outbound HTTPS is the only requirement.
    # Responses are the registry's own JSON, republished on scripts/users.
    #
    proc users {} {
        _require_registry
        set url "$::ess::registry_url/api/v1/ess/users?workgroup=$::ess::registry_workgroup"
        set response [https_get $url]
        catch { dservSet scripts/users $response }
        return $response
    }

    proc user_add {username {fullName ""} {email ""} {role editor}} {
        _require_registry
        if {$username eq ""} { error "username required" }
        set url "$::ess::registry_url/api/v1/ess/user/$::ess::registry_workgroup/$username"
        set body [dict_to_json [dict create username $username \
            fullName $fullName email $email role $role]]
        https_post $url $body
        return [users]
    }

    proc user_remove {username} {
        _require_registry
        if {$username eq ""} { error "username required" }
        set url "$::ess::registry_url/api/v1/ess/user/$::ess::registry_workgroup/$username"
        https_delete $url
        return [users]
    }

    # ── Diff (local vs registry) ────────────────────────────────────
    #
    # Unified diff of the local working copy against the registry HEAD,
    # labeled registry/<relkey> vs local/<relkey> — what a pull would
    # change (reversed, what a push would send).
    #
    proc diff {system protocol type args} {
        _require_registry
        set version ""
        foreach {k v} $args {
            switch -- $k {
                -version { set version $v }
                default  { error "diff: unknown option $k" }
            }
        }
        set project $::ess::current(project)
        set filename [ess::_script_filename $system $protocol $type]
        set relkey [_relkey $protocol $filename]
        set local_file [file join $::ess::system_path $project $system $relkey]

        set api_proto $protocol
        if {$api_proto eq "" || $api_proto eq "_system"} { set api_proto "_" }
        set url "$::ess::registry_url/api/v1/ess/script/$::ess::registry_workgroup/$system/$api_proto/$type"
        if {$version ne ""} { append url "?version=$version" }

        set reg_exists 1
        set reg_content ""
        if {[catch { set response [https_get $url] } err]} {
            if {[string match "HTTP 404*" $err]} {
                set reg_exists 0
            } else {
                error $err
            }
        } else {
            set reg_content [json_get $response content]
        }

        set local_exists [file exists $local_file]
        set local_content ""
        if {$local_exists} {
            set f [open $local_file r]
            set local_content [read $f]
            close $f
        }

        set difftext [_unified_diff $reg_content $local_content \
            "registry/$relkey" "local/$relkey"]

        return [_diff_json $system $protocol $type $relkey \
            $reg_exists $local_exists $difftext]
    }

    proc lib_diff {filename} {
        _require_registry
        if {![regexp {^(.+)-(\d[\d.]*)\.tm$} $filename -> name version]} {
            error "lib_diff: '$filename' is not a name-version.tm lib filename"
        }
        set project $::ess::current(project)
        set local_file [file join $::ess::system_path $project lib $filename]

        set url "$::ess::registry_url/api/v1/ess/lib/$::ess::registry_workgroup/$name/$version"
        set reg_exists 1
        set reg_content ""
        if {[catch { set response [https_get $url] } err]} {
            if {[string match "HTTP 404*" $err]} {
                set reg_exists 0
            } else {
                error $err
            }
        } else {
            set reg_content [json_get $response content]
        }

        set local_exists [file exists $local_file]
        set local_content ""
        if {$local_exists} {
            set f [open $local_file r]
            set local_content [read $f]
            close $f
        }

        set difftext [_unified_diff $reg_content $local_content \
            "registry/lib/$filename" "local/lib/$filename"]

        return [_diff_json "" "" lib "lib/$filename" $reg_exists $local_exists $difftext]
    }

    proc _diff_json {system protocol type relkey reg_exists local_exists difftext} {
        set obj [yajl create #auto]
        $obj map_open
        $obj string system string $system
        $obj string protocol string $protocol
        $obj string type string $type
        $obj string relkey string $relkey
        $obj string registryExists bool $reg_exists
        $obj string localExists bool $local_exists
        $obj string changed bool [expr {$difftext ne ""}]
        $obj string diff string $difftext
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/diff $json }
        return $json
    }

    # Unified diff via the system diff(1) (BSD and GNU both take -u/-L).
    # Exit 0 = identical, 1 = differences, anything else = real error.
    proc _unified_diff {a b la lb} {
        set fa [file tempfile pa scripts_diff_a]
        puts -nonewline $fa $a
        close $fa
        set fb [file tempfile pb scripts_diff_b]
        puts -nonewline $fb $b
        close $fb
        set rc [catch { exec diff -u -L $la -L $lb $pa $pb } out opts]
        file delete $pa $pb
        if {$rc == 0} { return "" }
        set ec [dict get $opts -errorcode]
        if {[lindex $ec 0] eq "CHILDSTATUS" && [lindex $ec 2] == 1} {
            # exec appends its exit-status complaint to captured output
            regsub {\nchild process exited abnormally$} $out {} out
            return $out
        }
        error "diff failed: $out"
    }

    # ── History (registry version history for one script) ───────────
    #
    # The registry keeps full content per saved version. scripts::history
    # returns metadata only (checksum/author/date/comment per version);
    # history_diff fetches content lazily to render an actual diff.
    #
    proc _history_url {system protocol type} {
        set api_proto $protocol
        if {$api_proto eq "" || $api_proto eq "_system"} { set api_proto "_" }
        return "$::ess::registry_url/api/v1/ess/history/$::ess::registry_workgroup/$system/$api_proto/$type"
    }

    proc history {system protocol type {limit 20}} {
        _require_registry
        set response [https_get [_history_url $system $protocol $type]]

        set filename [ess::_script_filename $system $protocol $type]
        set relkey [_relkey $protocol $filename]

        # Response bodies carry script content, which json_to_dict can't
        # digest — use indexed json_get access (ess_sync's idiom).
        set obj [yajl create #auto]
        $obj map_open
        $obj string system string $system
        $obj string protocol string $protocol
        $obj string type string $type
        $obj string relkey string $relkey
        $obj string current
        $obj map_open
        $obj string checksum string [json_get $response script.checksum]
        $obj string updatedAt string [json_get $response script.updatedAt]
        $obj string updatedBy string [json_get $response script.updatedBy]
        $obj map_close
        $obj string history
        $obj array_open
        for {set i 0} {$i < $limit} {incr i} {
            set cs [json_get $response history.$i.checksum]
            if {$cs eq ""} break
            $obj map_open
            $obj string checksum string $cs
            $obj string savedAt string [json_get $response history.$i.savedAt]
            $obj string savedBy string [json_get $response history.$i.savedBy]
            $obj string comment string [json_get $response history.$i.comment]
            $obj map_close
        }
        $obj array_close
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/history $json }
        return $json
    }

    # Diff two historical versions by checksum; with one checksum, diff
    # that version against the current registry HEAD.
    proc history_diff {system protocol type checksum {checksum2 ""}} {
        _require_registry
        set response [https_get [_history_url $system $protocol $type]]

        set filename [ess::_script_filename $system $protocol $type]
        set relkey [_relkey $protocol $filename]

        set content1 ""
        set found1 0
        for {set i 0} {$i < 100} {incr i} {
            set cs [json_get $response history.$i.checksum]
            if {$cs eq ""} break
            if {!$found1 && [string match "$checksum*" $cs]} {
                set content1 [json_get $response history.$i.content]
                set found1 1
                set checksum $cs
            }
            if {$checksum2 ne "" && [string match "$checksum2*" $cs]} {
                set checksum2 $cs
            }
        }
        if {!$found1} { error "history_diff: no version with checksum $checksum" }

        if {$checksum2 eq ""} {
            set content2 [json_get $response script.content]
            set label2 "current"
        } else {
            set content2 ""
            set found2 0
            for {set i 0} {$i < 100} {incr i} {
                set cs [json_get $response history.$i.checksum]
                if {$cs eq ""} break
                if {$cs eq $checksum2} {
                    set content2 [json_get $response history.$i.content]
                    set found2 1
                    break
                }
            }
            if {!$found2} { error "history_diff: no version with checksum $checksum2" }
            set label2 [string range $checksum2 0 7]
        }

        set difftext [_unified_diff $content1 $content2 \
            "$relkey@[string range $checksum 0 7]" "$relkey@$label2"]

        set obj [yajl create #auto]
        $obj map_open
        $obj string system string $system
        $obj string protocol string $protocol
        $obj string type string $type
        $obj string relkey string $relkey
        $obj string from string $checksum
        $obj string to string [expr {$checksum2 eq "" ? "current" : $checksum2}]
        $obj string changed bool [expr {$difftext ne ""}]
        $obj string diff string $difftext
        $obj string ts number [clock seconds]
        $obj map_close
        set json [$obj get]
        $obj delete
        catch { dservSet scripts/history_diff $json }
        return $json
    }
}
