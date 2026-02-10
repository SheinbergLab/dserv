# -*- mode: tcl -*-
#
# ess_paths - Script path resolution for ESS
#
# Centralizes all knowledge of:
#   - Where script files live on disk (naming conventions, directory layout)
#   - Overlay-vs-base precedence for file resolution
#   - File ownership management for multi-user overlays
#
# Stateless path functions take (system, protocol, type) as arguments
# so they can be used from any context — the ess subprocess, a sync
# subprocess, or command-line tools.
#

package provide ess_paths 1.0

namespace eval ess::paths {

    # ── Configuration ─────────────────────────────────────────────
    #
    # These must be set via configure before calling resolve/save_path.
    # system_path: root of the base script tree (e.g., /usr/local/dserv/systems)
    # overlay_path: user-specific overlay directory, or "" if disabled
    # project: the project subdirectory (e.g., "ess")
    #
    variable system_path ""
    variable overlay_path ""
    variable project ""

    proc configure {args} {
        variable system_path
        variable overlay_path
        variable project

        foreach {opt val} $args {
            switch -- $opt {
                -system_path  { set system_path $val }
                -overlay_path { set overlay_path $val }
                -project      { set project $val }
                default       { error "ess::paths::configure: unknown option '$opt'" }
            }
        }
    }

    # ── All known script types ────────────────────────────────────
    #
    # Core types (always present): system, protocol, loaders, variants, stim
    # Optional types: sys_extract, sys_analyze, proto_extract
    #
    variable all_types {system protocol loaders variants stim sys_extract sys_analyze proto_extract}
    variable optional_types {sys_extract sys_analyze proto_extract}

    # ── Relative path construction ────────────────────────────────
    #
    # Given a system, protocol, and script type, return the relative
    # path from the tree root.  Returns "" if insufficient info
    # (e.g., protocol-level type with no protocol).
    #
    # Examples:
    #   relpath planko 9point system     → ess/planko/planko.tcl
    #   relpath planko 9point variants   → ess/planko/9point/9point_variants.tcl
    #   relpath planko 9point sys_extract → ess/planko/planko_extract.tcl
    #   relpath planko "" system          → ess/planko/planko.tcl
    #
    proc relpath {system protocol type} {
        variable project

        if {$system eq ""} { return "" }

        switch -- $type {
            system {
                return [file join $project $system ${system}.tcl]
            }
            protocol {
                if {$protocol eq ""} { return "" }
                return [file join $project $system $protocol ${protocol}.tcl]
            }
            loaders {
                if {$protocol eq ""} { return "" }
                return [file join $project $system $protocol ${protocol}_loaders.tcl]
            }
            variants {
                if {$protocol eq ""} { return "" }
                return [file join $project $system $protocol ${protocol}_variants.tcl]
            }
            stim {
                if {$protocol eq ""} { return "" }
                return [file join $project $system $protocol ${protocol}_stim.tcl]
            }
            sys_extract {
                return [file join $project $system ${system}_extract.tcl]
            }
            sys_analyze {
                return [file join $project $system ${system}_analyze.tcl]
            }
            proto_extract {
                if {$protocol eq ""} { return "" }
                return [file join $project $system $protocol ${protocol}_extract.tcl]
            }
            default {
                error "ess::paths::relpath: unknown script type '$type'"
            }
        }
    }

    # ── System/protocol level query ───────────────────────────────
    #
    # Returns 1 if the type is system-level, 0 if protocol-level.
    #
    proc is_system_level {type} {
        switch -- $type {
            system - sys_extract - sys_analyze { return 1 }
            default { return 0 }
        }
    }

    # ── File resolution (overlay → base) ──────────────────────────
    #
    # resolve: returns the absolute path, checking overlay first.
    # The file may or may not exist — caller should check.
    #
    proc resolve {relpath} {
        variable overlay_path
        variable system_path

        if {$overlay_path ne ""} {
            set f [file join $overlay_path $relpath]
            if {[file exists $f]} {
                return $f
            }
        }
        return [file join $system_path $relpath]
    }

    # ── Glob with overlay precedence ──────────────────────────────
    #
    # Returns a list of resolved paths.  Overlay entries win on name
    # conflict, but only if they contain real content (not empty dirs).
    #
    proc resolve_glob {relpattern} {
        variable overlay_path
        variable system_path

        set entries [dict create]

        # Base entries first
        foreach f [glob -nocomplain [file join $system_path $relpattern]] {
            dict set entries [file tail $f] $f
        }

        # Overlay adds new entries (not in base) but does NOT replace
        # base directories.  For directories, the overlay may contain only
        # a subset of files (e.g., one edited variant script).  If we
        # replaced the base path, callers like find_systems that check
        # for $dir/$dir.tcl would fail because the overlay directory
        # lacks the main system .tcl file.
        #
        # Individual file resolution still honours overlay-first
        # precedence via resolve(), which is used when actually loading
        # each script.
        if {$overlay_path ne ""} {
            foreach f [glob -nocomplain [file join $overlay_path $relpattern]] {
                set tail [file tail $f]
                if {[file isfile $f]} {
                    # Files: overlay wins unconditionally
                    dict set entries $tail $f
                } elseif {![dict exists $entries $tail]} {
                    # Directories: only add if NOT already present in base.
                    # This handles overlay-only systems/protocols while
                    # preserving the base path for systems that exist in both.
                    if {[llength [glob -nocomplain [file join $f *]]] > 0} {
                        dict set entries $tail $f
                    }
                }
                # If directory exists in both base and overlay, keep the
                # base path — resolve() handles per-file overlay precedence.
            }
        }

        return [dict values $entries]
    }

    # ── Source identification ─────────────────────────────────────
    #
    # Returns "overlay" if the file exists in the overlay, "base" otherwise.
    #
    proc source {relpath} {
        variable overlay_path

        if {$overlay_path ne ""} {
            set f [file join $overlay_path $relpath]
            if {[file exists $f]} {
                return "overlay"
            }
        }
        return "base"
    }

    # ── Absolute path helpers ─────────────────────────────────────

    # Always returns the base path (ignoring overlay)
    proc base_path {relpath} {
        variable system_path
        return [file join $system_path $relpath]
    }

    # Returns the overlay path (may not exist, returns "" if no overlay)
    proc overlay_path_for {relpath} {
        variable overlay_path
        if {$overlay_path eq ""} { return "" }
        return [file join $overlay_path $relpath]
    }

    # Returns where saves should go: overlay if active, else base
    proc save_path {relpath} {
        variable overlay_path

        if {$overlay_path ne ""} {
            set save_file [file join $overlay_path $relpath]
            set dir [file dirname $save_file]
            if {![file exists $dir]} {
                mkdir_matching_owner $dir
            }
            return $save_file
        }
        return [base_path $relpath]
    }

    # ── Overlay user management ───────────────────────────────────
    #
    # set_overlay_user sets overlay_path based on username.
    # Empty string disables overlay.
    #
    proc set_overlay_user {username} {
        variable overlay_path
        variable system_path

        if {$username eq ""} {
            set overlay_path ""
        } else {
            set overlay_path [file join $system_path overlays $username]
            if {![file exists $overlay_path]} {
                mkdir_matching_owner $overlay_path
            }
        }
        return $overlay_path
    }

    proc get_overlay_user {} {
        variable overlay_path
        if {$overlay_path eq ""} { return "" }
        return [file tail $overlay_path]
    }

    # ── Overlay status for all script types ───────────────────────
    #
    proc overlay_status {system protocol} {
        set result [dict create]
        variable all_types

        foreach type $all_types {
            set rp [relpath $system $protocol $type]
            if {$rp ne ""} {
                dict set result $type [source $rp]
            }
        }
        return $result
    }

    # ── File ownership helpers ────────────────────────────────────
    #
    # These ensure files created in the overlay tree match the
    # ownership of the parent directory (important for multi-user
    # lab environments where dserv runs as root).
    #

    proc get_path_ownership {path} {
        set dir $path
        while {$dir ne "/" && $dir ne "."} {
            if {[file exists $dir]} {
                return [list [file attributes $dir -owner] [file attributes $dir -group]]
            }
            set dir [file dirname $dir]
        }
        return [list root root]
    }

    proc mkdir_matching_owner {dir} {
        if {[file exists $dir]} { return }

        # Walk up to find existing ancestor
        set ancestors {}
        set d $dir
        while {$d ne "/" && $d ne "." && ![file exists $d]} {
            lappend ancestors $d
            set d [file dirname $d]
        }

        # Get ownership from existing ancestor
        lassign [get_path_ownership $dir] owner group

        # Create directories top-down
        foreach d [lreverse $ancestors] {
            file mkdir $d
            catch {
                file attributes $d -owner $owner -group $group
            }
        }
    }

    proc fix_file_ownership {filepath} {
        if {![file exists $filepath]} { return }
        set dir [file dirname $filepath]
        lassign [get_path_ownership $dir] owner group
        catch {
            file attributes $filepath -owner $owner -group $group
        }
    }

    # ── Cleanup empty overlay directories ─────────────────────────
    #
    proc cleanup_empty_overlay_dirs {} {
        variable overlay_path
        if {$overlay_path eq ""} return

        set queue [glob -nocomplain -type d [file join $overlay_path *]]
        while {[llength $queue] > 0} {
            set dir [lindex $queue 0]
            set queue [lrange $queue 1 end]

            # Add subdirs to queue
            foreach sub [glob -nocomplain -type d [file join $dir *]] {
                lappend queue $sub
            }

            # Remove if empty
            if {[llength [glob -nocomplain [file join $dir *]]] == 0} {
                catch { file delete $dir }
            }
        }
    }
}
