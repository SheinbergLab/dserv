# -*- mode: tcl -*-

###
### Git API
###

package require yajltcl

namespace eval git {
    proc is_repo { folderPath } {
	# Normalize the path to handle various inputs 
	set normalizedPath [file normalize $folderPath]

	# Construct the path to the .git directory
	set gitDirPath [file join $normalizedPath .git]

	# Check if the .git directory exists and is actually a directory
	if {![file isdirectory $gitDirPath]} {
	    return 0
	}

	# Checks for expected contents (HEAD, config)
	if {![file exists [file join $gitDirPath HEAD]] ||
	    ![file exists [file join $gitDirPath config]]} {
	    return 0
	}

	return 1
    }

    proc diff { path } {
	set cmd [list git -C $path diff --name-only]
	catch [list exec {*}$cmd] result
	dservSet ess/git/diff $result
	return $result
    }

    proc current_branch { path } {
	set cmd [list git -C $path branch --show-current]
	catch [list exec {*}$cmd] result
	dservSet ess/git/branch $result
	return $result
    }
    
    proc switch_branch { path { branch main } } {
	set cmd [list git -C $path switch $branch]
	catch [list exec {*}$cmd] result
	return [current_branch $path]
    }

    #
    # branches
    #
    #  return a cleaned up list of available branches
    #   main will always be first
    #
    proc branches { path } {
	set cmd [list git -C $path branch -a]
	catch [list exec {*}$cmd] result
	set branches {}
	foreach line [split $result \n] {
	    if { ![string match *HEAD* $line] } {
		set b [file tail [string trim $line " *"]]
		if { $b != "main" } {
		    lappend branches $b
		}
	    }
	}
	set result "main [lsort -unique $branches]"
	dservSet ess/git/branches $result
	return $result
    }
    
    proc pull { path } {
	set cmd [list git -C $path pull]
	catch [list exec {*}$cmd] result
	dservSet ess/git/pull $result
	return $result
    }

    proc tags { path } {
	set cmd [list git -C $path ls-remote --tags origin]
	catch [list exec {*}$cmd] result

	set d [dict create]
	foreach line [split $result \n] {
	    lassign $line commit tag
	    dict set d $tag $commit
	}
	dservSet ess/git/tags $d
	return $d
    }

    proc current_tag { path } {
	set cmd [list git  -C $path describe --abbrev=0 --tags]
	catch [list exec {*}$cmd] result
	dservSet ess/git/tag $result
	return $result
    }
    
    proc set_safe_directory { path } {
	set cmd [list git config --system --add safe.directory $path]
	catch [list exec {*}$cmd] result
    }
}
