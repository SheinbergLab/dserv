#
# LOADERS
#   blinky blink
#
# DESCRIPTION
#   Builds stimdg: one row per observation, each saying where the spot is,
#   how big, what colour, and how many times it blinks.
#

namespace eval blinky::blink {
    proc loaders_init { s } {
        $s add_loader blink_load { nr radius color n_blinks } {
            # colour is a NAME, not "r g b": a loader option with spaces
            # in it does not survive the trip as one word, and the failure
            # is a column with zero rows rather than an error. blink_stim
            # maps the name, so stimdg stays readable too.
            if { [dg_exists stimdg] } { dg_delete stimdg }
            set g [dg_create stimdg]
            dg_rename $g stimdg

            set n_obs $nr

            dl_set $g:stimtype    [dl_fromto 0 $n_obs]
            dl_set $g:blink_x     [dl_repeat [dl_flist 0.0] $n_obs]
            dl_set $g:blink_y     [dl_repeat [dl_flist 0.0] $n_obs]
            dl_set $g:blink_r     [dl_repeat [dl_flist $radius] $n_obs]
            dl_set $g:n_blinks    [dl_repeat [dl_ilist $n_blinks] $n_obs]
            # Carry the COLOUR ITSELF, as three floats, beside the name.
            # The name is for reading stimdg; the numbers are what both the
            # stim and the viz draw with -- the alternative is a mapping on
            # each side, and the viz's silently drew nothing for any name
            # dlsh does not know (`amber`).
            # Inline, not a proc next door: a loader body runs as a METHOD
            # on the system object, so a bare `color_rgb` there resolves in
            # the object's namespace and is simply not found. Same trap as a
            # live param's apply prefix -- see blink.tcl.
            switch -exact -- $color {
                green { lassign {0.0 0.8 0.2} cr cg cb }
                amber { lassign {0.9 0.6 0.0} cr cg cb }
                blue  { lassign {0.2 0.5 1.0} cr cg cb }
                red   { lassign {0.9 0.2 0.2} cr cg cb }
                white { lassign {0.9 0.9 0.9} cr cg cb }
                default { lassign {0.0 0.8 0.2} cr cg cb }
            }
            dl_set $g:blink_color [dl_repeat [dl_slist $color] $n_obs]
            dl_set $g:blink_r [dl_repeat [dl_flist $cr] $n_obs]
            dl_set $g:blink_g [dl_repeat [dl_flist $cg] $n_obs]
            dl_set $g:blink_b [dl_repeat [dl_flist $cb] $n_obs]
            dl_set $g:remaining   [dl_ones $n_obs]

            return $g
        }
    }
}
