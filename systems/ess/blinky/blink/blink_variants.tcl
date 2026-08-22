#
# VARIANTS
#   blinky blink
#
# DESCRIPTION
#   A few, so the variant picker in ESS Control has something to show and a
#   fresh rig can see a reload actually change the display.
#

namespace eval blinky::blink {
    variable variants {
        green {
            description "a green spot, three blinks per observation"
            loader_proc blink_load
            loader_options {
                nr        { 20 50 200 }
                radius    { 3.0 1.0 6.0 }
                color     { green amber blue }
                n_blinks  { 3 1 10 }
            }
            init   { rmtSend "setBackground 10 10 10" }
            deinit {}
        }
        amber {
            description "a slower amber spot -- change on_ms/off_ms live"
            loader_proc blink_load
            loader_options {
                nr        { 20 50 200 }
                radius    { 4.0 2.0 8.0 }
                color     { amber green blue }
                n_blinks  { 2 1 6 }
            }
            init   { rmtSend "setBackground 10 10 10" }
            deinit {}
        }
    }

    # use subst to replace variables in the variant definition above
    set variants [subst $variants]
}
