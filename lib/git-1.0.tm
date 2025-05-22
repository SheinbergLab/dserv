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
	dservSet git/diff $result
	return $result
    }

    proc get_branch { path } {
	set cmd [list git -C $path branch --show-current]
	catch [list exec {*}$cmd] result
	dservSet git/branch $result
	return $result
    }
    
    proc set_branch { path branch } {
	set cmd [list git -C $path checkout $branch]
	catch [list exec {*}$cmd] result
	return [getBranch $path]
    }

    proc pull { path } {
	set cmd [list git -C $path pull]
	catch [list exec {*}$cmd] result
	dservSet git/pull $result
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
	dservSet git/tags $d
	return $d
    }

    proc set_safe_directory { path } {
	set cmd [list git config --system -add safe.directory $path]
	catch [list exec {*}$cmd] result
    }
}
