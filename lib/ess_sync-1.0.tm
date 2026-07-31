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
    variable sync_displaced [list]             ;# files displaced by sync (local edits overwritten)
    variable viewers_dir {}                    ;# directory for viewer plugins (served at /viewers/)

    # ── Workgroup manifest cache ─────────────────────────────────────
    #
    # GET /api/v1/ess/manifest/<wg> returns every system's script
    # checksums plus the lib checksums -- everything both the pull and
    # push previews need. Fetching it once turns a 14-request preview
    # into a single request and lets a push preview for any system run
    # with no network at all. Operations that change registry state
    # invalidate it; the TTL bounds staleness for a long-open dialog.
    #
    variable manifest_cache {}
    variable manifest_cache_key {}
    variable manifest_cache_time 0
    variable manifest_cache_ttl 60000

    # ── Sync-displaced file management ───────────────────────────────
    #
    # When sync_system overwrites a local base file that differs from
    # the registry, the local version is saved to a "sync_displaced"
    # directory.  This protects promoted overlay work that hasn't been
    # pushed to registry yet.
    #
    # The displaced list is published as a datapoint so the workbench
    # can warn the user.
    #

    proc _sync_displace_file {local_file relpath} {
        variable system_path
        variable sync_displaced

        if {![file exists $local_file]} return

        # Build displaced directory under system_path
        set displaced_dir [file join $system_path .sync_displaced]
        if {![file exists $displaced_dir]} {
            ess::paths::mkdir_matching_owner $displaced_dir
        }

        # Timestamp + flatten relpath for the backup filename
        set timestamp [clock format [clock seconds] -format "%Y%m%d_%H%M%S"]
        set flat_name [string map {/ _} $relpath]
        set displaced_file [file join $displaced_dir "${timestamp}_${flat_name}"]

        if {[catch {file copy $local_file $displaced_file} err]} {
            ess_warning "Could not save displaced file $relpath: $err" "sync"
            return
        }
        ess::paths::fix_file_ownership $displaced_file

        # Write metadata alongside the displaced file
        if {[catch {
            set mf [open "${displaced_file}.meta" w]
            puts $mf "# Sync-displaced file"
            puts $mf "original_path: $local_file"
            puts $mf "relpath: $relpath"
            puts $mf "displaced_at: [clock format [clock seconds] -format "%Y-%m-%d %H:%M:%S"]"
            puts $mf "displaced_timestamp: [clock seconds]"
            puts $mf "file_size: [file size $displaced_file]"
            close $mf
        } merr]} {
            ess_warning "Could not write displaced metadata: $merr" "sync"
        }

        lappend sync_displaced [dict create \
            relpath $relpath \
            displaced_file $displaced_file \
            timestamp [clock seconds] \
            time_formatted [clock format [clock seconds] -format "%Y-%m-%d %H:%M:%S"]]

        ess_warning "Displaced local file: $relpath (saved to [file tail $displaced_file])" "sync"
    }

    # Publish the current displaced list as a JSON datapoint
    proc _publish_sync_displaced {} {
        variable sync_displaced

        if {[llength $sync_displaced] == 0} {
            dservSet ess/sync/displaced "{\"files\":[]}"
            return
        }

        set items [list]
        foreach entry $sync_displaced {
            set relpath [dict get $entry relpath]
            set dfile   [dict get $entry displaced_file]
            set ts      [dict get $entry timestamp]
            set tfmt    [dict get $entry time_formatted]
            lappend items [format {{"relpath":"%s","file":"%s","timestamp":%d,"time":"%s"}} \
                [string map {\" \\\"} $relpath] \
                [string map {\" \\\"} $dfile] \
                $ts $tfmt]
        }
        set json "{\"files\":\[[join $items ,]\]}"
        dservSet ess/sync/displaced $json
    }

    # Query displaced files (callable from workbench)
    proc sync_displaced_list {} {
        variable sync_displaced
        _publish_sync_displaced
        return $sync_displaced
    }

    # Restore a displaced file back to its base location
    proc restore_displaced {displaced_file} {
        variable sync_displaced

        if {![file exists $displaced_file]} {
            error "Displaced file does not exist: $displaced_file"
        }

        # Find the entry in our list
        set found 0
        set new_list [list]
        foreach entry $sync_displaced {
            if {[dict get $entry displaced_file] eq $displaced_file} {
                set found 1
                set relpath [dict get $entry relpath]
            } else {
                lappend new_list $entry
            }
        }

        if {!$found} {
            # Try to recover relpath from metadata
            set meta_file "${displaced_file}.meta"
            if {[file exists $meta_file]} {
                set f [open $meta_file r]
                set meta_content [read $f]
                close $f
                if {[regexp {original_path:\s*(.+)} $meta_content -> orig_path]} {
                    set relpath ""
                    # We have the original path, just copy back
                    file copy -force $displaced_file $orig_path
                    ess::paths::fix_file_ownership $orig_path
                    file delete $displaced_file
                    catch { file delete "${displaced_file}.meta" }
                    ess_info "Restored displaced file to $orig_path" "sync"
                    set sync_displaced $new_list
                    _publish_sync_displaced
                    return "restored"
                }
            }
            error "Displaced file not found in tracking list"
        }

        # Restore to base location
        set base_file [ess::paths::base_path $relpath]
        file copy -force $displaced_file $base_file
        ess::paths::fix_file_ownership $base_file
        file delete $displaced_file
        catch { file delete "${displaced_file}.meta" }

        set sync_displaced $new_list
        _publish_sync_displaced

        ess_info "Restored displaced $relpath to base" "sync"
        return "restored"
    }

    # Dismiss (acknowledge) a displaced file — user reviewed and doesn't need it
    proc dismiss_displaced {displaced_file} {
        variable sync_displaced

        set new_list [list]
        foreach entry $sync_displaced {
            if {[dict get $entry displaced_file] ne $displaced_file} {
                lappend new_list $entry
            }
        }
        set sync_displaced $new_list

        # Don't delete the file — just remove from active list
        # User can manually clean up .sync_displaced/ later
        _publish_sync_displaced
        return "dismissed"
    }

    # Dismiss all displaced files
    proc dismiss_all_displaced {} {
        variable sync_displaced
        set sync_displaced [list]
        _publish_sync_displaced
        return "dismissed"
    }

    # ── Base manifest (sync ancestor tracking) ───────────────────────
    #
    # Each system directory carries a hidden .sync_base.json recording,
    # per file, the checksum the registry had the last time we pulled it
    # (or the checksum we last committed).  This "base" is the merge
    # ancestor: comparing base vs local vs registry lets sync tell apart
    #   - a stale local copy (registry moved; safe to overwrite),
    #   - genuine unpushed local edits (must be preserved), and
    #   - a true conflict (both changed since the base).
    # Without it, sync_system displaced *every* file whose checksum
    # differed from the registry, which both spammed .sync_displaced with
    # backups of merely-stale files and silently clobbered real edits —
    # worst of all after a crash+restart, when the old in-memory checksum
    # cache is gone and sync_base runs cold.
    #
    # The manifest lives with the files it describes, keyed relative to
    # its own directory (matching dservctl's relpath scheme so both tools
    # share the format):
    #   <system_path>/<project>/<system>/.sync_base.json  keys "<proto>/<file>" or "<file>"
    #   <system_path>/<project>/lib/.sync_base.json        keys "<file>"
    #
    # The file is a dotfile, so neither dservctl's scanners nor ess_sync's
    # globs ever treat it as a script.

    variable base_manifest_schema 1

    proc _base_manifest_path {project system} {
        variable system_path
        return [file join $system_path $project $system .sync_base.json]
    }

    proc _base_lib_manifest_path {project} {
        variable system_path
        return [file join $system_path $project lib .sync_base.json]
    }

    # Read a base manifest.  Returns a dict {schemaVersion workgroup
    # defaultVersion entries}.  A missing file, parse error, or workgroup
    # mismatch yields an empty manifest so we degrade to cold-start
    # behavior rather than trusting stale data.
    proc _base_manifest_read {path} {
        variable registry_workgroup
        variable base_manifest_schema
        set empty [dict create \
            schemaVersion  $base_manifest_schema \
            workgroup      $registry_workgroup \
            defaultVersion main \
            entries        [dict create]]

        if {![file exists $path]} { return $empty }

        if {[catch {
            set f [open $path r]
            set raw [read $f]
            close $f
            # Manifest values are hex/strings/ints only (no script bodies),
            # so json_to_dict is safe here.
            set m [json_to_dict $raw]
        } err]} {
            ess_warning "Base manifest unreadable ($path): $err — ignoring" "sync"
            return $empty
        }

        if {[dict exists $m workgroup] &&
            [dict get $m workgroup] ne $registry_workgroup} {
            ess_warning "Base manifest workgroup mismatch in $path\
                ([dict get $m workgroup] != $registry_workgroup) — ignoring" "sync"
            return $empty
        }
        if {![dict exists $m entries]} { dict set m entries [dict create] }
        return $m
    }

    # Minimal JSON string escaping for manual serialization.
    proc _json_str {s} {
        return "\"[string map [list \\ \\\\ \" \\\" \n \\n \r \\r \t \\t] $s]\""
    }

    # Serialize a manifest dict to JSON by hand.  We avoid dict_to_json
    # for the nested entries map because the empty-dict and deep-nesting
    # cases are unreliable (see the empty-checksums workaround below).
    proc _base_manifest_to_json {m} {
        variable base_manifest_schema
        set wg [dict get $m workgroup]
        set dv [dict get $m defaultVersion]
        set items [list]
        dict for {key e} [dict get $m entries] {
            set cs  [dict get $e checksum]
            set ver [expr {[dict exists $e version]  ? [dict get $e version]  : $dv}]
            set at  [expr {[dict exists $e syncedAt]  ? [dict get $e syncedAt]  : 0}]
            set by  [expr {[dict exists $e syncedBy]  ? [dict get $e syncedBy]  : ""}]
            lappend items [format {%s:{"checksum":%s,"version":%s,"syncedAt":%d,"syncedBy":%s}} \
                [_json_str $key] [_json_str $cs] [_json_str $ver] $at [_json_str $by]]
        }
        return [format {{"schemaVersion":%d,"workgroup":%s,"defaultVersion":%s,"entries":{%s}}} \
            $base_manifest_schema [_json_str $wg] [_json_str $dv] [join $items ,]]
    }

    # Atomically write a base manifest (temp + rename).
    #
    # dserv runs as root but the systems tree is user-owned, so written
    # files must be chowned back to the directory owner — otherwise this
    # manifest, which dservctl later writes as the (non-root) user, would
    # be left root-owned and lock dservctl out (root can write user-owned
    # files, but not vice versa). We fix ownership of the temp file
    # *before* the rename so the atomic swap yields an already-correctly-
    # owned manifest with no root-owned window for a crash to freeze.
    proc _base_manifest_write {path m} {
        # PID-tagged temp name so a stale root-owned leftover can't collide
        # with another writer's temp file.
        set tmp "${path}.tmp.[pid]"
        if {[catch {
            set dir [file dirname $path]
            if {![file exists $dir]} { mkdir_matching_owner $dir }
            set f [open $tmp w]
            puts -nonewline $f [_base_manifest_to_json $m]
            close $f
            fix_file_ownership $tmp
            file rename -force $tmp $path
        } err]} {
            catch { file delete $tmp }
            ess_warning "Could not write base manifest $path: $err" "sync"
        }
    }

    proc _base_entry_get {m key} {
        if {[dict exists $m entries $key checksum]} {
            return [dict get $m entries $key checksum]
        }
        return ""
    }

    # Set/replace an entry (manifest passed by name).
    proc _base_entry_set {mvar key checksum version by} {
        upvar 1 $mvar m
        dict set m entries $key [dict create \
            checksum $checksum \
            version  $version \
            syncedAt [clock seconds] \
            syncedBy $by]
    }

    proc _base_entry_unset {mvar key} {
        upvar 1 $mvar m
        if {[dict exists $m entries $key]} { dict unset m entries $key }
    }

    # 3-way decision from three checksums ("" == absent).
    # Returns: unchanged | pull | keep_local | conflict | cold
    proc _base_decide {base local registry} {
        if {$local eq $registry} { return unchanged }
        if {$base eq ""}         { return cold }       ;# no ancestor: ambiguous
        if {$base eq $local}     { return pull }       ;# stale local, registry moved
        if {$base eq $registry}  { return keep_local } ;# local edits, registry unchanged
        return conflict                                 ;# both moved since base
    }

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
        variable viewers_dir
        foreach {key val} $args {
            switch -- $key {
                -url         { set registry_url $val }
                -workgroup   { set registry_workgroup $val }
                -viewers_dir { set viewers_dir $val }
                default      { error "Unknown option: $key (use -url, -workgroup, or -viewers_dir)" }
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

        # Step 1: Build local checksum map.
        #   checksums         server-key ("proto/type") -> checksum (sent to server)
        #   local_relsum      manifest relkey -> local checksum
        #   sentkey_to_relkey server-key -> manifest relkey (to map "extra" back)
        set checksums [dict create]
        set local_relsum [dict create]
        set sentkey_to_relkey [dict create]
        set sys_dir [file join $system_path $project $system]

        if {[file exists $sys_dir]} {
            # System-level scripts
            foreach type {system extract analyze viewer} {
                set filename [_script_filename $system "" $type]
                set filepath [file join $sys_dir $filename]
                if {[file exists $filepath]} {
                    set cs [sha256 -file $filepath]
                    dict set checksums "_system/$type" $cs
                    dict set local_relsum $filename $cs
                    dict set sentkey_to_relkey "_system/$type" $filename
                }
            }

            # Protocol-level scripts
            foreach proto_dir [glob -nocomplain -type d [file join $sys_dir *]] {
                set proto [file tail $proto_dir]
                foreach type {protocol loaders variants stim extract viewer} {
                    set filename [_script_filename $system $proto $type]
                    set filepath [file join $proto_dir $filename]
                    if {[file exists $filepath]} {
                        set cs [sha256 -file $filepath]
                        set relkey [file join $proto $filename]
                        dict set checksums "$proto/$type" $cs
                        dict set local_relsum $relkey $cs
                        dict set sentkey_to_relkey "$proto/$type" $relkey
                    }
                }
            }
        }

        # Load the base manifest for this system (the merge ancestor).
        set manifest_path [_base_manifest_path $project $system]
        set manifest [_base_manifest_read $manifest_path]
        set manifest_dirty 0
        set seen_relkeys [dict create]

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

                set protocol     [json_get $response stale.$i.protocol]
                set content      [json_get $response stale.$i.content]
                set reg_checksum [json_get $response stale.$i.checksum]

                if {$protocol eq ""} {
                    set relkey  $filename
                    set relpath [file join $project $system $filename]
                } else {
                    set relkey  [file join $protocol $filename]
                    set relpath [file join $project $system $protocol $filename]
                }

                set local_file [file join $system_path $relpath]
                dict set seen_relkeys $relkey 1

                # 3-way decision: base (ancestor) vs local vs registry.
                set base_checksum [_base_entry_get $manifest $relkey]
                if {![file exists $local_file]} {
                    # New file from the registry — nothing local to lose.
                    set decision pull
                } else {
                    set decision [_base_decide $base_checksum \
                        [sha256 -file $local_file] $reg_checksum]
                }

                if {$decision eq "keep_local"} {
                    # Genuine unpushed local edits, registry unchanged since
                    # our base — preserve local, do not overwrite. Base
                    # already equals registry, so leave the entry as-is.
                    ess_warning "  Keeping local edits (registry unchanged): $relpath" "sync"
                    continue
                }

                if {$decision eq "conflict" || $decision eq "cold"} {
                    # conflict: both changed since base.  cold: no ancestor,
                    # so we can't tell edits from staleness. Either way,
                    # rescue the local copy before taking the registry's.
                    _sync_displace_file $local_file $relpath
                    if {$decision eq "conflict"} {
                        ess_warning "  CONFLICT (local and registry both changed): $relpath\
                            — local saved to .sync_displaced" "sync"
                    }
                }

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
                    # Base := the registry checksum we just wrote.
                    _base_entry_set manifest $relkey $reg_checksum $version "sync"
                    set manifest_dirty 1
                    ess_info "  Pulled: $relpath" "sync"
                } write_err]} {
                    ess_error "  Failed to write $relpath: $write_err" "sync"
                    lappend errors "$relpath: $write_err"
                }
            }
        }

        # Step 4: Report extra local files (client has, server doesn't),
        # collecting their relkeys so we can exclude them from base seeding.
        set extra_relkeys [dict create]
        set extra_type [json_type $response extra]
        if {$extra_type eq "array"} {
            for {set i 0} {1} {incr i} {
                set extra_key [json_get $response extra.$i]
                if {$extra_key eq ""} break
                if {[dict exists $sentkey_to_relkey $extra_key]} {
                    dict set extra_relkeys [dict get $sentkey_to_relkey $extra_key] 1
                }
                ess_info "  Extra local file (not on server): $extra_key" "sync"
            }
        }

        # Step 5: Seed/refresh base for unchanged files.  Any local file
        # that wasn't stale and isn't local-only matched the registry
        # (that's why it was unchanged), so its local checksum *is* the
        # registry's — record it as the base. This is what makes the very
        # first sync populate the whole manifest, not just changed files.
        dict for {relkey cs} $local_relsum {
            if {[dict exists $seen_relkeys $relkey]}  continue ;# stale: base already set
            if {[dict exists $extra_relkeys $relkey]} continue ;# local-only: no base
            if {[_base_entry_get $manifest $relkey] ne $cs} {
                _base_entry_set manifest $relkey $cs $version "sync"
                set manifest_dirty 1
            }
        }

        # Step 6: Prune base entries for scripts the registry no longer has,
        # so a deletion (by any tool/user) self-heals on the next sync. The
        # registry set this sync = local files we sent that weren't "extra",
        # plus the stale files the server returned (which includes registry
        # files missing locally). Guard on a non-empty set so a degenerate or
        # error response never prunes valid entries.
        set registry_seen [dict create]
        dict for {relkey cs} $local_relsum {
            if {![dict exists $extra_relkeys $relkey]} {
                dict set registry_seen $relkey 1
            }
        }
        foreach relkey [dict keys $seen_relkeys] {
            dict set registry_seen $relkey 1
        }
        if {[dict size $registry_seen] > 0} {
            foreach relkey [dict keys [dict get $manifest entries]] {
                if {![dict exists $registry_seen $relkey]} {
                    _base_entry_unset manifest $relkey
                    set manifest_dirty 1
                }
            }
        }

        if {$manifest_dirty} {
            _base_manifest_write $manifest_path $manifest
        }

        ess_info "Sync $system: $pulled pulled, $unchanged unchanged" "sync"

        # Install viewer plugins to the viewers directory (served by dserv-agent)
        _install_viewers $system

        # Publish displaced files so workbench can warn user
        _publish_sync_displaced

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

        # Load the lib base manifest (keys are bare filenames).
        set manifest_path [_base_lib_manifest_path $project]
        set manifest [_base_manifest_read $manifest_path]
        set manifest_dirty 0
        set registry_libs [dict create]

        # Step 2: Compare checksums and pull stale
        foreach lib $libs {
            set filename [dict get $lib filename]
            set server_checksum [dict get $lib checksum]
            dict set registry_libs $filename 1
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
                # Seed/refresh base for the unchanged lib.
                if {[_base_entry_get $manifest $filename] ne $server_checksum} {
                    _base_entry_set manifest $filename $server_checksum $version "sync"
                    set manifest_dirty 1
                }
                continue
            }

            # 3-way decision before overwriting.
            set base_checksum [_base_entry_get $manifest $filename]
            if {$local_checksum eq ""} {
                set decision pull
            } else {
                set decision [_base_decide $base_checksum $local_checksum $server_checksum]
            }

            if {$decision eq "keep_local"} {
                ess_warning "  Keeping local lib edits (registry unchanged): $filename" "sync"
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

                set lib_relpath [file join $project lib $filename]
                if {$decision eq "conflict" || $decision eq "cold"} {
                    _sync_displace_file $local_file $lib_relpath
                    if {$decision eq "conflict"} {
                        ess_warning "  CONFLICT (local and registry both changed): $filename\
                            — local saved to .sync_displaced" "sync"
                    }
                }

                set f [open $local_file w]
                puts -nonewline $f $content
                close $f
                fix_file_ownership $local_file
                incr pulled
                _base_entry_set manifest $filename $server_checksum $version "sync"
                set manifest_dirty 1
                ess_info "  Pulled lib: $filename" "sync"
            } pull_err]} {
                ess_error "  Failed to pull lib $filename: $pull_err" "sync"
                lappend errors "$filename: $pull_err"
            }
        }

        # Prune base entries for libs the registry no longer has (guard on a
        # non-empty lib list so an empty/error response never prunes).
        if {[llength $libs] > 0} {
            foreach fname [dict keys [dict get $manifest entries]] {
                if {![dict exists $registry_libs $fname]} {
                    _base_entry_unset manifest $fname
                    set manifest_dirty 1
                }
            }
        }

        if {$manifest_dirty} {
            _base_manifest_write $manifest_path $manifest
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

        # Discover system names. Forced fresh: this is the real pull, not a
        # preview, so it must not run off a cached view of the registry.
        set data [_manifest_fetch $version 1]

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

        _manifest_invalidate

        return [dict create \
            pulled $total_pulled \
            unchanged $total_unchanged \
            errors $all_errors]
    }

    # ── Resolve commit/push user ─────────────────────────────────────
    proc _resolve_commit_user {{user_override ""}} {
        variable overlay_path
        if {$user_override ne ""} { return $user_override }
        if {$overlay_path ne ""} { return [file tail $overlay_path] }
        if {[info exists ::env(USER)]} { return $::env(USER) }
        return ""
    }

    proc _local_relkey {protocol filename} {
        if {$protocol eq "" || $protocol eq "_" || $protocol eq "_system"} {
            return $filename
        }
        return [file join $protocol $filename]
    }

    proc _derive_system_script_type {filename} {
        set base [file rootname $filename]
        if {[string match *_extract $base]} { return extract }
        if {[string match *_analyze $base]} { return analyze }
        return system
    }

    proc _derive_protocol_script_type {protocol filename} {
        set base [file rootname $filename]
        if {$base eq $protocol} { return protocol }
        set prefix "${protocol}_"
        if {[string first $prefix $base] == 0} {
            return [string range $base [string length $prefix] end]
        }
        return $base
    }


    proc _sync_project {} {
        variable current
        if {[info exists current(project)] && $current(project) ne ""} {
            return $current(project)
        }
        if {[info exists ::ess::paths::project] && $::ess::paths::project ne ""} {
            return $::ess::paths::project
        }
        return "ess"
    }

    # ── System → shared-lib dependency closure ───────────────────────
    #
    # Push is scoped to a single system, but shared libs live in the
    # project-wide lib/ directory. Offer only the libs the system
    # actually pulls in: scan its scripts for `package require`, keep the
    # names a project lib provides, resolve each to the one .tm file Tcl
    # would load, then recurse through those libs' own requires --
    # planko-3.0 requires thread_pool, which no system names directly.
    #
    proc _lib_versions_by_name {lib_dir} {
        set out [dict create]
        foreach f [glob -nocomplain -directory $lib_dir *.tm] {
            set fname [file tail $f]
            if {![regexp {^(.+)-(\d+[\.\d]*)\.tm$} $fname -> name version]} continue
            dict lappend out $name [list $version $fname]
        }
        return $out
    }

    # Tcl's rule: "version or later, same major" (or an exact match when
    # -exact was given). An empty request takes the highest available.
    proc _lib_resolve_version {versions req exact} {
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

    # Returns a list of {name version exact} triples. Comment-only lines
    # are skipped so documentation examples ("#  package require tilemap")
    # do not register as real dependencies.
    proc _scan_package_requires {path} {
        set found [list]
        if {![file isfile $path]} { return $found }
        if {[catch { set f [open $path r] }]} { return $found }
        set content [read $f]
        close $f

        set re {package\s+require\s+(-exact\s+)?([A-Za-z_][\w:]*)\s*([0-9][\w.]*)?}
        foreach line [split $content \n] {
            if {[string index [string trimleft $line] 0] eq "#"} continue
            foreach {- exact name version} [regexp -all -inline $re $line] {
                lappend found [list $name $version [expr {$exact ne ""}]]
            }
        }
        return $found
    }

    proc _collect_tcl_files {dir} {
        set out [list]
        if {![file isdirectory $dir]} { return $out }
        foreach entry [glob -nocomplain -directory $dir *] {
            set tail [file tail $entry]
            if {[string match .* $tail]} continue
            if {[file isdirectory $entry]} {
                lappend out {*}[_collect_tcl_files $entry]
            } elseif {[string match *.tcl $tail]} {
                lappend out $entry
            }
        }
        return $out
    }

    proc _system_lib_closure {system} {
        variable system_path

        set project [_sync_project]
        set lib_dir [file join $system_path $project lib]
        set by_name [_lib_versions_by_name $lib_dir]
        if {[dict size $by_name] == 0} { return [list] }

        set pending [list]
        foreach f [_collect_tcl_files [file join $system_path $project $system]] {
            lappend pending {*}[_scan_package_requires $f]
        }

        set resolved [dict create]
        set seen [dict create]

        while {[llength $pending] > 0} {
            set req [lindex $pending 0]
            set pending [lrange $pending 1 end]
            lassign $req name version exact

            if {![dict exists $by_name $name]} continue
            set key "$name|$version|$exact"
            if {[dict exists $seen $key]} continue
            dict set seen $key 1

            set fname [_lib_resolve_version [dict get $by_name $name] $version $exact]
            if {$fname eq "" || [dict exists $resolved $fname]} continue
            dict set resolved $fname 1

            lappend pending {*}[_scan_package_requires [file join $lib_dir $fname]]
        }

        return [lsort [dict keys $resolved]]
    }

    # ── Preview JSON emission ────────────────────────────────────────
    #
    # dict_to_json -deep cannot represent a list of dicts: Tcl gives it no
    # way to tell one apart from a flat dict, so it emits the whole list as
    # a single JSON string. Preview payloads are arrays of objects, so they
    # are built with yajl explicitly.
    #
    proc _preview_int {v} {
        if {[string is integer -strict $v]} { return $v }
        return 0
    }

    proc _preview_obj_array {obj items} {
        $obj array_open
        foreach item $items {
            $obj map_open
            dict for {k v} $item {
                $obj string $k string $v
            }
            $obj map_close
        }
        $obj array_close
    }

    proc _preview_str_array {obj items} {
        $obj array_open
        foreach item $items {
            $obj string $item
        }
        $obj array_close
    }

    # Serialize one _sync_*_preview result (the libs group or one system).
    proc _preview_group_json {obj p} {
        $obj map_open
        $obj string to_pull
        _preview_obj_array $obj [dict get $p to_pull]
        $obj string skipped
        _preview_obj_array $obj [dict get $p skipped]
        if {[dict exists $p extra]} {
            $obj string extra
            _preview_str_array $obj [dict get $p extra]
        }
        $obj string unchanged
        $obj number [_preview_int [dict get $p unchanged]]
        if {[dict exists $p error]} {
            $obj string error string [dict get $p error]
        }
        $obj map_close
    }

    proc _api_protocol {protocol} {
        if {$protocol eq "" || $protocol eq "_system"} { return "_" }
        return $protocol
    }

    proc _manifest_invalidate {} {
        variable manifest_cache
        variable manifest_cache_key
        variable manifest_cache_time
        set manifest_cache {}
        set manifest_cache_key {}
        set manifest_cache_time 0
    }

    proc _manifest_fetch {{version "main"} {force 0}} {
        variable registry_url
        variable registry_workgroup
        variable manifest_cache
        variable manifest_cache_key
        variable manifest_cache_time
        variable manifest_cache_ttl

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured"
        }

        set key "${registry_workgroup}|${version}"
        set now [clock milliseconds]

        set usable 0
        if {$manifest_cache ne "" && $manifest_cache_key eq $key} {
            if {($now - $manifest_cache_time) < $manifest_cache_ttl} { set usable 1 }
        }
        if {!$force && $usable} { return $manifest_cache }

        set url "${registry_url}/api/v1/ess/manifest/${registry_workgroup}?version=${version}"
        if {[catch { set response [https_get $url] } err]} {
            error "Failed to fetch workgroup manifest: $err"
        }
        # Safe to parse as a dict: the manifest carries checksums only, not
        # script content (content is what breaks json_to_dict elsewhere).
        if {[catch { set data [json_to_dict $response] } err]} {
            error "Failed to parse workgroup manifest: $err"
        }

        set manifest_cache $data
        set manifest_cache_key $key
        set manifest_cache_time $now
        return $data
    }

    # filename -> checksum for every lib in the workgroup.
    proc _manifest_libs {data} {
        set out [dict create]
        if {![dict exists $data libs]} { return $out }
        foreach lib [dict get $data libs] {
            if {![dict exists $lib filename]} continue
            set cs ""
            catch { set cs [dict get $lib checksum] }
            dict set out [dict get $lib filename] $cs
        }
        return $out
    }

    # Returns [list found sys_entry].
    proc _manifest_find_system {data system} {
        if {[dict exists $data systems]} {
            foreach sys [dict get $data systems] {
                if {![dict exists $sys system]} continue
                if {[dict get $sys system] eq $system} { return [list 1 $sys] }
            }
        }
        return [list 0 [dict create]]
    }

    # relkey -> {protocol type filename checksum} for one manifest entry.
    proc _manifest_scripts_of {sys_entry} {
        set out [dict create]
        if {![dict exists $sys_entry scripts]} { return $out }
        foreach s [dict get $sys_entry scripts] {
            set protocol ""
            set stype ""
            set filename ""
            set checksum ""
            catch { set protocol [dict get $s protocol] }
            catch { set stype    [dict get $s type] }
            catch { set filename [dict get $s filename] }
            catch { set checksum [dict get $s checksum] }
            if {$stype eq "" && $filename eq ""} continue
            if {$filename eq ""} { set filename $stype }
            dict set out [_local_relkey $protocol $filename] [dict create \
                protocol $protocol type $stype filename $filename checksum $checksum]
        }
        return $out
    }

    proc _sync_libs_preview {server_libs} {
        variable system_path

        set project [_sync_project]
        set lib_dir [file join $system_path $project lib]
        set to_pull [list]
        set skipped [list]
        set unchanged 0

        set manifest_path [_base_lib_manifest_path $project]
        set manifest [_base_manifest_read $manifest_path]

        dict for {filename server_checksum} $server_libs {
            set local_file [file join $lib_dir $filename]

            set local_checksum ""
            if {[file exists $local_file]} {
                set local_checksum [sha256 -file $local_file]
            }

            if {$local_checksum eq $server_checksum} {
                incr unchanged
                continue
            }

            set base_checksum [_base_entry_get $manifest $filename]
            if {$local_checksum eq ""} {
                set decision pull
            } else {
                set decision [_base_decide $base_checksum $local_checksum $server_checksum]
            }

            if {$decision eq "keep_local"} {
                lappend skipped [dict create filename $filename decision $decision]
            } else {
                lappend to_pull [dict create filename $filename decision $decision]
            }
        }

        return [dict create to_pull $to_pull skipped $skipped unchanged $unchanged]
    }

    proc _sync_system_preview {system server_scripts} {
        variable system_path

        set project [_sync_project]
        set to_pull [list]
        set skipped [list]
        set extra [list]
        set unchanged 0

        set local_relsum [dict create]
        set sys_dir [file join $system_path $project $system]

        if {[file exists $sys_dir]} {
            foreach type {system extract analyze viewer} {
                set filename [_script_filename $system "" $type]
                set filepath [file join $sys_dir $filename]
                if {[file exists $filepath]} {
                    dict set local_relsum $filename [sha256 -file $filepath]
                }
            }
            foreach proto_dir [glob -nocomplain -type d [file join $sys_dir *]] {
                set proto [file tail $proto_dir]
                if {[string match .* $proto]} continue
                foreach type {protocol loaders variants stim extract viewer} {
                    set filename [_script_filename $system $proto $type]
                    set filepath [file join $proto_dir $filename]
                    if {[file exists $filepath]} {
                        dict set local_relsum [file join $proto $filename] \
                            [sha256 -file $filepath]
                    }
                }
            }
        }

        set manifest_path [_base_manifest_path $project $system]
        set manifest [_base_manifest_read $manifest_path]

        dict for {relkey info} $server_scripts {
            set reg_checksum [dict get $info checksum]

            set local_checksum ""
            if {[dict exists $local_relsum $relkey]} {
                set local_checksum [dict get $local_relsum $relkey]
            }

            if {$local_checksum eq $reg_checksum} {
                incr unchanged
                continue
            }

            set base_checksum [_base_entry_get $manifest $relkey]
            if {$local_checksum eq ""} {
                set decision pull
            } else {
                set decision [_base_decide $base_checksum $local_checksum $reg_checksum]
            }

            set entry [dict create \
                relpath $relkey \
                protocol [dict get $info protocol] \
                type [dict get $info type] \
                decision $decision]

            if {$decision eq "keep_local"} {
                lappend skipped $entry
            } else {
                lappend to_pull $entry
            }
        }

        dict for {relkey cs} $local_relsum {
            if {![dict exists $server_scripts $relkey]} {
                lappend extra $relkey
            }
        }

        return [dict create to_pull $to_pull skipped $skipped extra $extra unchanged $unchanged]
    }

    proc sync_preview {args} {
        set version "main"
        set force 0
        for {set i 0} {$i < [llength $args]} {incr i} {
            set a [lindex $args $i]
            switch -- $a {
                -force   { set force 1 }
                -version { incr i; set version [lindex $args $i] }
                default  { set version $a }
            }
        }

        set data [_manifest_fetch $version $force]

        set lib_preview [_sync_libs_preview [_manifest_libs $data]]
        set systems [dict create]

        if {[dict exists $data systems]} {
            foreach sys_manifest [dict get $data systems] {
                if {![dict exists $sys_manifest system]} continue
                set sys_name [dict get $sys_manifest system]
                dict set systems $sys_name [_sync_system_preview $sys_name \
                    [_manifest_scripts_of $sys_manifest]]
            }
        }

        package require yajltcl
        set obj [yajl create #auto]
        $obj map_open
        $obj string libs
        _preview_group_json $obj $lib_preview
        $obj string systems
        $obj map_open
        dict for {sys_name sys_preview} $systems {
            $obj string $sys_name
            _preview_group_json $obj $sys_preview
        }
        $obj map_close
        $obj map_close
        set result [$obj get]
        $obj delete
        return $result
    }

    proc _push_scan_local_only {sys_dir server_relkeys} {
        set results [list]
        if {![file exists $sys_dir]} { return $results }

        foreach entry [glob -nocomplain -directory $sys_dir *] {
            if {[file isdirectory $entry]} {
                set proto [file tail $entry]
                if {[string match .* $proto]} continue
                foreach sub [glob -nocomplain -directory $entry *] {
                    if {[file isdirectory $sub]} continue
                    set name [file tail $sub]
                    if {[string match .* $name]} continue
                    if {![regexp {\.(tcl|js)$} $name]} continue
                    set relkey [_local_relkey $proto $name]
                    if {[dict exists $server_relkeys $relkey]} continue
                    lappend results [dict create \
                        protocol $proto \
                        type [_derive_protocol_script_type $proto $name] \
                        filename $name \
                        relkey $relkey]
                }
            } else {
                set name [file tail $entry]
                if {[string match .* $name]} continue
                if {![regexp {\.(tcl|js)$} $name]} continue
                set relkey $name
                if {[dict exists $server_relkeys $relkey]} continue
                lappend results [dict create \
                    protocol "_" \
                    type [_derive_system_script_type $name] \
                    filename $name \
                    relkey $relkey]
            }
        }
        return $results
    }

    proc push_preview {system args} {
        variable system_path

        set version "main"
        set fresh 0
        for {set i 0} {$i < [llength $args]} {incr i} {
            set a [lindex $args $i]
            switch -- $a {
                -fresh   -
                -force   { set fresh 1 }
                -version { incr i; set version [lindex $args $i] }
                default  { set version $a }
            }
        }

        set data [_manifest_fetch $version $fresh]

        set project [_sync_project]
        set sys_dir [file join $system_path $project $system]
        set changed [list]
        set new_scripts [list]
        set missing [list]
        set unchanged 0

        lassign [_manifest_find_system $data $system] system_exists sys_entry

        set server_scripts [dict create]
        if {$system_exists} {
            set server_scripts [_manifest_scripts_of $sys_entry]
        }
        set server_relkeys [dict create]
        dict for {relkey -} $server_scripts {
            dict set server_relkeys $relkey 1
        }

        dict for {relkey info} $server_scripts {
            set protocol [dict get $info protocol]
            set filename [dict get $info filename]
            if {$protocol eq "" || $protocol eq "_system"} {
                set local_path [file join $sys_dir $filename]
            } else {
                set local_path [file join $sys_dir $protocol $filename]
            }

            if {![file exists $local_path]} {
                lappend missing $relkey
                continue
            }

            set local_hash [sha256 -file $local_path]
            if {$local_hash eq [dict get $info checksum]} {
                incr unchanged
            } else {
                lappend changed [dict create \
                    relkey $relkey \
                    protocol $protocol \
                    type [dict get $info type] \
                    filename $filename \
                    checksum [dict get $info checksum]]
            }
        }

        set new_scripts [_push_scan_local_only $sys_dir $server_relkeys]

        # Only the libs this system depends on, and only those with local
        # changes. Lib checksums ride along in the same manifest, so this
        # costs no extra request.
        set libs [list]
        set sys_libs [_system_lib_closure $system]
        if {[llength $sys_libs] > 0} {
            set lib_status [_lib_status_compare [_manifest_libs $data]]
            foreach fname $sys_libs {
                if {![dict exists $lib_status $fname]} continue
                set st [dict get $lib_status $fname]
                if {$st eq "modified" || $st eq "local_only"} {
                    lappend libs $fname
                }
            }
        }

        package require yajltcl
        set obj [yajl create #auto]
        $obj map_open
        $obj string system string $system
        $obj string systemExists bool $system_exists
        $obj string changed
        _preview_obj_array $obj $changed
        $obj string new
        _preview_obj_array $obj $new_scripts
        $obj string missing
        _preview_str_array $obj $missing
        $obj string unchanged
        $obj number [_preview_int $unchanged]
        $obj string libs
        _preview_str_array $obj $libs
        $obj map_close
        set result [$obj get]
        $obj delete
        return $result
    }

    proc _push_put_script {system project relkey script_info user comment manifest_path manifest} {
        variable registry_url
        variable registry_workgroup

        set protocol [dict get $script_info protocol]
        set stype    [dict get $script_info type]
        set filename [dict get $script_info filename]
        set server_cs ""
        if {[dict exists $script_info checksum]} {
            set server_cs [dict get $script_info checksum]
        }

        if {$protocol eq "" || $protocol eq "_system" || $protocol eq "_"} {
            set local_file [file join $::ess::system_path $project $system $filename]
            set api_protocol "_"
        } else {
            set local_file [file join $::ess::system_path $project $system $protocol $filename]
            set api_protocol $protocol
        }

        if {![file exists $local_file]} {
            error "Local file not found: $relkey"
        }

        set f [open $local_file r]
        set content [read $f]
        close $f

        set expected [_base_entry_get $manifest $relkey]
        if {$expected eq ""} { set expected $server_cs }

        set url "${registry_url}/api/v1/ess/script/${registry_workgroup}/${system}/${api_protocol}/${stype}"
        set body [dict_to_json [dict create \
            content $content \
            updatedBy $user \
            comment $comment \
            expectedChecksum $expected]]

        set response [https_put $url $body]

        set new_checksum ""
        catch { set new_checksum [json_get $response checksum] }
        if {$new_checksum eq ""} { set new_checksum [sha256 $content] }
        _base_entry_set manifest $relkey $new_checksum main $user
        return $new_checksum
    }

    proc push_system {system args} {
        variable registry_url
        variable registry_workgroup
        variable current

        set user_override ""
        set comment ""
        set include_libs 0
        set add_new 0

        foreach {key val} $args {
            switch -- $key {
                -user          { set user_override $val }
                -comment       { set comment $val }
                -include_libs  { set include_libs $val }
                -add           { set add_new 1 }
            }
        }

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }
        if {$registry_workgroup eq ""} {
            error "Workgroup not configured"
        }

        set user [_resolve_commit_user $user_override]
        if {$user ne ""} {
            set role [_get_user_role $user]
            if {$role eq "viewer"} {
                error "User '$user' has role 'viewer' and cannot push to registry"
            }
        }

        if {$comment eq ""} {
            set comment "pushed from dserv"
        }

        set preview_json [push_preview $system -fresh]
        set preview [json_to_dict $preview_json]

        set changed [dict get $preview changed]
        set new_scripts [dict get $preview new]
        set system_exists [dict get $preview systemExists]
        set lib_list [dict get $preview libs]

        if {!$system_exists && !$add_new && ([llength $new_scripts] > 0 || [llength $changed] > 0)} {
            error "System '$system' not on registry — use -add to create and push new scripts"
        }

        if {!$system_exists && $add_new && [llength $new_scripts] > 0} {
            set first_proto "_"
            foreach entry $new_scripts {
                set p [dict get $entry protocol]
                if {$p ne "" && $p ne "_" && $p ne "_system"} {
                    set first_proto $p
                    break
                }
            }
            set scaffold_url "${registry_url}/api/v1/ess/scaffold/system"
            set scaffold_body [dict_to_json [dict create \
                workgroup $registry_workgroup \
                system $system \
                protocol $first_proto \
                createdBy $user]]
            if {[catch { https_post $scaffold_url $scaffold_body } serr]} {
                error "Failed to scaffold system $system: $serr"
            }
        }

        set project $current(project)
        set manifest_path [_base_manifest_path $project $system]
        set manifest [_base_manifest_read $manifest_path]

        set pushed 0
        set added 0
        set lib_pushed 0
        set errors [list]

        foreach entry $changed {
            set relkey [dict get $entry relkey]
            if {[catch {
                _push_put_script $system $project $relkey $entry $user $comment \
                    $manifest_path manifest
                incr pushed
            } err]} {
                lappend errors "$relkey: $err"
            }
        }

        if {$add_new} {
            foreach entry $new_scripts {
                set relkey [dict get $entry relkey]
                if {[catch {
                    _push_put_script $system $project $relkey $entry $user $comment \
                        $manifest_path manifest
                    incr added
                } err]} {
                    lappend errors "$relkey: $err"
                }
            }
        }

        _base_manifest_write $manifest_path $manifest

        if {$include_libs && [llength $lib_list] > 0} {
            foreach fname $lib_list {
                if {[catch {
                    commit_lib $fname $comment $user
                    incr lib_pushed
                } lerr]} {
                    lappend errors "lib/$fname: $lerr"
                }
            }
        }

        _manifest_invalidate

        return [dict create pushed $pushed added $added lib_pushed $lib_pushed errors $errors]
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
    proc commit_script {type {comment ""} {user_override ""}} {
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

        set user [_resolve_commit_user $user_override]

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
        set project $current(project)
        set url "${registry_url}/api/v1/ess/script/${registry_workgroup}/${system}/${api_protocol}/${api_type}"

        # The base manifest is the authoritative ancestor checksum and,
        # unlike the in-memory registry_checksums cache, survives a
        # crash+restart. The manifest key is the path relative to the
        # system dir; strip the "project/system/" prefix from relpath.
        set manifest_path [_base_manifest_path $project $system]
        set manifest [_base_manifest_read $manifest_path]
        set sysprefix [file join $project $system]
        set relkey $relpath
        if {[string first "${sysprefix}/" $relpath] == 0} {
            set relkey [string range $relpath [string length "${sysprefix}/"] end]
        }

        # Use stored base checksum for optimistic locking. The server
        # rejects the commit if someone else modified the script since our
        # base. Empty string skips the check (first commit / no base yet).
        set expected [_base_entry_get $manifest $relkey]
        if {$expected eq "" && [dict exists $registry_checksums $type]} {
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

        # Advance the base to what the server now has (the checksum of the
        # content we just pushed) in both the in-memory cache and the
        # persistent manifest, so a later sync sees this as a clean
        # fast-forward rather than a phantom local edit.
        set new_checksum ""
        catch { set new_checksum [json_get $response checksum] }
        if {$new_checksum eq ""} {
            # Fall back to hashing what we pushed.
            set new_checksum [sha256 $content]
        }
        dict set registry_checksums $type $new_checksum
        _base_entry_set manifest $relkey $new_checksum main $user
        _base_manifest_write $manifest_path $manifest

        ess_info "Committed $type to registry ($registry_workgroup/$system)" "sync"
        _manifest_invalidate
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
    proc commit_lib {filename {comment ""} {user_override ""}} {
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

        set user [_resolve_commit_user $user_override]

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
        _manifest_invalidate
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
    # Compare local lib files against a filename -> checksum map. Shared
    # by lib_sync_status (which fetches that map) and push_preview (which
    # already has it from the workgroup manifest).
    proc _lib_status_compare {server_libs} {
        variable system_path

        set project [_sync_project]
        set lib_dir [file join $system_path $project lib]
        set result [dict create]
        set remaining $server_libs

        if {[file exists $lib_dir]} {
            foreach f [glob -nocomplain -type f [file join $lib_dir *.tm]] {
                set fname [file tail $f]
                set local_checksum [sha256 -file $f]

                if {[dict exists $remaining $fname]} {
                    if {$local_checksum eq [dict get $remaining $fname]} {
                        dict set result $fname synced
                    } else {
                        dict set result $fname modified
                    }
                    dict unset remaining $fname
                } else {
                    dict set result $fname local_only
                }
            }
        }

        dict for {fname checksum} $remaining {
            dict set result $fname registry_only
        }

        return $result
    }

    proc lib_sync_status {} {
        variable registry_url
        variable registry_workgroup

        if {$registry_url eq ""} {
            error "Registry URL not configured"
        }

        set url "${registry_url}/api/v1/ess/libs?workgroup=${registry_workgroup}"
        if {[catch {
            set response [https_get $url]
            set data [json_to_dict $response]
        } err]} {
            ess_error "Failed to fetch lib list for sync status: $err" "sync"
            return [dict create]
        }

        set server_libs [dict create]
        foreach lib [dict get $data libs] {
            dict set server_libs [dict get $lib filename] [dict get $lib checksum]
        }

        return [_lib_status_compare $server_libs]
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

    # ── Helper: install viewer plugins to web-accessible directory ────
    #
    # After sync, copies any *_viewer.js files from the system directory
    # to {www_path}/viewers/ where dg_viewer.html can find them.
    # The target filename is {system}.js so the HTML can load via
    # import('./viewers/{system}.js').
    #
    # Uses www_path from dserv (set via -w flag or config) or the
    # viewers_dir variable if explicitly configured.
    #
    proc _install_viewers {system} {
        variable system_path
        variable viewers_dir
        variable current

        # Determine target directory. Priority:
        #   1. Explicit viewers_dir variable (if configured)
        #   2. system/www_path datapoint (published by TclServer, available
        #      from any subprocess via dservGet — more reliable than the
        #      www_path Tcl command which may not be bound in subprocess
        #      interps)
        set target_dir $viewers_dir
        if {$target_dir eq ""} {
            set wp ""
            catch {set wp [dservGet system/www_path]}
            if {$wp eq ""} {
                # Last-ditch fallback: try www_path command (main interp only)
                catch {set wp [www_path]}
            }
            if {$wp eq ""} {
                ess_warning "  _install_viewers: www_path not available (checked dservGet system/www_path and www_path command)" "sync"
                return
            }
            set target_dir [file join $wp viewers]
        }

        set project $current(project)
        set sys_dir [file join $system_path $project $system]

        # Find all candidate viewer files first (system-level and protocol-level)
        set candidates [list]
        foreach viewer_file [glob -nocomplain [file join $sys_dir *_viewer.js]] {
            lappend candidates [list $viewer_file "${system}.js"]
        }
        foreach proto_dir [glob -nocomplain -type d [file join $sys_dir *]] {
            set proto [file tail $proto_dir]
            foreach viewer_file [glob -nocomplain [file join $proto_dir *_viewer.js]] {
                lappend candidates [list $viewer_file "${system}_${proto}.js"]
            }
        }

        if {[llength $candidates] == 0} {
            return
        }

        ess_info "  _install_viewers: target_dir=$target_dir ([llength $candidates] file(s))" "sync"

        # Make target directory if needed
        if {![file exists $target_dir]} {
            if {[catch {file mkdir $target_dir} mk_err]} {
                ess_warning "  Could not create $target_dir: $mk_err" "sync"
                return
            }
        }

        # Copy each viewer file
        foreach c $candidates {
            lassign $c src dst_name
            set target [file join $target_dir $dst_name]
            if {[catch {file copy -force $src $target} err]} {
                ess_warning "  Could not install viewer $src -> $target: $err" "sync"
            } else {
                ess_info "  Installed viewer: $target" "sync"
            }
        }
    }

    # ── Helper: map script type to filename ───────────────────────────
    proc _script_filename {system protocol type} {
        if {$protocol eq ""} {
            switch $type {
                system  { return "${system}.tcl" }
                extract { return "${system}_extract.tcl" }
                viewer  { return "${system}_viewer.js" }
                default { return "${system}_${type}.tcl" }
            }
        } else {
            switch $type {
                protocol { return "${protocol}.tcl" }
                viewer   { return "${protocol}_viewer.js" }
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
        # Use explicit system/protocol to build path (not current-system context)
        set project $current(project)
        if {$api_protocol eq "_" || $api_protocol eq ""} {
            set filename [_script_filename $system "" $api_type]
            set base_file [file join $system_path $project $system $filename]
        } else {
            set filename [_script_filename $system $api_protocol $api_type]
            set base_file [file join $system_path $project $system $api_protocol $filename]
        }
        if {[file exists $base_file]} {
            file delete $base_file
            ess_info "Removed local file $base_file" "sync"
        }

        # Drop the base-manifest entry so it doesn't linger as a phantom.
        # (delete_system/delete_protocol remove the whole dir, taking the
        # manifest with it; a single-script delete must prune by hand.)
        set manifest_path [_base_manifest_path $project $system]
        if {[file exists $manifest_path]} {
            if {$api_protocol eq "_" || $api_protocol eq ""} {
                set relkey $filename
            } else {
                set relkey [file join $api_protocol $filename]
            }
            set manifest [_base_manifest_read $manifest_path]
            _base_entry_unset manifest $relkey
            _base_manifest_write $manifest_path $manifest
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
