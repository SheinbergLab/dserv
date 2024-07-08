#
# VARIANTS
#   search circles
#
# DESCRIPTION
#   variant dictionary
#

namespace eval search::circles {
    set variants {
	single      { basic_search { 10 0 1.5 } }
	variable    { basic_search { 20 {0 2 4 8} 1.5 } }
	distractors { basic_search { 100 6 1.5 } }
    }
}

