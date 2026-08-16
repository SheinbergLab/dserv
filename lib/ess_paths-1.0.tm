# -*- mode: tcl -*-
#
# ess_paths - Script path resolution for ESS
#
# Centralizes all knowledge of:
#   - Where script files live on disk (naming conventions, directory layout)
#   - File ownership management for trees written to by root-run dserv
#
# Stateless path functions take (system, protocol, type) as arguments
# so they can be used from any context — the ess subprocess, a sync
# subprocess, or command-line tools.
#
# History: this module once implemented a per-user overlay layer
# (overlay-vs-base precedence for every resolve).  The overlay was
# retired in 2026-08 — the manifest-based sync (ess_sync/ess_scripts)
# made a second local layer redundant — so resolution is now a single
# tree rooted at system_path.
#

package provide ess_paths 1.0

namespace eval ess::paths {

    # ── Configuration ─────────────────────────────────────────────
    #
    # These must be set via configure before calling resolve.
    # system_path: root of the script tree (e.g., /home/lab/systems)
    # project: the project subdirectory (e.g., "ess")
    #
    variable system_path ""
    variable project ""

    proc configure {args} {
        variable system_path
        variable project

        foreach {opt val} $args {
            switch -- $opt {
                -system_path  { set system_path $val }
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

    # ── File resolution ───────────────────────────────────────────
    #
    # resolve: returns the absolute path in the tree.
    # The file may or may not exist — caller should check.
    #
    proc resolve {relpath} {
        variable system_path
        return [file join $system_path $relpath]
    }

    proc resolve_glob {relpattern} {
        variable system_path
        return [glob -nocomplain [file join $system_path $relpattern]]
    }

    # ── Absolute path helpers ─────────────────────────────────────

    proc base_path {relpath} {
        variable system_path
        return [file join $system_path $relpath]
    }

    # ── File ownership helpers ────────────────────────────────────
    #
    # These ensure files created in the tree match the ownership of
    # the parent directory (important for multi-user lab environments
    # where dserv runs as root).
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
}
