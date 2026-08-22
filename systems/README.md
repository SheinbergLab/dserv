# systems/ — the bootstrap only

This tree ships exactly one system, `ess/blinky`, and that is deliberate.

**Real systems come from the registry.** A rig syncs them into
`ESS_SYSTEM_PATH` (`ess system_path`, declared — see the settings gear), and
that synced tree is the one anybody actually runs. What used to live here
was a *copy* of a handful of them — emcalib, planko, match_to_sample, search,
choice — and copies rot: by the time they were removed they were a stale
subset of what every rig was really running.

**What blinky is for.** A fresh install, an offline box, or a rig being
brought up before it has synced anything should still come up working. It
blinks a spot, records observations, and shows the same thing in ESS
Control's viz panel — which proves the state machine, stim2 and the event
path in one glance, three things that look identical when broken. It needs
no hardware.

**What loads at boot** is `ess boot_system`, and empty (the default) means
follow the rig, in this order:

    a declared value        a rig that must always come up on one task
    last loaded cleanly     what this rig was doing (learned, in the db)
    blinky                  a fresh or offline box
    nothing                 no systems at all, and it says so

So adopting the real tree needs no decision: load a system cleanly once and
the next boot comes up on it. Pin one in the gear only if a rig must always
start the same way.

**Do not add systems here.** Put them in the registry, where every rig gets
them and where they are versioned. The one exception is a second bootstrap
that is genuinely hardware-free and useful on a bare box — and if you add
one, say why here.
