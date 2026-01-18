package require dlsh
package require qpcs
package require sqlite3
package require yajltcl

set db_path "$dspath/db/dserv.db"

set last_load_time {}

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# Function to handle database setup and corruption detection
proc setup_database {db_path} {
    set dir_path [file dirname $db_path]
    if {![file exists $dir_path]} {
        file mkdir $dir_path
    }
    if {[file exists $db_path]} {
        # Attempt to open the database to check for corruption
        set open_status [catch {
            sqlite3 db $db_path
            set result [db eval {PRAGMA integrity_check;}]
        } errMsg]
        if {$open_status || $result ne "ok"} {
            puts "Database is corrupted, renaming and recreating."
            file rename -force $db_path "$db_path.corrupt"
            sqlite3 db $db_path
        }
    } else {
        sqlite3 db $db_path
    }

    # Create the 'trials' table if it does not exist
    db eval {
        CREATE TABLE IF NOT EXISTS trials (
            block_id INTEGER PRIMARY KEY,
            subject TEXT,
            project TEXT,
            state_system TEXT,
            protocol TEXT,
            variant TEXT,
            n_trials INTEGER,
            n_complete INTEGER,
            pct_complete REAL,
            pct_correct REAL,
            trialdg BLOB,
            date TEXT DEFAULT (date()),
            sys_time TEXT DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
        );
    }

    db eval {
        CREATE TABLE IF NOT EXISTS status (
            host TEXT,
            status_source TEXT,
            status_type TEXT UNIQUE,
            status_value TEXT,
            sys_time TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
            primary key (host, status_source, status_type)
        );
    }

    # Create indices for faster lookup
    db eval {
        CREATE INDEX IF NOT EXISTS idx_trials_subject ON trials (subject);
        CREATE INDEX IF NOT EXISTS idx_trials_date ON trials (date);
    }

    # Adjust PRAGMA settings to improve performance
    db eval {
        PRAGMA synchronous = OFF;
        PRAGMA journal_mode = WAL;
        PRAGMA temp_store = MEMORY;
    }
}

#
# Core data handlers
#

proc process_trialdg { dpoint data } {
    set system       [dservGet ess/system]
    set protocol     [dservGet ess/protocol]
    set variant      [dservGet ess/variant]
    set subject      [dservGet ess/subject]
    set project      [dservGet ess/project]
    set block_id     [dservGet ess/block_id]
    set n_trials     [dservGet ess/block_n_trials]
    set n_complete   [dservGet ess/block_n_complete]
    set pct_complete [dservGet ess/block_pct_complete]
    set pct_correct  [dservGet ess/block_pct_correct]
    
    db eval { INSERT INTO trials (block_id, subject, project, state_system, protocol, variant, trialdg, sys_time)
        VALUES($block_id, $subject, $project, $system, $protocol, $variant, $data, current_timestamp)
        ON CONFLICT(block_id) DO UPDATE SET
        trialdg=$data, n_complete=$n_complete, n_trials=$n_trials, pct_complete=$pct_complete, pct_correct=$pct_correct, sys_time=current_timestamp;
    }
}

proc process_ess { dpoint data } {
    global last_load_time
    set host [dservGet system/hostaddr]
    set domain ess

    # if the system has changed, update the blockid
    if { [string equal $dpoint ess/last_load_time] } {
        if { $last_load_time == {} || 
             $last_load_time != $data} {
            set maxblockid [db eval { SELECT max(block_id) from trials; }]
            if { $maxblockid == "{}" } {
                dservSet ess/block_id 0
            } else {
                dservSet ess/block_id [expr { $maxblockid+1 }]
            }
            set last_load_time $data
        }
    }

    if { [string equal $dpoint ess/trialinfo] } {
        set key [file tail $dpoint]
        db eval { INSERT INTO status (host, status_source, status_type, status_value, sys_time)
            VALUES($host, $domain, $key, $data, current_timestamp)
            ON CONFLICT(host, status_source, status_type) DO UPDATE SET
            host=$host, status_source=$domain, status_type=$key, status_value=$data, sys_time=current_timestamp;
        }
    }
}

proc process_system { dpoint data } {
    set host [dservGet system/hostaddr]
    set domain system
    
    set key [file tail $dpoint]
    db eval { INSERT INTO status (host, status_source, status_type, status_value, sys_time)
        VALUES($host, $domain, $key, $data, current_timestamp)
        ON CONFLICT(host, status_source, status_type) DO UPDATE SET
        host=$host, status_source=$domain, status_type=$key, status_value=$data, sys_time=current_timestamp;
    }
}

proc process_stimdg { dpoint data } {
    # Reconstruct stimdg from serialized data and publish sortable columns
    if {[catch {
        if {[dg_exists stimdg]} { dg_delete stimdg }
        dg_fromString $data
        publish_sortable_columns stimdg
    } err]} {
        puts "Error processing stimdg: $err"
    }
}

#
# Helper functions
#

proc get_status_value { status_type { status_source ess } } {
    db eval { SELECT status_value from status where status_type == $status_type and status_source == $status_source }
}

proc get_trials { block_id } {
    db eval { SELECT trialdg from trials where block_id == $block_id } v {
        if { [string length $v(trialdg)] } {
            return [dg_fromString $v(trialdg)]
        } else {
            return
        }
    }
}

proc get_stats_by { block_id args } {
    set g [get_trials $block_id]
    set categs [dl_tcllist [dl_paste [dl_slist $g] [dl_slist :] [dl_slist {*}$args]]]
    dl_local c [dl_uniqueCross {*}$categs]
    dl_local pc [dl_means [dl_sortByLists $g:status {*}$categs]]
    dl_local rts [dl_means [dl_sortByLists $g:rt {*}$categs]]
    dl_local counts [dl_lengths [dl_sortByLists $g:status {*}$categs]]
    set result [list \
            vars   $args \
            levels [dl_tcllist $c] \
            status [dl_tcllist $pc] \
            rt     [dl_tcllist $rts] \
            count  [dl_tcllist $counts]]
    dg_delete $g
    return $result
}

#
# Performance sorting for frontend clients
#

# Analyze a datagroup and return columns suitable for sortby
# Returns list of column names that:
#   - Have same length as stimtype
#   - Have <= max_unique unique values
#   - Are not in the exclude list
#   - Are not list-type columns
proc get_sortable_columns { {g stimdg} {max_unique 6} } {
    set exclude {stimtype remaining}
    set result {}
    
    if {![dg_exists $g]} {
        return $result
    }
    
    # Get reference length from stimtype if it exists
    if {![dl_exists $g:stimtype]} {
        return $result
    }
    set reflen [dl_length $g:stimtype]
    
    foreach col [dg_tclListnames $g] {
        # Skip excluded columns
        if {$col in $exclude} continue
        
        # Skip if length doesn't match
        if {[dl_length $g:$col] != $reflen} continue
        
        # Skip list-type columns (nested lists)
        if {[dl_datatype $g:$col] eq "list"} continue
        
        # Check number of unique values
        dl_local u [dl_unique $g:$col]
        if {[dl_length $u] <= $max_unique} {
            lappend result $col
        }
    }
    
    return $result
}

# Publish sortable columns as JSON to ess/sortby_columns
proc publish_sortable_columns { {g stimdg} } {
    set cols [get_sortable_columns $g]
    set json [yajl create #auto]
    $json array_open
    foreach col $cols {
        $json string $col
    }
    $json array_close
    set result [$json get]
    $json delete
    
    dservSet ess/sortby_columns $result
    return $result
}

# Get sorted performance stats, returns JSON for frontend
# sortby1 and sortby2 can be empty strings to skip that grouping level
proc get_sorted_perf { block_id {sortby1 {}} {sortby2 {}} } {
    set g [get_trials $block_id]
    
    if {$g eq ""} {
        return {{"error": "no trials found"}}
    }
    
    # Build list of grouping variables
    set groupvars {}
    if {$sortby1 ne "" && [dl_exists $g:$sortby1]} {
        lappend groupvars $sortby1
    }
    if {$sortby2 ne "" && [dl_exists $g:$sortby2]} {
        lappend groupvars $sortby2
    }
    
    set json [yajl create #auto]
    $json map_open
    
    # vars
    $json string "vars" array_open
    foreach v $groupvars {
        $json string $v
    }
    $json array_close
    
    if {[llength $groupvars] == 0} {
        # No grouping - just compute overall stats
        $json string "levels" array_open array_close
        
        # status is 0/1, so mean gives pct correct directly
        set pct_correct [dl_mean $g:status]
        set mean_rt [dl_mean $g:rt]
        set n [dl_length $g:status]
        
        $json string "status" array_open number $pct_correct array_close
        $json string "rt" array_open number $mean_rt array_close
        $json string "count" array_open number $n array_close
    } else {
        # Group by the specified variables
        set categs {}
        foreach v $groupvars {
            lappend categs $g:$v
        }
        
        dl_local sorted_status [dl_sortByLists $g:status {*}$categs]
        dl_local sorted_rt [dl_sortByLists $g:rt {*}$categs]
        
        dl_local pct_correct [dl_means $sorted_status]
        dl_local mean_rt [dl_means $sorted_rt]
        dl_local counts [dl_lengths $sorted_status]
        
        # Get unique levels for each variable
        # dl_uniqueCross requires 2+ args, so handle single var with dl_unique
        if {[llength $groupvars] == 1} {
            # Single variable: use dl_unique
            dl_local levels [dl_unique [lindex $categs 0]]
            set levels_tcl [list [dl_tcllist $levels]]
        } else {
            # Multiple variables: use dl_uniqueCross (returns parallel lists)
            dl_local levels [dl_uniqueCross {*}$categs]
            set levels_tcl [dl_tcllist $levels]
        }
        
        # levels_tcl is now a list of lists, one per variable
        # e.g., {{-5 0 5} {-5 0 5}} for two vars
        # Convert to array of tuples: [[-5,-5], [-5,0], ...]
        
        set nvars [llength $groupvars]
        set nlevels [llength [lindex $levels_tcl 0]]
        
        $json string "levels" array_open
        for {set i 0} {$i < $nlevels} {incr i} {
            $json array_open
            for {set v 0} {$v < $nvars} {incr v} {
                $json string [lindex [lindex $levels_tcl $v] $i]
            }
            $json array_close
        }
        $json array_close
        
        # status (pct correct per level)
        $json string "status" array_open
        foreach val [dl_tcllist $pct_correct] {
            $json number $val
        }
        $json array_close
        
        # rt
        $json string "rt" array_open
        foreach val [dl_tcllist $mean_rt] {
            $json number $val
        }
        $json array_close
        
        # count
        $json string "count" array_open
        foreach val [dl_tcllist $counts] {
            $json number $val
        }
        $json array_close
    }
    
    $json map_close
    set result [$json get]
    $json delete
    
    dg_delete $g
    return $result
}

#
# Initialize database and subscriptions
#

setup_database $db_path

dservAddMatch   ess/*
dpointSetScript ess/* process_ess

dservAddMatch   system/*
dpointSetScript system/* process_system

dservAddExactMatch trialdg
dpointSetScript    trialdg process_trialdg

dservAddExactMatch stimdg
dpointSetScript    stimdg process_stimdg

# find latest block id stored in db
dservSet ess/block_id [get_status_value block_id]

puts "SQLite DB ready at $db_path"
