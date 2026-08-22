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
            dl_set $g:blink_color [dl_repeat [dl_slist $color] $n_obs]
            dl_set $g:remaining   [dl_ones $n_obs]

            return $g
        }
    }
}
