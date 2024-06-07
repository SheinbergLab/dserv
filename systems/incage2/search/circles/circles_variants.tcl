#
# FILE
#   circles_variants.tcl
#
# PROTOCOL
#   search/circles
#
# DESCRIPTION
#   This file defines the variants, including the shortname for default 
#     init and destroy files, and a description
#
# AUTHOR
#   DLS

namespace eval circles {
    # ---------------------------------------------------------------------
    # Variant Definitions
    # format { {display name} {loader proc}
    #          {shortname for defaults,init,destroy} {display description} }
    set variants {    	
	{ {single}      basic_search {single}   {touch a single target} }
	{ {variable}    basic_search {variable} {circles with multiple distractors} }
	{ {distractors} basic_search {multiple} {find target among distractors} }
    }
    
    # ---------------------------------------------------------------------
    # Default init and destroy procs (copy and edit to make variant specific)
    proc default_init { } {
	 ess::set_param n_rep 100
	 ess::set_param ndists 0
     }
    proc default_destroy { } {
	 # nothing to do
     }
    
    proc single_init { } {
	 ess::set_param n_rep 100
	 ess::set_param ndists 0
    }
    proc single_destroy { } {
	# nothing to do
    }
    
    proc variable_init { } {
	ess::set_param n_rep 30
	ess::set_param ndists "0 2 4 8"
    }
    proc variable_destroy { } {
 	# nothing to do
    }
    
    proc multiple_init { } {
	ess::set_param n_rep 100
	ess::set_param ndists 6
    }
    proc multiple_destroy { } {
 	# nothing to do
    }

}

