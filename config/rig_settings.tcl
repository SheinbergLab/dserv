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

namespace eval ::rig {
    # What the ENVIRONMENT already said before this file touched anything --
    # a launchd plist, a shell, an older local/pre-systemdir.tcl. Captured
    # once, because the applies below WRITE these variables: without it,
    # clearing a declared path reads back the value the apply itself put
    # there and the rig can never return to what it inherited.
    variable inherited
    array set inherited {}
    foreach _v {ESS_SYSTEM_PATH ESS_DATA_DIR ESS_EXPORT_PATH ESS_RMT_HOST
                ESS_REGISTRY_URL ESS_WORKGROUP} {
        set inherited($_v) [expr {[info exists ::env($_v)] ? $::env($_v) : ""}]
    }
    unset -nocomplain _v
}

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
wants a restart; `stim dservhost` pins it when the derivation
cannot see the truth."

# ------------------------------------------------------- stim dservhost

# The reverse direction of `stim host`: the address stim2 sends its
# datapoints BACK to -- ess hands it over at connect as
#     rmtSend "set dservhost <this>"
# EMPTY is the default and means DERIVED, which is almost always right:
# loopback when stim2 runs on this box, else this box's address on the
# route toward the stim host, multi-homed safe (set_hostinfo, dsconf.tcl).
# Declare one only when the derivation cannot see the truth -- NAT or a
# tunnel between stim2 and this box. Replaces ESS_IPADDR in
# local/pre-remote.tcl (adopted below with the others).
proc ::rig::_norm_dservhost {v} {
    set v [string trim $v]
    if { $v eq "" } { return "" }             ;# derive -- the normal case
    if { [regexp {^[a-zA-Z][a-zA-Z0-9+.-]*://} $v] } {
        error "stim dservhost: '$v' is a URL -- give the address alone,\
 e.g. 192.168.88.40"
    }
    if { [regexp {\s} $v] } {
        error "stim dservhost: '$v' contains whitespace"
    }
    return $v
}

proc ::rig::_apply_stim_dservhost {v} {
    # ess/ipaddr is read at system-load time (rmtSend picks it up on the
    # next connect), so updating the datapoint is enough; the live stim2
    # connection keeps its current return address -- same bargain as
    # _apply_stim_host above.
    if { $v ne "" } {
        set ::env(ESS_IPADDR) $v
        dservSet ess/ipaddr $v
    } else {
        unset -nocomplain ::env(ESS_IPADDR)
        dservSet ess/ipaddr ""
        # Re-derive now that the pin is gone. set_hostinfo fills ess/ipaddr
        # only when empty (it now is). It does not exist yet during the
        # boot-time adoption pass -- dsconf defines and runs it later, so
        # boot needs nothing from here.
        if { [llength [info commands set_hostinfo]] } { set_hostinfo }
    }
}

settings::declare stim dservhost -default "" \
    -validate ::rig::_norm_dservhost \
    -apply ::rig::_apply_stim_dservhost \
    -doc "the address stim2 sends datapoints back to (handed over as
`set dservhost ...` when ESS connects). EMPTY -- the default --
derives it: loopback for a stim2 on this box, else this box's
address on the route toward the stim host, which is right even
multi-homed. Declare one only when the derivation cannot see the
truth (NAT, a tunnel). Applies at the next system load. Replaces
ESS_IPADDR in local/pre-remote.tcl."

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

#
# The one path worth applying LIVE. Changing where systems come from is
# something you do while setting a rig up, with the gear open, and being
# told "12 systems here" or "that directory does not exist" at the moment
# you type it is the difference between a setting and a guess. The env
# export still governs the next boot; this makes the running ess agree with
# it, and publishes what it found.
#
proc ::rig::_apply_system_path { v } {
    variable inherited
    # Empty means "not declared", NOT "use the built-in default": clearing
    # the setting must return the rig to what it INHERITED, not drag it off
    # a tree it was happily using. Reading $::env here would read this
    # proc's own previous write.
    if { $v ne "" } {
        set p $v
        set ::env(ESS_SYSTEM_PATH) $v
    } elseif { $inherited(ESS_SYSTEM_PATH) ne "" } {
        set p $inherited(ESS_SYSTEM_PATH)
        set ::env(ESS_SYSTEM_PATH) $p
    } else {
        set p /usr/local/dserv/systems
        catch { unset ::env(ESS_SYSTEM_PATH) }
    }
    #
    # Changing where systems come from has to be a RESET, not just a new
    # string. Four things move with it:
    #
    #   the module path -- set_project re-adds <path>/<project>/lib, or the
    #       old tree's lib keeps answering package requires;
    #   the loaded system -- if the new tree does not contain it, the rig is
    #       claiming to run something that is no longer there. Unload it and
    #       say so: idle and honest beats loaded and unfindable. Nothing is
    #       auto-loaded in its place, because silently starting a DIFFERENT
    #       system on a path change would be the worse surprise;
    #   ess/systems -- the System dropdown reads it, and ESS only republishes
    #       it inside load_system, so without this the menu still offers the
    #       old tree's systems (blinky, from the shipped tree) long after the
    #       path moved;
    #   the same judgement the picker uses -- a directory with no
    #       <name>.tcl, or one named after the project, is not a system.
    #
    # Sent as a BRACED script: it runs in ess, where current(project) and
    # system_path are, rather than being interpolated from here.
    #
    push ess "set ::ess::system_path [list $p]"
    push ess {
        catch { ::ess::set_project $::ess::current(project) }

        set _sys $::ess::current(system)
        set _root [file join $::ess::system_path $::ess::current(project)]
        if { $_sys ne "" &&
             ![file exists [file join $_root $_sys $_sys.tcl]] } {
            catch { ::ess::unload_system }
            # unload_system clears current() and publishes NOTHING: ess/system
            # and friends are written by config/triggers.tcl from the event
            # stream (ID/SYSTEM, type 18), which only a LOAD emits. So after a
            # standalone unload every page still shows the system that is no
            # longer there. Say the true thing here.
            foreach _dp {ess/system ess/protocol ess/variant} {
                catch { dservSet $_dp "" }
            }
            catch { dservSet system/warning \
                        "'$_sys' is not in $::ess::system_path -- unloaded" }
        }

        set _l {}
        foreach _d [lsort [glob -nocomplain -tails -directory $_root -types d *]] {
            if { $_d eq "lib" || $_d eq $::ess::current(project) } continue
            if { [file exists [file join $_root $_d $_d.tcl]] } { lappend _l $_d }
        }
        catch { dservSet ess/systems $_l }
        unset -nocomplain _sys _root _l _d _dp
    }
    catch { dservSet system/warning \
        [expr {[file isdirectory [file join $p ess]] ? "" :
               "ESS_SYSTEM_PATH '$p' has no ess/ subdirectory -- nothing will load"}] }
    return
}

settings::declare ess system_path -default "" \
    -validate ::rig::_norm_path \
    -apply ::rig::_apply_system_path \
    -doc "where ESS looks for systems (ESS_SYSTEM_PATH), holding them as
<path>/ess/<system>. Empty uses the built-in default,
/usr/local/dserv/systems. Applied live, so `ess boot_system`'s
picker lists what is actually there the moment you change it --
which is also the fastest way to find out a path is wrong. This
is the one path whose absence is loud: with no systems tree the
boot system does not load and ESS comes up empty."

settings::declare ess data_dir -default "" \
    -validate ::rig::_norm_path \
    -doc "where raw data files are written (ESS_DATA_DIR). Empty uses the
built-in default."

settings::declare ess export_path -default "" \
    -validate ::rig::_norm_path \
    -doc "where exports are written (ESS_EXPORT_PATH). Empty uses the
built-in default."

::rig::adopt_and_export stim     host        ESS_RMT_HOST
::rig::adopt_and_export stim     dservhost   ESS_IPADDR
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
    pre-remote.tcl          "stim host/dservhost"
    pre-registry.tcl        "registry url/workgroup"
    pre-systemdir.tcl       "ess system_path/data_dir/export_path"
    pre-datafiles.tcl       "ess data_dir/export_path"
    post-remotecompute.tcl  "ess compute_host"
    pre-remoteservers.tcl   "trialsync ingest_url"
    post-pins.tcl           "ess obs_pin (+ button N gpio:<pin>, juicer gpio_pin)"
} {
    if { [file exists [file join $dspath local $f]] } {
        puts "rig_settings: local/$f is superseded by '$what' in\
 local/rig.tcl -- edits to it no longer take effect; safe to delete"
    }
}

# ---------------------------------------------------------------------------
# QUARANTINE: local files whose MECHANISM is gone.
#
# Nothing sources these names any more (ain + openiris retired 2026-08-23;
# em/docs/ptp/registry never had a consumer), so renaming them cannot change
# behavior -- but leaving them invites the next person to edit a file that
# does nothing. local/post-openiris.tcl is worse than inert: essconf's
# post-*.tcl glob still picks it up and the openirisconf.tcl it sources is
# gone, so a leftover copy would abort essconf at boot. The rename IS the
# backup: the bytes stay where they lived, and the suffix answers "why is
# this here". The EXAMPLEs ride along because `make install` copies but
# never deletes, so repo-removed EXAMPLEs would otherwise sit on every rig
# forever.
#
# NAMES only -- content is never judged here. A file that is merely
# SUPERSEDED (the pre-*.tcl family above, still sourced every boot) only
# gets the warning: deciding whether its content is fully covered is a
# human's call.
#
foreach _f {
    ain.tcl post-openiris.tcl em.tcl docs.tcl ptp.tcl registry.tcl
    ain.tcl.EXAMPLE post-openiris.tcl.EXAMPLE registry.tcl.EXAMPLE
    pre-docs.tcl.EXAMPLE post-remotecompute.tcl.EXAMPLE
    pre-remoteservers.tcl.EXAMPLE post-pins.tcl.EXAMPLE
} {
    set _p [file join $dspath local $_f]
    if { ![file exists $_p] } continue
    set _to $_p.retired-[clock format [clock seconds] -format %Y-%m-%d]
    if { [catch { file rename $_p $_to } _e] } {
        # A same-name file re-appearing after a same-day quarantine lands
        # here (rename refuses to clobber the first backup). Say so and
        # leave both.
        puts stderr "rig_settings: could not retire local/$_f: $_e"
    } else {
        puts "rig_settings: local/$_f retired -> [file tail $_to]\
 (nothing sources it any more; see local/README)"
    }
}
unset -nocomplain _f _p _to _e

if { [catch { settings::errors } errs] == 0 && [llength $errs] } {
    foreach e $errs { puts stderr "rig_settings: $e" }
}
