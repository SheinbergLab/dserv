#
# VARIANTS
#   emcalib 9point
#
# DESCRIPTION
#   variants/loaders for 9point version of emcalib
#

namespace eval emcalib::9point {
    variable variants {
	spots {
	    description "standard 9 point"
	    loader_proc basic_calib
	    loader_options {
		nr { 2 4 }
	    }
	    init { rmtSend "setBackground 10 10 10" }
	    deinit { rmtSend "setBackground 0 0 0" }
	    params { fixhold_time 300 }
	}
    }
}

