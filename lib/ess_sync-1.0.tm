#
# ess_sync-1.0.tm - Base layer synchronization with central registry
#
# Provides efficient sync of system_path (base layer) from the central
# registry server. Uses SHA256 checksums to pull only changed files.
#
# Usage:
#   package require ess_sync
#   ess::registry_configure -url https://server:port -workgroup mylab
#   ess::sync_base
#

package provide ess_sync 1.0
#
# Usage:
#   ess::sync_system match_to_sample   ;# sync one system
#   ess::sync_base                      ;# sync all systems in project
#
# Requires:
#   - https_get / https_post (TclHttps)
#   - json_to_dict / dict_to_json (tcljson)
#   - sha256 -file (TclSha256)
#   - Registry URL and workgroup configured
#

namespace eval ess {

    variable registry_url {}
    variable registry_workgroup {}

    if {[info exists ::env(ESS_REGISTRY_URL)]} {
        set registry_url $::env(ESS_REGISTRY_URL)
    }
    if {[info exists ::env(ESS_WORKGROUP)]} {
        set registry_workgroup $::env(ESS_WORKGROUP)
    }

    # Configure registry connection
    proc registry_configure {args} {
        variable registry_url
        variable registry_workgroup
        foreach {key val} $args {
            switch -- $key {
                -url       { set registry_url $val }
                -workgroup { set registry_workgroup $val }
                default    { error "Unknown option: $key (use -url or -workgroup)" }
            }
        }
        dservSet ess/registry/url $registry_url
        dservSet ess/registry/workgroup $registry_workgroup
    }

    # ── Single-system sync using POST /sync endpoint ──────────────────
    #
    # Sends local checksums to server, receives back only stale scripts
    # with content. Single round trip — most efficient for startup sync.
    #
    # Returns dict: pulled <n> unchanged <n> errors <list>
    #
    proc sync_system {system {version "main"}} {
        variable system_path
        variable registry_url
        variable registry_workgroup
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured (use ess::registry_configure -url ...)"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured (use ess::registry_configure -workgroup ...)"
        }

        set project $current(project)
        set pulled 0
        set unchanged 0
        set errors [list]

        ess_info "Syncing $system from $registry_workgroup (version: $version)" "sync"

        # Step 1: Build local checksum map
        set checksums [dict create]
        set sys_dir [file join $system_path $project $system]

        if {[file exists $sys_dir]} {
            # System-level scripts
            foreach type {system extract} {
                set filename [_script_filename $system "" $type]
                set filepath [file join $sys_dir $filename]
                if {[file exists $filepath]} {
                    dict set checksums "_system/$type" [sha256 -file $filepath]
                }
            }

            # Protocol-level scripts
            foreach proto_dir [glob -nocomplain -type d [file join $sys_dir *]] {
                set proto [file tail $proto_dir]
                foreach type {protocol loaders variants stim extract} {
                    set filename [_script_filename $system $proto $type]
                    set filepath [file join $proto_dir $filename]
                    if {[file exists $filepath]} {
                        dict set checksums "$proto/$type" [sha256 -file $filepath]
                    }
                }
            }
        }

        # Step 2: POST checksums to server
        set url "${registry_url}/api/v1/ess/sync/${registry_workgroup}/${system}"
        set body [dict_to_json [dict create checksums $checksums version $version] -deep]

        if {[catch {
            set response [https_post $url $body]
        } err]} {
            ess_error "Sync request failed for $system: $err" "sync"
            return [dict create pulled 0 unchanged 0 errors [list "sync request: $err"]]
        }

        # Use json_get for field extraction — json_to_dict breaks on
        # script content containing complex Tcl code with braces/spaces
        set unchanged [json_get $response unchanged]

        # Step 3: Write stale files to base
        # Get stale count from json_to_dict (top-level list length is safe)
        set stale_type [json_type $response stale]
        if {$stale_type eq "array"} {
            # Iterate by index using json_get for each field
            for {set i 0} {1} {incr i} {
                set filename [json_get $response stale.$i.filename]
                if {$filename eq ""} break

                set protocol [json_get $response stale.$i.protocol]
                set content  [json_get $response stale.$i.content]

                if {$protocol eq ""} {
                    set relpath [file join $project $system $filename]
                } else {
                    set relpath [file join $project $system $protocol $filename]
                }

                set local_file [file join $system_path $relpath]

                if {[catch {
                    set dir [file dirname $local_file]
                    if {![file exists $dir]} {
                        file mkdir $dir
                    }
                    set f [open $local_file w]
                    puts -nonewline $f $content
                    close $f
                    incr pulled
                    ess_info "  Pulled: $relpath" "sync"
                } write_err]} {
                    ess_error "  Failed to write $relpath: $write_err" "sync"
                    lappend errors "$relpath: $write_err"
                }
            }
        }

        # Step 4: Report extra local files (client has, server doesn't)
        set extra_type [json_type $response extra]
        if {$extra_type eq "array"} {
            for {set i 0} {1} {incr i} {
                set extra_key [json_get $response extra.$i]
                if {$extra_key eq ""} break
                ess_info "  Extra local file (not on server): $extra_key" "sync"
            }
        }

        ess_info "Sync $system: $pulled pulled, $unchanged unchanged" "sync"
        return [dict create pulled $pulled unchanged $unchanged errors $errors]
    }

    # ── Sync shared libs ──────────────────────────────────────────────
    #
    # Compares local $system_path/$project/lib/*.tm against server's
    # ess_libs table by checksum. Pulls only changed files.
    #
    # Returns dict: pulled <n> unchanged <n> errors <list>
    #
    proc sync_libs {} {
        variable system_path
        variable registry_url
        variable registry_workgroup
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }

        set project $current(project)
        set lib_dir [file join $system_path $project lib]
        set pulled 0
        set unchanged 0
        set errors [list]

        ess_info "Syncing libs for $registry_workgroup" "sync"

        # Step 1: Get lib list with checksums (no content)
        set url "${registry_url}/api/v1/ess/libs?workgroup=${registry_workgroup}"

        if {[catch {
            set response [https_get $url]
            set data [json_to_dict $response]
        } err]} {
            ess_error "Failed to fetch lib list: $err" "sync"
            return [dict create pulled 0 unchanged 0 errors [list "lib list: $err"]]
        }

        set libs [dict get $data libs]

        # Step 2: Compare checksums and pull stale
        foreach lib $libs {
            set filename [dict get $lib filename]
            set server_checksum [dict get $lib checksum]
            set name [dict get $lib name]
            set version [dict get $lib version]
            set local_file [file join $lib_dir $filename]

            # Compare
            set local_checksum ""
            if {[file exists $local_file]} {
                set local_checksum [sha256 -file $local_file]
            }

            if {$local_checksum eq $server_checksum} {
                incr unchanged
                continue
            }

            # Pull full lib content
            set lib_url "${registry_url}/api/v1/ess/lib/${registry_workgroup}/${name}/${version}"

            if {[catch {
                set lib_response [https_get $lib_url]
                set lib_data [json_to_dict $lib_response]
                set content [dict get $lib_data content]

                # Ensure lib directory exists
                if {![file exists $lib_dir]} {
                    file mkdir $lib_dir
                }

                set f [open $local_file w]
                puts -nonewline $f $content
                close $f
                incr pulled
                ess_info "  Pulled lib: $filename" "sync"
            } pull_err]} {
                ess_error "  Failed to pull lib $filename: $pull_err" "sync"
                lappend errors "$filename: $pull_err"
            }
        }

        ess_info "Libs sync: $pulled pulled, $unchanged unchanged" "sync"
        return [dict create pulled $pulled unchanged $unchanged errors $errors]
    }

    # ── Sync all systems in workgroup ─────────────────────────────────
    #
    # Fetches workgroup manifest to discover systems, then syncs each.
    # Also syncs shared libs.
    #
    proc sync_base {{version "main"}} {
        variable registry_url
        variable registry_workgroup

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }

        set total_pulled 0
        set total_unchanged 0
        set all_errors [list]

        ess_info "Syncing all systems for $registry_workgroup" "sync"

        # Sync shared libs first (systems may depend on them)
        set lib_result [sync_libs]
        incr total_pulled    [dict get $lib_result pulled]
        incr total_unchanged [dict get $lib_result unchanged]
        foreach e [dict get $lib_result errors] {
            lappend all_errors "libs: $e"
        }

        # Get workgroup manifest to discover system names
        set url "${registry_url}/api/v1/ess/manifest/${registry_workgroup}?version=${version}"

        if {[catch {
            set response [https_get $url]
            set data [json_to_dict $response]
        } err]} {
            error "Failed to fetch workgroup manifest: $err"
        }

        foreach sys_manifest [dict get $data systems] {
            set sys_name [dict get $sys_manifest system]

            set result [sync_system $sys_name $version]

            incr total_pulled    [dict get $result pulled]
            incr total_unchanged [dict get $result unchanged]
            foreach e [dict get $result errors] {
                lappend all_errors "$sys_name: $e"
            }
        }

        set nerr [llength $all_errors]
        ess_info "Full sync: $total_pulled pulled, $total_unchanged unchanged, $nerr errors" "sync"

        return [dict create \
            pulled $total_pulled \
            unchanged $total_unchanged \
            errors $all_errors]
    }

    # ── Push overlay file to server sandbox ───────────────────────────
    #
    # After saving locally, push the overlay file to the registry so
    # it persists for cross-machine roaming.
    #
    proc push_overlay {type} {
        variable overlay_path
        variable registry_url
        variable registry_workgroup
        variable current

        if {$overlay_path eq ""} {
            error "No overlay user set"
        }
        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }

        set relpath [get_script_relpath $type]
        set overlay_file [file join $overlay_path $relpath]

        if {![file exists $overlay_file]} {
            error "No overlay file for $type"
        }

        # Read content
        set f [open $overlay_file r]
        set content [read $f]
        close $f

        # Determine protocol for URL path
        set protocol $current(protocol)
        if {$type eq "system"} {
            set protocol "_"
        }

        set overlay_user [file tail $overlay_path]
        set system $current(system)

        set url "${registry_url}/api/v1/ess/script/${registry_workgroup}/${system}/${protocol}/${type}"

        set body [dict_to_json [dict create \
            content $content \
            updatedBy $overlay_user \
            comment "pushed from overlay"]]

        if {[catch {
            set response [https_put $url $body]
        } err]} {
            ess_error "Failed to push $type to registry: $err" "sync"
            error "Push failed: $err"
        }

        ess_info "Pushed $type to registry ($registry_workgroup/$system)" "sync"
        return "success"
    }

    # ── Pull overlay from server sandbox ──────────────────────────────
    #
    # Pull a user's sandbox files from the server into the local overlay.
    # Used when switching machines or after fresh boot with overlay user set.
    #
    proc pull_overlay {{version ""}} {
        variable overlay_path
        variable system_path
        variable registry_url
        variable registry_workgroup
        variable current

        if {$overlay_path eq ""} {
            error "No overlay user set"
        }
        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }

        set overlay_user [file tail $overlay_path]
        if {$version eq ""} {
            set version $overlay_user
        }

        set system $current(system)
        set project $current(project)
        set pulled 0

        ess_info "Pulling overlay for $overlay_user ($system@$version)" "sync"

        # Get the sandbox version's scripts
        set url "${registry_url}/api/v1/ess/scripts/${registry_workgroup}/${system}?version=${version}"

        if {[catch {
            set response [https_get $url]
            set data [json_to_dict $response]
        } err]} {
            # No sandbox exists — that's fine, nothing to pull
            ess_info "No sandbox found for $overlay_user/$system (may not exist yet)" "sync"
            return [dict create pulled 0]
        }

        # Write each script to the overlay directory
        set scripts [dict get $data scripts]
        dict for {group script_list} $scripts {
            foreach script $script_list {
                set protocol [dict get $script protocol]
                set filename [dict get $script filename]
                set content  [dict get $script content]

                if {$protocol eq ""} {
                    set relpath [file join $project $system $filename]
                } else {
                    set relpath [file join $project $system $protocol $filename]
                }

                set overlay_file [file join $overlay_path $relpath]

                if {[catch {
                    set dir [file dirname $overlay_file]
                    if {![file exists $dir]} {
                        file mkdir $dir
                    }
                    set f [open $overlay_file w]
                    puts -nonewline $f $content
                    close $f
                    incr pulled
                    ess_info "  Pulled overlay: $relpath" "sync"
                } write_err]} {
                    ess_error "  Failed to write overlay $relpath: $write_err" "sync"
                }
            }
        }

        ess_info "Pulled $pulled overlay files for $overlay_user" "sync"
        return [dict create pulled $pulled]
    }

    # ── Helper: map script type to filename ───────────────────────────
    proc _script_filename {system protocol type} {
        if {$protocol eq ""} {
            switch $type {
                system  { return "${system}.tcl" }
                extract { return "${system}_extract.tcl" }
                default { return "${system}_${type}.tcl" }
            }
        } else {
            switch $type {
                protocol { return "${protocol}.tcl" }
                default  { return "${protocol}_${type}.tcl" }
            }
        }
    }
}
