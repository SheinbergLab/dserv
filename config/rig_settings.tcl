#
# rig_settings.tcl -- rig facts that SEVERAL interps read, declared once
#
# Sourced by dsconf.tcl after local/pre-*.tcl and BEFORE the first
# subprocess. See docs/settings_panel_plan.md §3 for why this file exists at
# all; the short version:
#
# `joystick transport` is owned by ess and `juicer destination` by juicer, so
# each declares its own knob and settings::put routes there. The three knobs
# here are a different shape -- ONE fact about the rig that three or four
# interps each hold a copy of:
#
#   stim host          stimconf.tcl (stim_host), ess-2.0.tm (::ess::rmt_host),
#                      dsconf.tcl (derives ess/ipaddr, in MAIN)
#   registry url       essconf.tcl, ess_sync, scripts, configs
#   registry workgroup the same, plus trialsyncconf.tcl and mesh
#
# They were distributed by the process ENVIRONMENT -- ESS_RMT_HOST in
# local/pre-remote.tcl, ESS_REGISTRY_URL/ESS_WORKGROUP in
# local/pre-registry.tcl -- and that is not an accident to be undone: the
# environment is INHERITED, so it needs no ordering. ess starts before stim
# (dsconf.tcl), so a knob declared in stim could never reach ess, and reading
# another subprocess's datapoint on the boot path is the failure dsconf.tcl
# already carries a scar from.
#
# So MAIN declares them, exports the effective value to the environment
# before any child exists, and every downstream reader stays exactly as it
# was. What the declaration adds is a way to CHANGE one without editing a
# file by hand: validation at the door, a value the fleet page can read
# (settings/stim/host, settings/registry/*), and an -apply that pushes to the
# interps already running.
#
# Their schema carries `interp dserv`, which a page must recognize: `send`
# refuses main, so a UI evaluates the put directly rather than wrapping it.
#

package require settings

namespace eval ::rig {}

#
# Push a change into a live child.
#
# sendNoReply, ALWAYS. `send` blocks on the child's reply queue, so one
# wedged subprocess would take the main request loop down -- every page,
# every rig tool -- from somebody adjusting a setting. A push that cannot be
# delivered is not an error either: mesh is absent on a solo box, trialsync
# and scripts may not have started, and at BOOT none of them exist yet (the
# environment is what carries the value then, not these pushes).
#
proc ::rig::push {who script} {
    catch { sendNoReply $who $script }
}

# ---------------------------------------------------------------- stim host

# A bare host or address for a socket, not a URL. Rejecting the scheme here
# is worth more than accepting it: `http://192.168.88.50` would otherwise
# reach `socket` and fail once per stimSend, far from the mistake.
proc ::rig::_norm_host {v} {
    set v [string trim $v]
    if { $v eq "" } {
        error "stim host: empty -- use 'localhost' for a stim2 on this box"
    }
    if { [regexp {^[a-zA-Z][a-zA-Z0-9+.-]*://} $v] } {
        error "stim host: '$v' is a URL -- give the host alone (stim2 listens\
 on port 4612), e.g. localhost or 192.168.88.50"
    }
    if { [regexp {\s} $v] } {
        error "stim host: '$v' contains whitespace"
    }
    return $v
}

proc ::rig::_apply_stim_host {v} {
    set ::env(ESS_RMT_HOST) $v
    # stimSend opens a socket per message, so the proxy moves on the very
    # next message.
    push stim "stimOpen [list $v]"
    # ess does NOT: configure_stim/rmtOpen holds a connection to the old host
    # and has already read screen geometry from it. Update the variable so
    # the next system load connects to the right place, and leave the live
    # connection alone -- tearing it down mid-experiment to honor a settings
    # write would be the worse surprise.
    push ess "set ::env(ESS_RMT_HOST) [list $v]
              set ::ess::rmt_host [list $v]
              dservSet ess/rmt_host [list $v]"
}

settings::declare stim host -default localhost \
    -values {localhost <host> <ip>} \
    -validate ::rig::_norm_host \
    -apply ::rig::_apply_stim_host \
    -doc "Where stim2 listens (port 4612). 'localhost' for a stim2 on this
box; a hostname or LAN address for a separate stimulus computer.
Takes effect immediately for the stim proxy; ESS picks it up on the
next system load. Replaces ESS_RMT_HOST in local/pre-remote.tcl.
ess/ipaddr -- the address stim2 sends datapoints BACK to -- is
DERIVED from this at boot, so changing it on a multi-homed rig
wants a restart."

# ------------------------------------------------------------ registry url,
# ------------------------------------------------------------ registry workgroup

# Empty is a real value here and MUST stay the default: a rig with no
# registry configured has ess::registry's url/workgroup empty today, and a
# default of https://dserv.net would quietly enroll every such rig.
proc ::rig::_norm_url {v} {
    set v [string trim $v]
    if { $v eq "" } { return "" }             ;# not configured
    if { [regexp {\s} $v] } {
        error "registry url: '$v' contains whitespace"
    }
    if { ![regexp {^https?://} $v] } {
        set v "https://$v"                    ;# bare host is the common typo
    }
    return [string trimright $v /]
}

proc ::rig::_norm_workgroup {v} {
    set v [string trim $v]
    if { $v eq "" } { return "" }             ;# not configured
    if { ![regexp {^[A-Za-z0-9][A-Za-z0-9_.-]*$} $v] } {
        error "registry workgroup: '$v' -- letters, digits, '-', '_' and '.'\
 only (it is a path element in the registry API), e.g. brown-sheinberg"
    }
    return $v
}

proc ::rig::_apply_registry {which v} {
    switch -- $which {
        url       { set envvar ESS_REGISTRY_URL; set opt -url }
        workgroup { set envvar ESS_WORKGROUP;    set opt -workgroup }
    }
    if { $v ne "" } {
        set ::env($envvar) $v
    } elseif { [info exists ::env($envvar)] } {
        unset ::env($envvar)
    }

    # Lazy readers -- trialsync consults $::env(ESS_WORKGROUP) at flush time
    # rather than caching it -- need only the child's environment.
    foreach who {ess scripts trialsync configs} {
        if { $v ne "" } {
            push $who "set ::env($envvar) [list $v]"
        } else {
            push $who "catch {unset ::env($envvar)}"
        }
    }

    # Cached readers need their own setter. Two registry stacks are in play
    # (ess_registry's ess::registry::configure and ess_sync's
    # ess::registry_configure); which interp has which varies, so offer both
    # and let push's catch sort it out.
    push ess       "ess::registry::configure $opt [list $v]"
    push ess       "ess::registry_configure $opt [list $v]"
    push scripts   "ess::registry_configure $opt [list $v]"
    push configs   "registry_configure $opt [list $v]"

    # mesh takes both at once and normalizes the URL its own way (it adds an
    # explicit port), so hand it the pair rather than one half.
    push mesh "mesh_configure [list [settings::get registry url]]\
 [list [settings::get registry workgroup]]"
}

# No -values on either: settings::_validate's wildcard branch accepts a
# wildcard match only for a NON-empty value, so any values list at all would
# make "" -- the not-configured state, and the default -- unwritable. The
# shape belongs in -doc for these two.
settings::declare registry url -default "" \
    -validate ::rig::_norm_url \
    -apply {::rig::_apply_registry url} \
    -doc "ess-script registry this rig syncs with, e.g. https://dserv.net.
Empty means no registry: sync, push and trial ingest stay local.
A bare host gets https:// prepended. Replaces ESS_REGISTRY_URL in
local/pre-registry.tcl (and the registry half of local/mesh.tcl).
dserv-agent has its OWN --registry flag and will not follow this
until it is restarted with a matching one."

settings::declare registry workgroup -default "" \
    -validate ::rig::_norm_workgroup \
    -apply {::rig::_apply_registry workgroup} \
    -doc "Workgroup this rig belongs to on the registry, e.g.
brown-sheinberg. Empty means trial ingest reports no_workgroup and
script sync has nothing to pull. Replaces ESS_WORKGROUP in
local/pre-registry.tcl (and the workgroup half of local/mesh.tcl).
Same dserv-agent caveat as registry url."

# ------------------------------------------------------- adopt and export

#
# One-time carryover, exactly as obs_autobind's migration did it: if nobody
# has DECLARED the knob (source is still `default`) but the legacy
# local/pre-*.tcl set the environment variable, adopt that value into
# local/rig.tcl and keep going. The rig behaves identically across the
# restart, and the hand-edited file stops being the source of truth.
#
# Guarded on source_of so it happens once. After that the declaration wins
# even if the legacy file is still there and still exports -- which is the
# right way round, and the reason the note below only suggests deleting it.
#
proc ::rig::adopt_and_export {sub key envvar} {
    if { [settings::source_of $sub $key] eq "default" &&
         [info exists ::env($envvar)] && [string trim $::env($envvar)] ne "" } {
        if { [catch { settings::put $sub $key $::env($envvar) -persist } err] } {
            puts stderr "rig_settings: $envvar='$::env($envvar)' not adopted: $err"
        } else {
            puts "rig_settings: adopted $envvar into local/rig.tcl\
 (setting $sub $key [settings::get $sub $key])"
        }
    }

    # The declaration is authoritative in BOTH directions: a rig that
    # declares an empty value must not keep running on a stale export from a
    # legacy pre-file.
    set v [settings::get $sub $key]
    if { $v ne "" } {
        set ::env($envvar) $v
    } elseif { [info exists ::env($envvar)] } {
        unset ::env($envvar)
    }
    return $v
}

#
# ---- the ESS paths -------------------------------------------------------
#
# Where the systems live, where data is written, where exports go. Same
# class as the two above: read by ess, scripts, configs and df, distributed
# by the environment, and set until now in local/pre-systemdir.tcl and
# local/pre-datafiles.tcl by hand.
#
# EMPTY is the default and means "do not export" -- ess-2.0.tm then uses its
# own built-in default. A rig that has never declared these behaves exactly
# as it did, and the gear shows `default` rather than a path someone might
# think is in force.
#
# Not validated for EXISTENCE, deliberately: an installer may create the
# directory after this runs, and refusing here would trade a legible warning
# for an aborted boot. The check happens below, where it can say so.
#
proc ::rig::_norm_path { v } {
    set v [string trim $v]
    if { $v eq "" } { return "" }
    if { [file pathtype $v] ne "absolute" } {
        error "path '$v' must be absolute -- it is read by subprocesses whose\
               working directory is not yours"
    }
    return [string trimright $v /]
}

settings::declare ess system_path -default "" \
    -validate ::rig::_norm_path \
    -doc "where ESS looks for systems (ESS_SYSTEM_PATH). Empty uses the
built-in default, /usr/local/dserv/systems. This is the one path
whose absence is loud: with no systems tree the boot system does
not load and ESS comes up empty."

settings::declare ess data_dir -default "" \
    -validate ::rig::_norm_path \
    -doc "where raw data files are written (ESS_DATA_DIR). Empty uses the
built-in default."

settings::declare ess export_path -default "" \
    -validate ::rig::_norm_path \
    -doc "where exports are written (ESS_EXPORT_PATH). Empty uses the
built-in default."

::rig::adopt_and_export stim     host        ESS_RMT_HOST
::rig::adopt_and_export registry url         ESS_REGISTRY_URL
::rig::adopt_and_export registry workgroup   ESS_WORKGROUP
::rig::adopt_and_export ess      system_path ESS_SYSTEM_PATH
::rig::adopt_and_export ess      data_dir    ESS_DATA_DIR
::rig::adopt_and_export ess      export_path ESS_EXPORT_PATH

# A declared systems path that is not there is worth one loud line at boot:
# it is the difference between "ESS is empty because nothing is configured"
# and "ESS is empty because it was pointed somewhere that does not exist",
# and those look identical from a page.
if { [info exists ::env(ESS_SYSTEM_PATH)] && ![file isdirectory $::env(ESS_SYSTEM_PATH)] } {
    puts stderr "rig_settings: WARNING ESS_SYSTEM_PATH '$::env(ESS_SYSTEM_PATH)'\
 is not a directory -- no system will load"
    catch { dservSet system/warning \
                "ESS_SYSTEM_PATH '$::env(ESS_SYSTEM_PATH)' is not a directory" }
}

# Name the files this replaced, once, at boot. They are still sourced (they
# run before this file) and they are now overridden, so a rig that keeps them
# is not broken -- just misleading to the next person who edits one and sees
# nothing change.
foreach {f what} {
    pre-remote.tcl   "stim host"
    pre-registry.tcl "registry url/workgroup"
} {
    if { [file exists [file join $dspath local $f]] } {
        puts "rig_settings: local/$f is superseded by '$what' in\
 local/rig.tcl -- edits to it no longer take effect; safe to delete"
    }
}

if { [catch { settings::errors } errs] == 0 && [llength $errs] } {
    foreach e $errs { puts stderr "rig_settings: $e" }
}
