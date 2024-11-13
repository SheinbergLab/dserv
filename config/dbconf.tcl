set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require qpcs
package require postgres
package require yajltcl

set conn -1;		       # connection to our postgresql server
set dbname mydb;	       # name of database to write to
set insert_trialinfo_cmd    "insert_trialinfo"
set insert_ess_setting_cmd  "insert_ess_setting"

# Function to handle database setup and corruption detection
proc setup_database { db { overwrite 0 } } {
    global conn dbname insert_trialinfo_cmd insert_ess_setting_cmd
    set conninfo "dbname=$db user=postgres password=postgres host=localhost port=5432"
    if { [catch { set conn [postgres::connect $conninfo] } error] } {
	puts $error
	set conn -1
    }
    
    # Create the 'trial' table if it does not exist
    set stmt {
        CREATE TABLE IF NOT EXISTS trial (
  	    id INTEGER primary key generated always as identity,
	    host VARCHAR(256),
            blockid INTEGER,
            trialid INTEGER,
            system TEXT,
            protocol TEXT,
	    variant TEXT,
	    version TEXT,
	    subject TEXT,
	    status INTEGER,
	    rt INTEGER,
	    trialinfo JSONB,
	    sys_time timestamp default current_timestamp	    
        );
    }
    postgres::exec $conn $stmt

    # Create prepared insert statement to make updates more efficient
    set stmt {
	INSERT INTO trial (host, blockid, trialid, system, protocol, variant, version, subject, status, rt, trialinfo) \
	    VALUES (($1), ($2), ($3), ($4), ($5), ($6), ($7), ($8), ($9), ($10), ($11))
    }
    postgres::prepare $conn $insert_trialinfo_cmd $stmt 1

    
    # Create the 'essvars' table if it does not exist
    set stmt {
        CREATE TABLE IF NOT EXISTS ess (
	    host VARCHAR(256),
            key TEXT UNIQUE,
            value TEXT,
	    sys_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
	    primary key (host, key)
        );
    }
    postgres::exec $conn $stmt

    # Create prepared insert statement to make updates more efficient
    set stmt {
	INSERT INTO ess (host, key, value) VALUES(($1), ($2), ($3))
	ON CONFLICT (key)
	DO UPDATE SET host = ($1), key = ($2), value = ($3);
    }
    postgres::prepare $conn $insert_ess_setting_cmd $stmt 1
}

proc process_ess { dpoint data } {
    global conn insert_trialinfo_cmd insert_ess_setting_cmd

    set host [dservGet ess/hostaddr]
    
    if { [string equal $dpoint ess/trialinfo] } {
	set d [::yajl::json2dict $data]
	
	foreach v "blockid trialid system protocol variant version subject status rt" {
	    set $v [dict get $d $v]
	}
	
	postgres::exec_prepared $conn $insert_trialinfo_cmd \
	    $host $blockid $trialid $system $protocol $variant $version $subject $status $rt $data
    } else {
	set key [string range $dpoint 4 end]
	postgres::exec_prepared $conn $insert_ess_setting_cmd $host $key $data
    }
}

setup_database $dbname

# insert initial settings into the ESS table
dservTouch ess/ipaddr
dservTouch ess/hostaddr

puts "PostgreSQL DB ready"

dservAddMatch   ess/*
dpointSetScript ess/* process_ess
