#
# Handle branch and pull requests for system paths that are git repos
#
puts "Configuring git"

set dspath [file dir [info nameofexecutable]]

set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

namespace eval git {
    variable path {}
    variable setuid 0
    variable owner {}
    variable is_repo 0
    
    proc set_path { { project ess } } {
	variable path
	variable owner
	variable setuid
	variable is_repo
	set path $::env(ESS_SYSTEM_PATH)/ess
	set is_repo [path_is_repo $path]
	set owner [file attributes $path -owner]
	set uid [exec whoami]
	if { $owner != $uid } { set setuid 1 } { set setuid 0 }
    }

    proc exec_cmd {} {
	variable path
	variable owner
	variable setuid
	if { !$setuid } {
	    return "git -C $path"
	} else {
	    return "sudo -u $owner git -C $path"
	}
    }
    
    proc path_is_repo { path } {
	# Normalize the path to handle various inputs 
	set normalizedPath [file normalize $path]

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

    proc diff {} {
	variable is_repo
	if { !$is_repo } return
	
	set git [exec_cmd]
	set cmd [list {*}$git diff --name-only]
	catch [list exec {*}$cmd] result
	dservSet ess/git/diff $result
	return $result
    }

    proc current_branch {} {
	variable is_repo
	if { !$is_repo } { return "default" }
	
	set git [exec_cmd]
	set cmd [list {*}$git branch --show-current]
	catch [list exec {*}$cmd] result
	dservSet ess/git/branch $result
	return $result
    }
    
    proc switch { { branch main } } {
	variable is_repo
	if { !$is_repo } { return "default" }
	
	set git [exec_cmd]
	set cmd [list {*}$git switch $branch]
	catch [list exec {*}$cmd] result
	current_tag;		 # sets ess/git/tag
	return [current_branch]; # sets ess/git/branch 
    }

    #
    # branches
    #
    #  return a cleaned up list of available branches
    #   main will always be first
    #
    proc branches {} {
	variable is_repo
	if { !$is_repo } { return "default" }
	
	set git [exec_cmd]
	set cmd [list {*}$git remote prune origin]
        catch [list exec {*}$cmd] result
	set cmd [list {*}$git branch -a]
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
    
    proc pull {} {
	variable is_repo
	if { !$is_repo } return 
	
	set git [exec_cmd]
	set cmd [list {*}$git pull]
	catch [list exec {*}$cmd] result
	dservSet ess/git/pull $result
	return $result
    }

    proc push {} {
	variable is_repo
	if { !$is_repo } return 
	
	set git [exec_cmd]
	set cmd [list {*}$git push]
	catch [list exec {*}$cmd] result
	dservSet ess/git/push $result
	return $result
    }
    
    proc commit { pattern message } {
	variable is_repo
	if { !$is_repo } return 
	
	set git [exec_cmd]
	set cmd [list {*}$git commit $pattern -m $message]
	catch [list exec {*}$cmd] result
	dservSet ess/git/commit $result
	return $result
    }

    proc commit_and_push { { message "" } } {
	# Get current timestamp in a readable format
	set timestamp [clock format [clock seconds] -format "%Y-%m-%d %H:%M:%S"]
	
	# Create the full commit message with timestamp
	if { $message == "" } {
	    set commit_message "commit"
	} else {
	    set commit_message $message
	}
	set full_message "${commit_message} ${timestamp}"

	set git [exec_cmd]

	# commit step
	set cmd [list {*}$git commit -am $full_message]
	catch [list exec {*}$cmd] result
	dservSet ess/git/commit $result

	# Push the updates to active branch
	set cmd [list {*}$git push]
	catch [list exec {*}$cmd] result
	dservSet ess/git/push $result

	return $result
	
    }
    
    proc status {} {
	variable is_repo
	if { !$is_repo } return 
	
	set git [exec_cmd]
	set cmd [list {*}$git status]
	catch [list exec {*}$cmd] result
	dservSet ess/git/status $result
	return $result
    }
    
    proc switch_and_pull { { branch main } } {
	variable is_repo
	if { !$is_repo } return 
	switch $branch
	pull
    }

    proc tags {} {
	variable is_repo
	if { !$is_repo } { return 0.0.0 }
	
	set git [exec_cmd]
	set cmd [list {*}$git ls-remote --tags origin]
	catch [list exec {*}$cmd] result

	set d [dict create]
	foreach line [split $result \n] {
	    lassign $line commit tag
	    dict set d $tag $commit
	}
	dservSet ess/git/tags $d
	return $d
    }

    proc merge_to_main { { message "" } } {
	variable is_repo
	if { !$is_repo } { return -1 }

	set this_branch [current_branch]
	
	if { $this_branch == "main" } { return 0 }
	
	# Get current timestamp in a readable format
	set timestamp [clock format [clock seconds] -format "%Y-%m-%d %H:%M:%S"]
	
	# Create the full commit message with timestamp
	if { $message == "" } {
	    set commit_message "commit"
	} else {
	    set commit_message $message
	}
	set full_message "$commit_message - $timestamp"	

	set git [exec_cmd]

	# commit step
	set cmd [list {*}$git commit -am $full_message]
	catch [list exec {*}$cmd] result
	dservSet ess/git/commit $result

	# Switch to main branch
	git::switch

	# Pull latest changes from remote
	set cmd [list {*}$git pull origin main]
	catch [list exec {*}$cmd] result
	dservSet ess/git/pull $result

	# Merge your branch into main
	set cmd [list {*}$git merge ${this_branch}]
	catch [list exec {*}$cmd] result
	dservSet ess/git/merge $result

	# Push the updated main branch
	set cmd [list {*}$git push origin main]
	catch [list exec {*}$cmd] result
	dservSet ess/git/push $result

	return $result
    }
    proc current_tag {} {
	variable is_repo
	if { !$is_repo } { return 0.0.0 }
	
	set git [exec_cmd]
	set cmd [list {*}$git describe --abbrev=0 --tags]
	catch [list exec {*}$cmd] result
	dservSet ess/git/tag $result
	return $result
    }

}

git::set_path

if { 1 } {
    git::branches
    if { [info exists ::env(ESS_GIT_USERID)] } {
	git::switch_and_pull $::env(ESS_GIT_USERID)
    } else {
	git::switch_and_pull
    }
}
puts "Git configured"
