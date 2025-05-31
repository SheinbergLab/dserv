#
# VARIANTS
#   match_to_sample phd
#
# DESCRIPTION
#   variant dictionary
#

namespace eval match_to_sample::phd {

    # variant description
    # find database
    set db {}
    set paths [list \
		   /shared/qpcs/stimuli/graspomatic/Grasp3Shapes.db \
		   ${::ess::system_path}/$::ess::current(project)/match_to_sample/phd/data/Grasp3Shapes.db]
    foreach p $paths {
	if [file exists $p] { set db $p; break }
    }

    variable variants {
	VV {
	    description "visual visual shape MTS"	    
	    loader_proc setup_trials
	    loader_options {
		dbfile { { Grasp3Shapes.db $db } }
		trial_type VV
		filled 1
		limit -1
	    }
	    params { sample_time 2000 }
	    init { rmtSend "setBackground 100 100 100" }
	}
	HV {
	    description "haptic visual shape MTS"	    
	    loader_proc setup_trials
	    loader_options {
		dbfile { { Grasp3Shapes.db $db } }
		trial_type HV
		filled 1
		limit -1
	    }
	    params { sample_time 10000 }
	    init { rmtSend "setBackground 10 10 10" }
	}
    }
    
    # substitute variables ($db) in variant description above
    set variants [subst $variants]
}
