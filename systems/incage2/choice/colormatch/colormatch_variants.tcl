#
# FILE
#   colormatch_variants.tcl
#
# PROTOCOL
#   choice/colormatch
#
# DESCRIPTION
#   This file defines the variants, including the shortname for default 
#     init and destroy files, and a description
#
# AUTHOR
#   DLS

namespace eval colormatch {
    # ---------------------------------------------------------------------
    # Variant Definitions
    # format { {display name} {loader proc}
    #          {shortname for defaults,init,destroy} {display description} }
    set variants {    	
	{ {redgreen} setup_redgreen {redgreen} {match using only red and green} }
	{ {easy}     setup_easy     {easy}     {very different colors} }
	{ {random}   setup_random   {random}   {random color match to sample}   }
    }
    
    # ---------------------------------------------------------------------
    # Default init and destroy procs (copy and edit to make variant specific)
     proc default_init { } {
	 
     }
     proc default_destroy { } {
 	# nothing to do
     }

    proc easy_init { } {
	 ess::set_param use_buttons 0
    }
    proc easy_destroy { } {
	# nothing to do
    }
}
