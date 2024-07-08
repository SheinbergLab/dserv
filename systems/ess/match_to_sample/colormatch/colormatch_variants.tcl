#
# VARIANTS
#   match_to_sample colormatch
#
# DESCRIPTION
#   variant dictionary
#

namespace eval match_to_sample::colormatch {
    set variants {
	easy     { setup_trials easy }
	random   { setup_trials random }
	redgreen { setup_trials redgreen }
    }
}

