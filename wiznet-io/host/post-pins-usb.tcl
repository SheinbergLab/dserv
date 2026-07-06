# post-pins-usb.tcl -- name-agnostic example: bind extio box I/O into a rig.
# Adapt into /usr/local/dserv/local/post-pins.tcl (runs in the main dserv interp).
#
# CONNECTION is handled for you now -- you do NOT open the port here. The persistent
# `extio` subprocess (config/extioconf.tcl) opens the box, forwards ess/in_obs +
# config/cmd, injects the box's telemetry, and HOT-SWAPS it (open on appear / close on
# vanish). It also publishes the live boxes it currently sees:
#     extio/boxes    -- list of currently-live box device names   (e.g. {reward buttons})
#     extio/primary  -- the first of them (the "the box" for a single-box rig)
#
# So post-pins only BINDS logical ESS channels to box I/O lines.
#
# HOT-SWAP-TRANSPARENT: bind with a GLOB device ("*") so the binding follows whatever
# box is currently plugged in. dserv matches the glob at PUBLISH time, so a box can be
# unplugged / renamed / replaced and its di/<pin> still drives the channel -- no
# re-binding, no hardcoded box name. (The box publishes LOGICAL levels: set the box
# pin's `active_low` so pressed = 1.)

# --- single box: bind any connected extio box's DI pins to ESS button channels ---
::ess::button_bind 0 {} box {* 14}          ;# extio/*/state/di/14 -> channel 0
#::ess::button_bind 1 {} box {* 13}         ;# extio/*/state/di/13 -> channel 1

# --- multi-box rig: scope the glob to a role-named box instead of a bare "*" ---
#   ::ess::button_bind 0 {} box {reward* 14}   ;# only boxes named "reward..."
#
# --- need exact identity / the current primary? bind it and re-bind on hot-swap ---
#   proc rebind_primary {args} {
#       set b [dservGet extio/primary]
#       if {$b ne ""} { ::ess::button_bind 0 {} box [list $b 14] }
#   }
#   dservAddExactMatch extio/primary ; dpointSetScript extio/primary rebind_primary ; rebind_primary

# --- box-timed outputs (deterministic, fired on the box's own clock) ---
#   ::ess::box_schedule_pulse [dservGet extio/primary] 5 200   ;# pulse pin 5 for 200 ms
