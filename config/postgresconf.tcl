set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require qpcs
package require postgres
package require yajltcl

set conn -1;		       # connection to our postgresql server
set dbname base;	       # name of database to write to
set insert_trialinfo_cmd    "insert_trialinfo"
set insert_status_cmd  "insert_status"

# use this to track block_id updates
set last_load_time {}

# Function to handle database setup and corruption detection
proc setup_database { db { overwrite 0 } } {
    global conn dbname insert_trialinfo_cmd insert_status_cmd
    set conninfo "dbname=$db user=postgres password=postgres host=localhost port=5432"
    if { [catch { set conn [postgres::connect $conninfo] } error] } {
	puts $error
	set conn -1
    }
    
    # Create the 'trial' table if it does not exist
    set stmt {
        CREATE TABLE IF NOT EXISTS trial (
  	    base_trial_id INTEGER primary key generated always as identity,
	    host VARCHAR(256),
            block_id INTEGER,
            trial_id INTEGER,
	    project TEXT,
            state_system TEXT,
            protocol TEXT,
	    variant TEXT,
	    version TEXT,
	    subject TEXT,
	    status INTEGER,
	    rt INTEGER,
	    trialinfo JSONB,
	    sys_time TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP	    
        );
    }
    postgres::exec $conn $stmt

    # Create prepared insert statement to make updates more efficient
    set stmt {
	INSERT INTO trial (host, block_id, trial_id, project, state_system, protocol, variant, version, subject, status, rt, trialinfo) \
	    VALUES (($1), ($2), ($3), ($4), ($5), ($6), ($7), ($8), ($9), ($10), ($11), ($12))
    }
    postgres::prepare $conn $insert_trialinfo_cmd $stmt 1

    
    # Create the 'status' table if it does not exist
    set stmt {
        CREATE TABLE IF NOT EXISTS status (
	    host VARCHAR(256),
	    status_source TEXT,
            status_type TEXT UNIQUE,
            status_value TEXT,
	    sys_time TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
	    primary key (host, status_source, status_type)
        );
    }
    postgres::exec $conn $stmt

    # Create prepared insert statement to make updates more efficient
    set stmt {
	INSERT INTO status (host, status_source, status_type, status_value) VALUES(($1), ($2), ($3), ($4))
	ON CONFLICT (status_type)
	DO UPDATE SET host = ($1), status_source = ($2), status_type = ($3), status_value = ($4), sys_time = current_timestamp;
    }
    postgres::prepare $conn $insert_status_cmd $stmt 1
}

proc process_ess { dpoint data } {
    global conn insert_trialinfo_cmd insert_status_cmd last_load_time

    set host [dservGet system/hostaddr]
    set domain ess

    # if the system has changed, update the blockid
    if { [string equal $dpoint last_load_time] } {
    	if { $last_load_time == {} ||
    	     $last_load_time != $data } {
			set maxblockid [postgres::query $conn { SELECT max(block_id) from trial; }]
			dservSet ess/block_id [expr $maxblockid+1]
			set last_load_time $data
		}
	}
    
    if { [string equal $dpoint ess/trialinfo] } {
	set d [::yajl::json2dict $data]
	
	foreach v "trialid project system protocol variant version subject status rt" {
	    set $v [dict get $d $v]
	}

	set blockid [dservGet ess/block_id]
	postgres::exec_prepared $conn $insert_trialinfo_cmd \
	    $host $blockid $trialid $project $system $protocol $variant $version $subject $status $rt $data
    } else {
	set key [file tail $dpoint]
	postgres::exec_prepared $conn $insert_status_cmd $host $domain $key $data
    }
}

proc process_system { dpoint data } {
    global conn insert_status_cmd

    set host [dservGet system/hostaddr]
    set domain system
    
    set key [file tail $dpoint]
    postgres::exec_prepared $conn $insert_status_cmd $host $domain $key $data
}


setup_database $dbname

dservAddMatch   ess/*
dpointSetScript ess/* process_ess

dservAddMatch   system/*
dpointSetScript system/* process_system

# insert initial settings into the ESS table
foreach v "time dio state name executable remote ipaddr \
	   obs_active obs_id obs_total datafile lastfile" {
    dservTouch ess/$v
}
dservTouch system/hostaddr

puts "PostgreSQL DB ready"
