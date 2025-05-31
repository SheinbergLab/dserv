#
# VARIANTS
#   match_to_sample colormatch
#
# DESCRIPTION
#   variant dictionary
#

namespace eval match_to_sample::colormatch {
    variable variants {
	noDistractor     {
	    description "no distractor"
	    loader_proc setup_trials
	    loader_options {
		n_rep { 50 100 200 400 800}
		targ_scale 1.5
		color_choices noDistractor
	    }
	}
	easy     {
	    description "easy comparisons"
	    loader_proc setup_trials
	    loader_options {
		n_rep { 50 100 200 400 800}
		targ_scale 1.5
		color_choices easy
	    }
	}
	random   {
	    description "random comparisons"
	    loader_proc setup_trials
	    loader_options {
		n_rep { 50 100 }
		targ_scale 1.5
		color_choices random
	    }
	}
	redgreen {
	    description "red/green MTS"
	    loader_proc setup_trials
	    loader_options {
		n_rep { 50 100 }
		targ_scale 1.5
		color_choices redgreen
	    }
	}
    }
}

