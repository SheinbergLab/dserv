set dspath [file dir [info nameofexecutable]]

set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require dlsh
package require qpcs
package require sqlite3
package require yajltcl

set db_path "$dspath/db/dserv.db"

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
    set host [dservGet system/hostaddr]
    set domain ess

    # if the system has changed, update the blockid
    if { [string equal $dpoint ess/variant_info] } {
	set maxblockid [db eval { SELECT max(block_id) from trials; }]
	if { $maxblockid == "{}" } {
	    dservSet ess/block_id 0
	} else {
	    dservSet ess/block_id [expr { $maxblockid+1 }]
	}
    }

    if { [string equal $dpoint ess/trialinfo] } {
	if { 0 } {
	    set d [::yajl::json2dict $data]
	    
	    foreach v "trialid project system protocol variant version subject status rt" {
		set $v [dict get $d $v]
	    }
	    
	    set blockid [dservGet ess/block_id]
	    db eval {
		INSERT into trial (host, block_id, trial_id, project, state_system, protocol, variant, version, subject, status, rt, trialinfo)
		VALUES($host, $blockid, $trialid, $project, $system, $protocol, $variant, $version, $subject, $status, $rt, jsonb($data))
	    }
	}
    } else {
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

#
# helper functions
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
    set g [get_trials $block_id ]
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

setup_database $db_path

dservAddMatch   ess/*
dpointSetScript ess/* process_ess

dservAddMatch   system/*
dpointSetScript system/* process_system

dservAddExactMatch trialdg
dpointSetScript    trialdg process_trialdg

# find latest block id stored in db
dservSet ess/block_id [get_status_value block_id]

puts "SQLite DB ready at $db_path"



