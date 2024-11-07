puts "initializing dbserver"

set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]


package require dlsh
package require qpcs
package require sqlite3
package require yajltcl

set db_path "/tmp/trialinfo.db"

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

    # Create the 'trial' table if it does not exist
    db eval {
        CREATE TABLE IF NOT EXISTS trial (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            blockid INTEGER,
            trialid INTEGER,
            system TEXT,
            protocol TEXT,
	    variant TEXT,
	    subject TEXT,
	    status INTEGER,
	    rt INTEGER,
	    stiminfo TEXT,
            sys_time TEXT DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
        );
    }

    # Create indices for faster lookup
    db eval {
        CREATE INDEX IF NOT EXISTS idx_trial_id ON trial (id);
        CREATE INDEX IF NOT EXISTS idx_trial_subject ON trial (subject);
    }

    # Adjust PRAGMA settings to improve performance
    db eval {
        PRAGMA synchronous = OFF;
        PRAGMA journal_mode = WAL;
        PRAGMA temp_store = MEMORY;
    }
}

proc process_trial { dpoint trialinfo } {
    global db

    set d [::yajl::json2dict $trialinfo]

    foreach v "blockid trialid system protocol variant subject status rt" {
	set $v [dict get $d $v]
    }
    
    db eval {
	INSERT INTO trial (blockid, trialid, system, protocol, variant, subject, status, rt, stiminfo) \
	    VALUES ($blockid, $trialid, $system, $protocol, $variant, $subject, $status, $rt, $trialinfo)
    }
}


setup_database $db_path
puts "SQLite DB ready at $db_path"

dservAddExactMatch ess/trialinfo
dpointSetScript    ess/trialinfo process_trial



