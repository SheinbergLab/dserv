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
    variable registry_checksums [dict create]  ;# last-known registry checksums per script type

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
            foreach type {system extract analyze} {
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
        if {[dict size $checksums] == 0} {
            # dict_to_json serializes empty dict as "" not {}
            set body "{\"checksums\":{},\"version\":\"$version\"}"
        } else {
            set body [dict_to_json [dict create checksums $checksums version $version] -deep]
        }

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
                        mkdir_matching_owner $dir
                    }
                    set f [open $local_file w]
                    puts -nonewline $f $content
                    close $f
                    fix_file_ownership $local_file
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
                    mkdir_matching_owner $lib_dir
                }

                set f [open $local_file w]
                puts -nonewline $f $content
                close $f
                fix_file_ownership $local_file
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

        # Map to registry API type and protocol
        lassign [_registry_type_mapping $type $current(protocol)] api_type api_protocol

        set overlay_user [file tail $overlay_path]
        set system $current(system)

        set url "${registry_url}/api/v1/ess/script/${registry_workgroup}/${system}/${api_protocol}/${api_type}"

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
                        mkdir_matching_owner $dir
                    }
                    set f [open $overlay_file w]
                    puts -nonewline $f $content
                    close $f
                    fix_file_ownership $overlay_file
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

    # ── Commit a single base script to registry ────────────────────
    #
    # Pushes a promoted (base) script to the registry as the main version.
    # This is the "publish" step after promote_overlay → base.
    #
    # type: system, protocol, loaders, variants, stim, etc.
    # comment: optional commit message
    #
    proc commit_script {type {comment ""}} {
        variable system_path
        variable overlay_path
        variable registry_url
        variable registry_workgroup
        variable registry_checksums
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured"
        }
        if {$current(system) eq ""} {
            error "No system loaded"
        }

        # Read from base, not overlay
        set relpath [get_script_relpath $type]
        if {$relpath eq ""} {
            error "Unknown or unavailable script type: $type"
        }
        set base_file [file join $system_path $relpath]

        if {![file exists $base_file]} {
            error "No base file for $type: $base_file"
        }

        set f [open $base_file r]
        set content [read $f]
        close $f

        # Map to registry API type and protocol
        lassign [_registry_type_mapping $type $current(protocol)] api_type api_protocol

        # Identify who is committing
        set user ""
        if {$overlay_path ne ""} {
            set user [file tail $overlay_path]
        } elseif {[info exists ::env(USER)]} {
            set user $::env(USER)
        }

        # Check user's role before attempting commit
        if {$user ne ""} {
            set role [_get_user_role $user]
            if {$role eq "viewer"} {
                error "User '$user' has role 'viewer' and cannot commit to registry"
            }
        }

        if {$comment eq ""} {
            set comment "committed from dserv"
        }

        set system $current(system)
        set url "${registry_url}/api/v1/ess/script/${registry_workgroup}/${system}/${api_protocol}/${api_type}"

        # Use stored registry checksum for optimistic locking.
        # If we have one from a prior sync_status, the server will reject
        # the commit if someone else modified the script in between.
        # Empty string skips the check (first commit or no prior status).
        set expected ""
        if {[dict exists $registry_checksums $type]} {
            set expected [dict get $registry_checksums $type]
        }

        set body [dict_to_json [dict create \
            content $content \
            updatedBy $user \
            comment $comment \
            expectedChecksum $expected]]

        if {[catch {
            set response [https_put $url $body]
        } err]} {
            ess_error "Failed to commit $type to registry: $err" "sync"
            error "Commit failed: $err"
        }

        # Update stored checksum to what the server now has
        # (the checksum of the content we just pushed)
        if {[catch {
            set new_checksum [json_get $response checksum]
            if {$new_checksum ne ""} {
                dict set registry_checksums $type $new_checksum
            }
        }]} {
            # If we can't parse the response checksum, clear it
            # so next commit won't send a stale expected value
            dict set registry_checksums $type ""
        }

        ess_info "Committed $type to registry ($registry_workgroup/$system)" "sync"
        return "success"
    }

    # ── Commit all scripts for current system to registry ─────────
    #
    # Pushes all base scripts that exist for the current system+protocol.
    #
    proc commit_system {{comment ""}} {
        variable current

        if {$current(system) eq ""} {
            error "No system loaded"
        }

        set committed {}
        set errors {}

        foreach type {system protocol loaders variants stim sys_extract sys_analyze proto_extract} {
            set relpath [get_script_relpath $type]
            if {$relpath eq ""} continue

            set base_file [file join $::ess::system_path $relpath]
            if {![file exists $base_file]} continue

            if {[catch {
                commit_script $type $comment
                lappend committed $type
            } err]} {
                lappend errors "$type: $err"
                ess_warning "Failed to commit $type: $err" "sync"
            }
        }

        if {[llength $errors] > 0} {
            ess_warning "Committed [llength $committed], [llength $errors] error(s)" "sync"
        } else {
            ess_info "Committed [llength $committed] script(s) to registry" "sync"
        }

        return [dict create committed $committed errors $errors]
    }

    # ── Check sync status for current system ──────────────────────
    #
    # Compares local base checksums against registry.
    # Returns dict of type → {status checksum registry_checksum}
    #   status: synced, modified, local_only, registry_only
    #
    proc sync_status {} {
        variable system_path
        variable registry_url
        variable registry_workgroup
        variable registry_checksums
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$current(system) eq ""} {
            error "No system loaded"
        }

        set system $current(system)
        set project $current(project)
        set result [dict create]

        foreach type {system protocol loaders variants stim sys_extract sys_analyze proto_extract} {
            set relpath [get_script_relpath $type]
            if {$relpath eq ""} continue

            set base_file [file join $system_path $relpath]
            if {![file exists $base_file]} continue

            set local_checksum [sha256 -file $base_file]

            # Map to registry API type and protocol
            lassign [_registry_type_mapping $type $current(protocol)] api_type api_protocol

            set url "${registry_url}/api/v1/ess/script/${registry_workgroup}/${system}/${api_protocol}/${api_type}"

            if {[catch {
                set response [https_get $url]
                set remote_checksum [json_get $response checksum]

                # Store registry checksum for optimistic locking on commit
                dict set registry_checksums $type $remote_checksum

                if {$local_checksum eq $remote_checksum} {
                    dict set result $type synced
                } else {
                    dict set result $type modified
                }
            } err]} {
                # 404 means not on registry yet
                dict set registry_checksums $type ""
                dict set result $type local_only
            }
        }

        return $result
    }

    # ── List local lib modules ──────────────────────────────────────
    #
    # Returns a JSON array of {name, version, filename} for all .tm
    # files in the lib directory. Delegates to get_lib_files for the
    # file listing (defined in ess-2.0.tm).
    #
    proc list_libs {} {
        set files [get_lib_files]

        set result [list]
        foreach filename $files {
            set match [regexp {^(.+)-(\d+[\.\d]*)\.tm$} $filename -> name version]
            if {$match} {
                lappend result "{\"name\":\"$name\",\"version\":\"$version\",\"filename\":\"$filename\"}"
            } else {
                set name [file rootname $filename]
                lappend result "{\"name\":\"$name\",\"version\":\"\",\"filename\":\"$filename\"}"
            }
        }

        return "\[[join $result ,]\]"
    }

    # ── Read a lib file ──────────────────────────────────────────────
    #
    # Returns content of a lib .tm file.
    # Checks overlay path first if active, falls back to base via
    # get_lib_file_content (defined in ess-2.0.tm).
    #
    proc read_lib {filename} {
        variable overlay_path
        variable current

        # Check overlay first
        if {$overlay_path ne ""} {
            set project $current(project)
            set overlay_file [file join $overlay_path $project lib $filename]
            if {[file exists $overlay_file]} {
                set f [open $overlay_file r]
                set content [read $f]
                close $f
                return $content
            }
        }

        # Fall back to base (with validation)
        return [get_lib_file_content $filename]
    }

    # ── Save a lib file ──────────────────────────────────────────────
    #
    # If overlay is active, writes to overlay lib path directly.
    # If no overlay, delegates to save_lib_file (ess-2.0.tm) which
    # handles validation, backup creation, and ownership.
    #
    proc save_lib {filename content} {
        variable system_path
        variable overlay_path
        variable current

        set project $current(project)

        if {$overlay_path ne ""} {
            # Save to overlay
            set dir [file join $overlay_path $project lib]
            set target [file join $dir $filename]

            if {![file exists $dir]} {
                mkdir_matching_owner $dir
            }

            set f [open $target w]
            puts -nonewline $f $content
            close $f
            fix_file_ownership $target

            ess_info "Saved lib $filename to overlay" "sync"
            return "ok"
        }

        # No overlay — save to base via save_lib_file (gets backup + validation)
        return [save_lib_file $filename $content]
    }

    # ── Commit a lib from base to registry ───────────────────────────
    #
    # Reads the base lib file and PUTs it to the registry.
    # Requires the lib to be promoted (in base) first if overlay was active.
    #
    proc commit_lib {filename {comment ""}} {
        variable system_path
        variable overlay_path
        variable registry_url
        variable registry_workgroup
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured"
        }

        set project $current(project)

        # Read from base (not overlay)
        set base_file [file join $system_path $project lib $filename]
        if {![file exists $base_file]} {
            error "No base lib file: $filename"
        }

        set f [open $base_file r]
        set content [read $f]
        close $f

        # Parse name-version from filename
        set name $filename
        set version "1.0"
        if {[regexp {^(.+)-(\d+[\.\d]*)\.tm$} $filename -> n v]} {
            set name $n
            set version $v
        }

        # Identify user
        set user ""
        if {$overlay_path ne ""} {
            set user [file tail $overlay_path]
        } elseif {[info exists ::env(USER)]} {
            set user $::env(USER)
        }

        # Check role
        if {$user ne ""} {
            set role [_get_user_role $user]
            if {$role eq "viewer"} {
                error "User '$user' has role 'viewer' and cannot commit to registry"
            }
        }

        if {$comment eq ""} {
            set comment "committed from dserv"
        }

        set url "${registry_url}/api/v1/ess/lib/${registry_workgroup}/${name}/${version}"

        set body [dict_to_json [dict create \
            content $content \
            updatedBy $user]]

        if {[catch {
            set response [https_put $url $body]
        } err]} {
            ess_error "Failed to commit lib $filename to registry: $err" "sync"
            error "Commit failed: $err"
        }

        ess_info "Committed lib $filename to registry ($registry_workgroup)" "sync"
        return "success"
    }

    # ── Seed/push all local libs to registry ──────────────────────────
    #
    # Reads every .tm file from the local lib/ directory and PUTs it
    # to the registry.  Creates new entries or updates existing ones.
    # Compares checksums first so unchanged files are skipped.
    #
    # Optional workgroup arg overrides the configured workgroup,
    # allowing:  ess::seed_libs _templates   (to re-seed templates)
    #
    # Use -force to push all files regardless of checksum match:
    #   ess::seed_libs -force
    #   ess::seed_libs -force _templates
    #
    # Returns dict: pushed <n> unchanged <n> skipped <n> errors <list>
    #
    proc seed_libs {args} {
        variable system_path
        variable registry_url
        variable registry_workgroup
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }

        # Parse args: optional -force flag and optional workgroup
        set force 0
        set target_workgroup ""
        foreach arg $args {
            if {$arg eq "-force"} {
                set force 1
            } else {
                set target_workgroup $arg
            }
        }

        set wg $registry_workgroup
        if {$target_workgroup ne ""} {
            set wg $target_workgroup
        }
        if {$wg eq ""} {
            error "Workgroup not configured"
        }

        set project $current(project)
        set lib_dir [file join $system_path $project lib]

        if {![file isdirectory $lib_dir]} {
            error "Lib directory not found: $lib_dir"
        }

        # Get current registry checksums for comparison
        set reg_checksums [dict create]
        if {[catch {
            set resp [https_get "${registry_url}/api/v1/ess/libs?workgroup=${wg}"]
            set data [json_to_dict $resp]
            foreach lib [dict get $data libs] {
                set fn [dict get $lib filename]
                dict set reg_checksums $fn [dict get $lib checksum]
            }
        }]} {
            # No existing libs or fetch failed — push everything
        }

        set pushed 0
        set unchanged 0
        set skipped 0
        set errors [list]

        ess_info "Seeding libs from $lib_dir to $wg" "sync"

        foreach f [lsort [glob -nocomplain -directory $lib_dir *.tm]] {
            set filename [file tail $f]

            # Parse name-version.tm
            if {![regexp {^(.+)-([0-9]+[._][0-9]+)\.tm$} $filename -> name version]} {
                ess_debug "  Skipping $filename (doesn't match pattern)" "sync"
                incr skipped
                continue
            }
            # Normalize underscores to dots for registry
            set version [string map {_ .} $version]

            # Compare checksum — skip if unchanged (unless -force)
            set local_checksum [sha256 -file $f]
            # Registry stores filename with dots (planko-3.0.tm)
            set reg_filename "${name}-${version}.tm"
            if {!$force && [dict exists $reg_checksums $reg_filename]} {
                if {$local_checksum eq [dict get $reg_checksums $reg_filename]} {
                    incr unchanged
                    continue
                }
            }

            # Read local content
            if {[catch {
                set fh [open $f r]
                set content [read $fh]
                close $fh
            } read_err]} {
                ess_error "  Failed to read $filename: $read_err" "sync"
                lappend errors "$filename: $read_err"
                continue
            }

            # Identify who is pushing
            set user "seed"
            if {[info exists ::env(USER)]} {
                set user $::env(USER)
            }

            # PUT to registry
            set url "${registry_url}/api/v1/ess/lib/${wg}/${name}/${version}"
            set body [dict_to_json [dict create content $content updatedBy $user]]

            if {[catch {
                set response [https_put $url $body]
                incr pushed
                ess_info "  Pushed: $filename" "sync"
            } put_err]} {
                ess_error "  Failed to push $filename: $put_err" "sync"
                lappend errors "$filename: $put_err"
            }
        }

        ess_info "Seed libs: $pushed pushed, $unchanged unchanged, $skipped skipped" "sync"
        return [dict create pushed $pushed unchanged $unchanged skipped $skipped errors $errors]
    }

    # ── Lib sync status ──────────────────────────────────────────────
    #
    # Compares local lib checksums against registry.
    # Returns dict of {filename status} where status is:
    #   synced, modified, local_only, registry_only
    #
    proc lib_sync_status {} {
        variable system_path
        variable registry_url
        variable registry_workgroup
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }

        set project $current(project)
        set lib_dir [file join $system_path $project lib]
        set result [dict create]

        # Get registry lib list with checksums
        set url "${registry_url}/api/v1/ess/libs?workgroup=${registry_workgroup}"
        if {[catch {
            set response [https_get $url]
            set data [json_to_dict $response]
        } err]} {
            ess_error "Failed to fetch lib list for sync status: $err" "sync"
            return $result
        }

        set server_libs [dict create]
        foreach lib [dict get $data libs] {
            set fname [dict get $lib filename]
            dict set server_libs $fname [dict get $lib checksum]
        }

        # Check local files against server
        if {[file exists $lib_dir]} {
            foreach f [glob -nocomplain -type f [file join $lib_dir *.tm]] {
                set fname [file tail $f]
                set local_checksum [sha256 -file $f]

                if {[dict exists $server_libs $fname]} {
                    set remote_checksum [dict get $server_libs $fname]
                    if {$local_checksum eq $remote_checksum} {
                        dict set result $fname synced
                    } else {
                        dict set result $fname modified
                    }
                    # Remove from server_libs so we can find registry_only later
                    dict unset server_libs $fname
                } else {
                    dict set result $fname local_only
                }
            }
        }

        # Any remaining server libs are registry_only
        dict for {fname checksum} $server_libs {
            dict set result $fname registry_only
        }

        return $result
    }

    # ── Helper: map internal type to registry API type + protocol ────
    #
    # Internal names: sys_extract, sys_analyze, proto_extract
    # Registry API expects: extract, analyze, extract (with protocol)
    #
    proc _registry_type_mapping {type current_protocol} {
        switch $type {
            sys_extract {
                return [list extract "_"]
            }
            sys_analyze {
                return [list analyze "_"]
            }
            proto_extract {
                return [list extract $current_protocol]
            }
            system {
                return [list system "_"]
            }
            default {
                return [list $type $current_protocol]
            }
        }
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

    # ── Helper: look up a user's role from the registry ───────────
    #
    # Returns role string (admin, editor, viewer) or "" if unknown.
    # Failures are non-fatal — returns "" so commit proceeds
    # (server will enforce if needed).
    #
    proc _get_user_role {username} {
        variable registry_url
        variable registry_workgroup

        if {$registry_url eq "" || $registry_workgroup eq ""} {
            return ""
        }

        set url "${registry_url}/api/v1/ess/user/${registry_workgroup}/${username}"

        if {[catch {
            set response [https_get $url]
            set role [json_get $response role]
            return $role
        } err]} {
            ess_debug "Could not fetch role for $username: $err" "sync"
            return ""
        }
    }

    # ── Scaffold: create new protocol ─────────────────────────────
    #
    # Creates a new protocol in the registry by cloning an existing one
    # or generating from a skeleton (if from_protocol is empty).
    # After creation, syncs the system locally so the new files appear.
    #
    # Usage:
    #   ess::scaffold_protocol new_proto -from colormatch
    #   ess::scaffold_protocol new_proto                    ;# skeleton
    #   ess::scaffold_protocol new_proto -from colormatch -system match_to_sample
    #
    # Returns dict: system, protocols, scripts, forkedFrom
    #
    proc scaffold_protocol {new_protocol args} {
        variable registry_url
        variable registry_workgroup
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured (use ess::registry_configure -url ...)"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured (use ess::registry_configure -workgroup ...)"
        }

        # Parse options
        set from_protocol ""
        set system ""
        set description ""
        foreach {key val} $args {
            switch -- $key {
                -from        { set from_protocol $val }
                -system      { set system $val }
                -description { set description $val }
                default      { error "Unknown option: $key (use -from, -system, -description)" }
            }
        }

        # Default to current system
        if {$system eq ""} {
            if {$current(system) eq ""} {
                error "No system specified and no system loaded"
            }
            set system $current(system)
        }

        # Identify user
        set user "scaffold"
        if {[info exists ::env(USER)]} {
            set user $::env(USER)
        }

        # Build request
        set request [dict create \
            workgroup $registry_workgroup \
            system    $system \
            protocol  $new_protocol \
            createdBy $user]

        if {$from_protocol ne ""} {
            dict set request fromProtocol $from_protocol
        }
        if {$description ne ""} {
            dict set request description $description
        }

        set url "${registry_url}/api/v1/ess/scaffold/protocol"
        set body [dict_to_json $request]

        ess_info "Scaffolding protocol $new_protocol in $system (from: [expr {$from_protocol ne {} ? $from_protocol : {skeleton}}])" "scaffold"

        if {[catch {
            set response [https_post $url $body]
        } err]} {
            ess_error "Scaffold failed: $err" "scaffold"
            error "Scaffold failed: $err"
        }

        # Parse response
        set success [json_get $response success]
        if {$success ne "true"} {
            set errmsg [json_get $response error]
            ess_error "Scaffold failed: $errmsg" "scaffold"
            error "Scaffold failed: $errmsg"
        }

        set result_system  [json_get $response result.system]
        set result_scripts [json_get $response result.scripts]
        set result_forked  [json_get $response result.forkedFrom]

        ess_info "Created protocol $new_protocol ($result_scripts scripts, forked from $result_forked)" "scaffold"

        # Sync locally so the new files appear on disk
        ess_info "Syncing $system to pull new protocol files..." "scaffold"
        set sync_result [sync_system $system]

        return [dict create \
            system     $result_system \
            protocol   $new_protocol \
            scripts    $result_scripts \
            forkedFrom $result_forked \
            sync       $sync_result]
    }

    # ── Scaffold: delete protocol ─────────────────────────────────
    #
    # Removes a protocol and all its scripts from the registry,
    # then removes the local protocol directory.
    #
    # Usage:
    #   ess::delete_protocol testmatch
    #   ess::delete_protocol testmatch -system match_to_sample
    #
    proc delete_protocol {protocol args} {
        variable registry_url
        variable registry_workgroup
        variable system_path
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured"
        }

        set system ""
        foreach {key val} $args {
            switch -- $key {
                -system { set system $val }
                default { error "Unknown option: $key" }
            }
        }

        if {$system eq ""} {
            if {$current(system) eq ""} {
                error "No system specified and no system loaded"
            }
            set system $current(system)
        }

        set request [dict create \
            workgroup $registry_workgroup \
            system    $system \
            protocol  $protocol]

        set url "${registry_url}/api/v1/ess/scaffold/protocol"
        set body [dict_to_json $request]

        ess_info "Deleting protocol $protocol from $system" "scaffold"

        if {[catch {
            set response [https_delete $url $body]
        } err]} {
            ess_error "Delete failed: $err" "scaffold"
            error "Delete failed: $err"
        }

        set deleted [json_get $response deleted]
        ess_info "Deleted protocol $protocol from registry ($deleted scripts removed)" "scaffold"

        # Remove local protocol directory
        set project $current(project)
        set proto_dir [file join $system_path $project $system $protocol]
        if {[file exists $proto_dir]} {
            file delete -force $proto_dir
            ess_info "Removed local directory $proto_dir" "scaffold"
        }

        return [dict create protocol $protocol deleted $deleted]
    }

    # ── Scaffold: delete system ───────────────────────────────────
    #
    # Removes a system and all its scripts from the registry,
    # then removes the local system directory.
    #
    # Usage:
    #   ess::delete_system prf
    #
    proc delete_system {system} {
        variable registry_url
        variable registry_workgroup
        variable system_path
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured"
        }

        set request [dict create \
            workgroup $registry_workgroup \
            system    $system]

        set url "${registry_url}/api/v1/ess/scaffold/system"
        set body [dict_to_json $request]

        ess_info "Deleting system $system" "scaffold"

        if {[catch {
            set response [https_delete $url $body]
        } err]} {
            ess_error "Delete system failed: $err" "scaffold"
            error "Delete system failed: $err"
        }

        set deleted_scripts [json_get $response deletedScripts]
        ess_info "Deleted system $system from registry ($deleted_scripts scripts removed)" "scaffold"

        # Remove local system directory
        set project $current(project)
        set sys_dir [file join $system_path $project $system]
        if {[file exists $sys_dir]} {
            file delete -force $sys_dir
            ess_info "Removed local directory $sys_dir" "scaffold"
        }

        return [dict create system $system deletedScripts $deleted_scripts]
    }

    # ── Delete script: remove a single script from the registry ────
    #
    # Removes one script (by type) from the registry and optionally
    # removes the local base-layer file.
    #
    # Usage:
    #   ess::delete_script protocol          ;# delete current protocol script
    #   ess::delete_script stim              ;# delete stim script
    #   ess::delete_script extract -protocol mymatch  ;# specific protocol extract
    #   ess::delete_script system -system prf         ;# explicit system
    #
    proc delete_script {type args} {
        variable registry_url
        variable registry_workgroup
        variable registry_checksums
        variable system_path
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured"
        }
        if {$current(system) eq ""} {
            error "No system loaded"
        }

        # Parse optional overrides
        set system $current(system)
        set protocol ""
        foreach {key val} $args {
            switch -- $key {
                -system   { set system $val }
                -protocol { set protocol $val }
                default   { error "Unknown option: $key (use -system or -protocol)" }
            }
        }

        # Map local type name to registry API type and protocol
        lassign [_registry_type_mapping $type $current(protocol)] api_type api_protocol

        # Allow explicit -protocol to override
        if {$protocol ne ""} {
            set api_protocol $protocol
        }

        set url "${registry_url}/api/v1/ess/script/${registry_workgroup}/${system}/${api_protocol}/${api_type}"

        ess_info "Deleting script $type ($api_type) from $system/$api_protocol" "sync"

        if {[catch {
            set response [https_delete $url ""]
        } err]} {
            ess_error "Delete script failed: $err" "sync"
            error "Delete script failed: $err"
        }

        # Clear cached checksum for this type
        if {[dict exists $registry_checksums $type]} {
            dict unset registry_checksums $type
        }

        # Remove local base file if it exists
        set relpath [get_script_relpath $type]
        if {$relpath ne ""} {
            set base_file [file join $system_path $relpath]
            if {[file exists $base_file]} {
                file delete $base_file
                ess_info "Removed local file $base_file" "sync"
            }
        }

        ess_info "Deleted script $api_type from registry" "sync"
        return [dict create type $api_type protocol $api_protocol deleted 1]
    }


    # ── Scaffold: list available protocols to clone ───────────────
    #
    # Returns info about what's available for scaffolding in a system.
    #
    # Usage:
    #   ess::scaffold_info                              ;# current system
    #   ess::scaffold_info -system match_to_sample
    #
    proc scaffold_info {args} {
        variable registry_url
        variable registry_workgroup
        variable current

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }

        set system ""
        foreach {key val} $args {
            switch -- $key {
                -system { set system $val }
                default { error "Unknown option: $key" }
            }
        }

        if {$system eq ""} {
            if {$current(system) ne ""} {
                set system $current(system)
            }
        }

        if {$system eq ""} {
            # No system — return templates only
            set url "${registry_url}/api/v1/ess/scaffold/info/"
        } else {
            set url "${registry_url}/api/v1/ess/scaffold/info/${registry_workgroup}/${system}"
        }

        if {[catch {
            set response [https_get $url]
            set data [json_to_dict $response]
        } err]} {
            ess_error "Failed to get scaffold info: $err" "scaffold"
            error "Failed to get scaffold info: $err"
        }

        return $data
    }

    # ── Scaffold: create new system ───────────────────────────────
    #
    # Creates a new system in the registry by cloning an existing one,
    # from a template, or from a built-in skeleton.
    #
    # Usage:
    #   ess::scaffold_system my_task -from match_to_sample
    #   ess::scaffold_system my_task -template match_to_sample
    #   ess::scaffold_system my_task -protocol first_proto    ;# skeleton
    #
    # Returns dict: system, protocols, scripts, forkedFrom, sync
    #
    proc scaffold_system {new_system args} {
        variable registry_url
        variable registry_workgroup

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured"
        }

        # Parse options
        set from_system ""
        set from_workgroup ""
        set template ""
        set protocol ""
        set description ""
        foreach {key val} $args {
            switch -- $key {
                -from          { set from_system $val }
                -from_workgroup { set from_workgroup $val }
                -template      { set template $val }
                -protocol      { set protocol $val }
                -description   { set description $val }
                default        { error "Unknown option: $key" }
            }
        }

        set user "scaffold"
        if {[info exists ::env(USER)]} {
            set user $::env(USER)
        }

        set request [dict create \
            workgroup $registry_workgroup \
            system    $new_system \
            createdBy $user]

        if {$from_system ne ""} {
            dict set request fromSystem $from_system
        }
        if {$from_workgroup ne ""} {
            dict set request fromWorkgroup $from_workgroup
        }
        if {$template ne ""} {
            dict set request template $template
        }
        if {$protocol ne ""} {
            dict set request protocol $protocol
        }
        if {$description ne ""} {
            dict set request description $description
        }

        set url "${registry_url}/api/v1/ess/scaffold/system"
        set body [dict_to_json $request]

        set source "skeleton"
        if {$from_system ne ""} {
            set source $from_system
        } elseif {$template ne ""} {
            set source "template:$template"
        }

        ess_info "Scaffolding system $new_system (from: $source)" "scaffold"

        if {[catch {
            set response [https_post $url $body]
        } err]} {
            ess_error "Scaffold system failed: $err" "scaffold"
            error "Scaffold system failed: $err"
        }

        set success [json_get $response success]
        if {$success ne "true"} {
            set errmsg [json_get $response error]
            error "Scaffold system failed: $errmsg"
        }

        set result_system  [json_get $response result.system]
        set result_scripts [json_get $response result.scripts]
        set result_forked  [json_get $response result.forkedFrom]

        ess_info "Created system $new_system ($result_scripts scripts)" "scaffold"

        # Sync locally
        ess_info "Syncing $new_system to pull files..." "scaffold"
        set sync_result [sync_system $new_system]

        return [dict create \
            system     $result_system \
            scripts    $result_scripts \
            forkedFrom $result_forked \
            sync       $sync_result]
    }
}
