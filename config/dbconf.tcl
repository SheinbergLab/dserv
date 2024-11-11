set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require qpcs
package require postgres
package require yajltcl

set conn -1;		       # connection to our postgresql server
set dbname mydb;	       # name of database to write to
set insert_cmd "insert_value"; # this is prepared ahead of time to call repeatedly

# Function to handle database setup and corruption detection
proc setup_database { db } {
    global conn dbname insert_cmd
    set conninfo "dbname=$db user=postgres password=postgres host=localhost port=5432"
    if { [catch { set conn [postgres::connect $conninfo] } error] } {
	puts $error
	set conn -1
    }
    
    # Create the 'trial' table if it does not exist
    set stmt {
        CREATE TABLE IF NOT EXISTS trial (
  	    id INTEGER primary key generated always as identity,
            blockid INTEGER,
            trialid INTEGER,
            system TEXT,
            protocol TEXT,
	    variant TEXT,
	    subject TEXT,
	    status INTEGER,
	    rt INTEGER,
	    stiminfo TEXT,
	    sys_time timestamp default current_timestamp	    
        );
    }
    postgres::exec $conn $stmt

    # Create prepared insert statement to make updates more efficient
    set stmt {
	INSERT INTO trial (blockid, trialid, system, protocol, variant, subject, status, rt, stiminfo) \
	    VALUES (($1), ($2), ($3), ($4), ($5), ($6), ($7), ($8), ($9))
    }
    postgres::prepare $conn $insert_cmd $stmt 1
    
}

proc process_trial { dpoint trialinfo } {
    global conn insert_cmd

    set d [::yajl::json2dict $trialinfo]

    foreach v "blockid trialid system protocol variant subject status rt" {
	set $v [dict get $d $v]
    }

    postgres::exec_prepared $conn $insert_cmd \
	$blockid $trialid $system $protocol $variant $subject $status $rt $trialinfo
}

setup_database $dbname
puts "PostgreSQL DB ready"

dservAddExactMatch ess/trialinfo
dpointSetScript    ess/trialinfo process_trial
