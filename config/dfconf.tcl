#
# dfconf.tcl - Datafile management subprocess
#
# Uses df 2.0 module for core functionality, adds:
#   - SQLite catalog/index with column tracking
#   - Datapoint subscriptions for auto-indexing
#   - Post-close hooks via system extractors
#   - Query API for frontend
#

package require dlsh
package require qpcs
package require sqlite3
package require yajltcl

tcl::tm::add $dspath/lib
package require df 2.0

# Configuration
set df_db_path "$dspath/db/datafiles.db"

# Paths will be set when we get datapoints
set ess_dir ""
set ess_system_path ""
set ess_project ""

# Track current datafile for close detection
set current_datafile ""

# Disable exit
proc exit {args} { error "exit not available for this subprocess" }

#
# Database setup
#

proc setup_database {db_path} {
    set dir_path [file dirname $db_path]
    if {![file exists $dir_path]} {
        file mkdir $dir_path
    }
    
    if {[file exists $db_path]} {
        set open_status [catch {
            sqlite3 dfdb $db_path
            set result [dfdb eval {PRAGMA integrity_check;}]
        } errMsg]
        if {$open_status || $result ne "ok"} {
            puts "Datafiles database corrupted, renaming and recreating."
            file rename -force $db_path "$db_path.corrupt"
            sqlite3 dfdb $db_path
        }
    } else {
        sqlite3 dfdb $db_path
    }
    
    # Datafile catalog
    dfdb eval {
        CREATE TABLE IF NOT EXISTS datafiles (
            id INTEGER PRIMARY KEY,
            filename TEXT UNIQUE,
            filepath TEXT,
            subject TEXT,
            system TEXT,
            protocol TEXT,
            variant TEXT,
            n_obs INTEGER,
            date TEXT,
            time TEXT,
            timestamp INTEGER,
            file_size INTEGER,
            created_at TEXT DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
            processed INTEGER DEFAULT 0,
            synced INTEGER DEFAULT 0
        );
    }
    
    # Indices for common queries
    dfdb eval {
        CREATE INDEX IF NOT EXISTS idx_datafiles_subject ON datafiles (subject);
        CREATE INDEX IF NOT EXISTS idx_datafiles_system ON datafiles (system);
        CREATE INDEX IF NOT EXISTS idx_datafiles_date ON datafiles (date);
        CREATE INDEX IF NOT EXISTS idx_datafiles_timestamp ON datafiles (timestamp);
        CREATE INDEX IF NOT EXISTS idx_datafiles_system_protocol ON datafiles (system, protocol, variant);
    }
    
    # Column info table - tracks what data each file contains
    dfdb eval {
        CREATE TABLE IF NOT EXISTS file_columns (
            id INTEGER PRIMARY KEY,
            datafile_id INTEGER,
            column_name TEXT,
            column_type TEXT,
            column_depth INTEGER,
            column_length INTEGER,
            column_category TEXT,
            FOREIGN KEY (datafile_id) REFERENCES datafiles(id) ON DELETE CASCADE,
            UNIQUE(datafile_id, column_name)
        );
        
        CREATE INDEX IF NOT EXISTS idx_file_columns_datafile ON file_columns(datafile_id);
        CREATE INDEX IF NOT EXISTS idx_file_columns_category ON file_columns(column_category);
    }
    
    # Processing log - track post-close hook executions
    dfdb eval {
        CREATE TABLE IF NOT EXISTS processing_log (
            id INTEGER PRIMARY KEY,
            datafile_id INTEGER,
            processor TEXT,
            status TEXT,
            message TEXT,
            processed_at TEXT DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
            FOREIGN KEY (datafile_id) REFERENCES datafiles(id)
        );
    }
    
    dfdb eval {
        PRAGMA synchronous = NORMAL;
        PRAGMA journal_mode = WAL;
        PRAGMA temp_store = MEMORY;
    }
}

#
# Configuration datapoint callbacks
#

proc process_data_dir {dpoint data} {
    global ess_dir
    
    if {$data eq ""} {
        return
    }
    
    set ess_dir $data
    puts "dfconf: Data directory set to: $ess_dir"
    
    # Scan for datafiles now that we have the directory
    if {[file exists $ess_dir]} {
        puts "dfconf: Scanning for existing datafiles in $ess_dir..."
        set count [scan_datafiles]
        puts "dfconf: Indexed $count new datafiles"
    }
}

proc process_system_path {dpoint data} {
    global ess_system_path ess_project
    
    if {$data eq ""} {
        return
    }
    
    set ess_system_path $data
    puts "dfconf: ESS system path set to: $ess_system_path"
    
    # Update df::ess_root if we have both pieces
    update_ess_root
}

proc process_project {dpoint data} {
    global ess_system_path ess_project
    
    if {$data eq ""} {
        return
    }
    
    set ess_project $data
    puts "dfconf: ESS project set to: $ess_project"
    
    # Update df::ess_root if we have both pieces
    update_ess_root
}

proc update_ess_root {} {
    global ess_system_path ess_project
    
    if {$ess_system_path ne "" && $ess_project ne ""} {
        set ess_root [file join $ess_system_path $ess_project]
        df::set_ess_root $ess_root
        puts "dfconf: ESS root set to: $ess_root"
    }
}

proc get_ess_dir {} {
    global ess_dir
    
    if {$ess_dir ne ""} {
        return $ess_dir
    }
    
    if {[catch {set dir [dservGet ess/data_dir]}] == 0 && $dir ne ""} {
        set ess_dir $dir
        return $ess_dir
    }
    
    if {[info exists ::env(ESS_DATA_DIR)]} {
        set ess_dir $::env(ESS_DATA_DIR)
        return $ess_dir
    }
    
    set ess_dir /tmp/essdat
    return $ess_dir
}

#
# Datafile indexing
#

proc scan_datafiles { {dir ""} } {
    if {$dir eq ""} {
        set dir [get_ess_dir]
    }
    
    if {$dir eq "" || ![file exists $dir]} {
        puts "dfconf: Data directory does not exist or not configured: $dir"
        return 0
    }
    
    set count 0
    foreach f [glob -nocomplain -directory $dir *.ess] {
        if {[index_datafile $f]} {
            incr count
        }
    }
    
    return $count
}

proc index_datafile {filepath} {
    set filename [file tail $filepath]
    
    # Check if already indexed
    set exists [dfdb eval {
        SELECT COUNT(*) FROM datafiles WHERE filename = $filename
    }]
    
    if {$exists > 0} {
        return 0
    }
    
    # Extract metadata using df module
    set meta [df::metadata $filepath]
    
    # Insert file record
    set meta_subject [dict get $meta subject]
    set meta_system [dict get $meta system]
    set meta_protocol [dict get $meta protocol]
    set meta_variant [dict get $meta variant]
    set meta_n_obs [dict get $meta n_obs]
    set meta_date [dict get $meta date]
    set meta_time [dict get $meta time]
    set meta_timestamp [dict get $meta timestamp]
    set meta_file_size [dict get $meta file_size]
    
    dfdb eval {
        INSERT INTO datafiles (filename, filepath, subject, system, protocol, variant, 
                               n_obs, date, time, timestamp, file_size)
        VALUES ($filename, $filepath, 
                $meta_subject, $meta_system, $meta_protocol, $meta_variant,
                $meta_n_obs, $meta_date, $meta_time, $meta_timestamp, $meta_file_size)
    }
    
    set file_id [dfdb last_insert_rowid]
    
    # Insert column information
    set col_info [dict get $meta column_info]
    dict for {col_name info} $col_info {
        set col_type [dict get $info type]
        set col_depth [dict get $info depth]
        set col_length [dict get $info length]
        
        # Determine category
        if {[string match "<stimdg>*" $col_name]} {
            set category "stimdg"
        } elseif {[string match "<ds>*" $col_name]} {
            set category "datastream"
        } elseif {[string match "e_*" $col_name]} {
            set category "event"
        } else {
            set category "other"
        }
        
        dfdb eval {
            INSERT INTO file_columns (datafile_id, column_name, column_type, 
                                      column_depth, column_length, column_category)
            VALUES ($file_id, $col_name, $col_type, $col_depth, $col_length, $category)
        }
    }
    
    puts "dfconf: Indexed: $filename ($meta_system/$meta_protocol, $meta_n_obs obs)"
    return 1
}

proc reindex_datafile {filename} {
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    
    if {$filepath eq ""} {
        return 0
    }
    
    set meta [df::metadata $filepath]
    
    set meta_subject [dict get $meta subject]
    set meta_system [dict get $meta system]
    set meta_protocol [dict get $meta protocol]
    set meta_variant [dict get $meta variant]
    set meta_n_obs [dict get $meta n_obs]
    set meta_date [dict get $meta date]
    set meta_time [dict get $meta time]
    set meta_timestamp [dict get $meta timestamp]
    set meta_file_size [dict get $meta file_size]
    
    dfdb eval {
        UPDATE datafiles SET
            subject = $meta_subject,
            system = $meta_system,
            protocol = $meta_protocol,
            variant = $meta_variant,
            n_obs = $meta_n_obs,
            date = $meta_date,
            time = $meta_time,
            timestamp = $meta_timestamp,
            file_size = $meta_file_size
        WHERE filename = $filename
    }
    
    # Update column info
    set file_id [dfdb eval {SELECT id FROM datafiles WHERE filename = $filename}]
    dfdb eval {DELETE FROM file_columns WHERE datafile_id = $file_id}
    
    set col_info [dict get $meta column_info]
    dict for {col_name info} $col_info {
        set col_type [dict get $info type]
        set col_depth [dict get $info depth]
        set col_length [dict get $info length]
        
        if {[string match "<stimdg>*" $col_name]} {
            set category "stimdg"
        } elseif {[string match "<ds>*" $col_name]} {
            set category "datastream"
        } elseif {[string match "e_*" $col_name]} {
            set category "event"
        } else {
            set category "other"
        }
        
        dfdb eval {
            INSERT INTO file_columns (datafile_id, column_name, column_type, 
                                      column_depth, column_length, column_category)
            VALUES ($file_id, $col_name, $col_type, $col_depth, $col_length, $category)
        }
    }
    
    return 1
}

proc reindex_all {} {
    set count 0
    dfdb eval {SELECT filename FROM datafiles} {
        if {[reindex_datafile $filename]} {
            incr count
        }
    }
    puts "dfconf: Reindexed $count datafiles"
    return $count
}

proc remove_missing {} {
    set count 0
    dfdb eval {SELECT id, filename, filepath FROM datafiles} values {
        if {![file exists $values(filepath)]} {
            dfdb eval {DELETE FROM datafiles WHERE id = $values(id)}
            puts "dfconf: Removed missing file: $values(filename)"
            incr count
        }
    }
    puts "dfconf: Removed $count missing datafiles"
    return $count
}

#
# Datafile close detection and post-processing
#

proc process_ess_datafile {dpoint data} {
    global current_datafile
    
    set new_datafile $data
    
    # Detect file close: was something, now empty
    if {$current_datafile ne "" && $new_datafile eq ""} {
        on_datafile_closed $current_datafile
    }
    
    set current_datafile $new_datafile
}

proc on_datafile_closed {filepath} {
    puts "dfconf: Datafile closed: $filepath"
    
    # Index the file
    index_datafile $filepath
    
    # Get file metadata for processor lookup
    set meta [df::metadata $filepath]
    
    set system [dict get $meta system]
    set protocol [dict get $meta protocol]
    
    if {$system eq ""} {
        puts "dfconf: Warning: Could not determine system for $filepath"
        return
    }
    
    # Log the close event
    set filename [file tail $filepath]
    set file_id [dfdb eval {SELECT id FROM datafiles WHERE filename = $filename}]
    
    if {$file_id ne ""} {
        log_processing $file_id "close_detected" "success" "system=$system protocol=$protocol"
    }
    
    # Publish close event for other subsystems
    dservSet df/file_closed [dict create \
        filepath $filepath \
        filename $filename \
        system $system \
        protocol $protocol \
        n_obs [dict get $meta n_obs] \
        timestamp [clock seconds]]
}

proc log_processing {file_id processor status message} {
    dfdb eval {
        INSERT INTO processing_log (datafile_id, processor, status, message)
        VALUES ($file_id, $processor, $status, $message)
    }
}

#
# Query functions for frontend
#

proc list_datafiles { {filters {}} } {
    set where_clauses {}
    
    if {[dict exists $filters subject] && [dict get $filters subject] ne ""} {
        set subj [dict get $filters subject]
        lappend where_clauses "subject = '$subj'"
    }
    
    if {[dict exists $filters system] && [dict get $filters system] ne ""} {
        set sys [dict get $filters system]
        lappend where_clauses "system = '$sys'"
    }
    
    if {[dict exists $filters protocol] && [dict get $filters protocol] ne ""} {
        set proto [dict get $filters protocol]
        lappend where_clauses "protocol = '$proto'"
    }
    
    if {[dict exists $filters variant] && [dict get $filters variant] ne ""} {
        set var [dict get $filters variant]
        lappend where_clauses "variant = '$var'"
    }
    
    if {[dict exists $filters date] && [dict get $filters date] ne ""} {
        set dt [dict get $filters date]
        lappend where_clauses "date = '$dt'"
    }
    
    if {[dict exists $filters after] && [dict get $filters after] ne ""} {
        set after [dict get $filters after]
        lappend where_clauses "timestamp >= $after"
    }
    
    if {[dict exists $filters before] && [dict get $filters before] ne ""} {
        set before [dict get $filters before]
        lappend where_clauses "timestamp <= $before"
    }
    
    set sql "SELECT id, filename, filepath, subject, system, protocol, variant, 
                    n_obs, date, time, timestamp, file_size, processed, synced
             FROM datafiles"
    
    if {[llength $where_clauses] > 0} {
        append sql " WHERE [join $where_clauses { AND }]"
    }
    
    append sql " ORDER BY timestamp DESC, created_at DESC"
    
    if {[dict exists $filters limit]} {
        append sql " LIMIT [dict get $filters limit]"
    }
    
    if {[dict exists $filters offset]} {
        append sql " OFFSET [dict get $filters offset]"
    }
    
    # Build JSON result
    set json [yajl create #auto]
    $json array_open
    
    dfdb eval $sql values {
        $json map_open
        $json string "id" number $values(id)
        $json string "filename" string $values(filename)
        $json string "filepath" string $values(filepath)
        $json string "subject" string $values(subject)
        $json string "system" string $values(system)
        $json string "protocol" string $values(protocol)
        $json string "variant" string $values(variant)
        $json string "n_obs" number $values(n_obs)
        $json string "date" string $values(date)
        $json string "time" string $values(time)
        $json string "timestamp" number $values(timestamp)
        $json string "file_size" number $values(file_size)
        $json string "processed" number $values(processed)
        $json string "synced" number $values(synced)
        $json map_close
    }
    
    $json array_close
    set result [$json get]
    $json delete
    
    return $result
}

proc get_datafile_count { {filters {}} } {
    set where_clauses {}
    
    if {[dict exists $filters subject] && [dict get $filters subject] ne ""} {
        set subj [dict get $filters subject]
        lappend where_clauses "subject = '$subj'"
    }
    
    if {[dict exists $filters system] && [dict get $filters system] ne ""} {
        set sys [dict get $filters system]
        lappend where_clauses "system = '$sys'"
    }
    
    if {[dict exists $filters protocol] && [dict get $filters protocol] ne ""} {
        set proto [dict get $filters protocol]
        lappend where_clauses "protocol = '$proto'"
    }
    
    set sql "SELECT COUNT(*) FROM datafiles"
    
    if {[llength $where_clauses] > 0} {
        append sql " WHERE [join $where_clauses { AND }]"
    }
    
    return [dfdb eval $sql]
}

proc get_filter_options {} {
    set json [yajl create #auto]
    $json map_open
    
    # Subjects
    $json string "subjects" array_open
    dfdb eval {SELECT DISTINCT subject FROM datafiles WHERE subject != '' ORDER BY subject} {
        $json string $subject
    }
    $json array_close
    
    # Systems
    $json string "systems" array_open
    dfdb eval {SELECT DISTINCT system FROM datafiles WHERE system != '' ORDER BY system} {
        $json string $system
    }
    $json array_close
    
    # Protocols
    $json string "protocols" array_open
    dfdb eval {SELECT DISTINCT protocol FROM datafiles WHERE protocol != '' ORDER BY protocol} {
        $json string $protocol
    }
    $json array_close
    
    # Variants
    $json string "variants" array_open
    dfdb eval {SELECT DISTINCT variant FROM datafiles WHERE variant != '' ORDER BY variant} {
        $json string $variant
    }
    $json array_close
    
    # Date range
    $json string "date_range" map_open
    set min_date [dfdb eval {SELECT MIN(date) FROM datafiles}]
    set max_date [dfdb eval {SELECT MAX(date) FROM datafiles}]
    $json string "min" string [expr {$min_date ne "" ? $min_date : ""}]
    $json string "max" string [expr {$max_date ne "" ? $max_date : ""}]
    $json map_close
    
    $json map_close
    set result [$json get]
    $json delete
    
    return $result
}

proc get_datafile_info {filename} {
    set json [yajl create #auto]
    
    set found 0
    dfdb eval {SELECT * FROM datafiles WHERE filename = $filename} values {
        set found 1
        set file_id $values(id)
        
        $json map_open
        foreach col {id filename filepath subject system protocol variant n_obs date time timestamp file_size processed synced} {
            $json string $col
            if {$col in {id n_obs timestamp file_size processed synced}} {
                $json number $values($col)
            } else {
                $json string $values($col)
            }
        }
        
        # Add column info
        $json string "columns" array_open
        dfdb eval {
            SELECT column_name, column_type, column_depth, column_length, column_category
            FROM file_columns WHERE datafile_id = $file_id
        } col_values {
            $json map_open
            $json string "name" string $col_values(column_name)
            $json string "type" string $col_values(column_type)
            $json string "depth" number $col_values(column_depth)
            $json string "length" number $col_values(column_length)
            $json string "category" string $col_values(column_category)
            $json map_close
        }
        $json array_close
        
        $json map_close
    }
    
    if {!$found} {
        $json map_open
        $json string "error" string "File not found"
        $json map_close
    }
    
    set result [$json get]
    $json delete
    
    return $result
}

proc get_processing_log {filename} {
    set json [yajl create #auto]
    $json array_open
    
    set file_id [dfdb eval {SELECT id FROM datafiles WHERE filename = $filename}]
    
    if {$file_id ne ""} {
        dfdb eval {
            SELECT processor, status, message, processed_at 
            FROM processing_log 
            WHERE datafile_id = $file_id 
            ORDER BY processed_at DESC
        } values {
            $json map_open
            $json string "processor" string $values(processor)
            $json string "status" string $values(status)
            $json string "message" string $values(message)
            $json string "processed_at" string $values(processed_at)
            $json map_close
        }
    }
    
    $json array_close
    set result [$json get]
    $json delete
    
    return $result
}

proc get_stats {} {
    set json [yajl create #auto]
    $json map_open
    
    set total [dfdb eval {SELECT COUNT(*) FROM datafiles}]
    set processed [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE processed = 1}]
    set synced [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE synced = 1}]
    set total_obs [dfdb eval {SELECT SUM(n_obs) FROM datafiles}]
    set total_size [dfdb eval {SELECT SUM(file_size) FROM datafiles}]
    
    $json string "total_files" number $total
    $json string "processed_files" number $processed
    $json string "synced_files" number $synced
    $json string "total_obs" number [expr {$total_obs ne "" ? $total_obs : 0}]
    $json string "total_size_bytes" number [expr {$total_size ne "" ? $total_size : 0}]
    
    # By system breakdown
    $json string "by_system" array_open
    dfdb eval {
        SELECT system, COUNT(*) as cnt, SUM(n_obs) as obs 
        FROM datafiles GROUP BY system ORDER BY cnt DESC
    } {
        $json map_open
        $json string "system" string $system
        $json string "count" number $cnt
        $json string "n_obs" number [expr {$obs ne "" ? $obs : 0}]
        $json map_close
    }
    $json array_close
    
    # By subject breakdown
    $json string "by_subject" array_open
    dfdb eval {
        SELECT subject, COUNT(*) as cnt, SUM(n_obs) as obs 
        FROM datafiles GROUP BY subject ORDER BY cnt DESC
    } {
        $json map_open
        $json string "subject" string $subject
        $json string "count" number $cnt
        $json string "n_obs" number [expr {$obs ne "" ? $obs : 0}]
        $json map_close
    }
    $json array_close
    
    $json map_close
    set result [$json get]
    $json delete
    
    return $result
}

#
# Data loading - delegates to df module
#

proc load_trials {filename args} {
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    
    if {$filepath eq ""} {
        error "File not found in catalog: $filename"
    }
    
    set g [df::load_data $filepath {*}$args]
    
    # Mark as processed
    dfdb eval {UPDATE datafiles SET processed = 1 WHERE filename = $filename}
    
    return $g
}

#
# Manual commands for testing/maintenance
#

proc rescan {} {
    remove_missing
    scan_datafiles
}

proc list_files { {limit 20} } {
    dfdb eval {SELECT filename, subject, system, protocol, n_obs, date FROM datafiles ORDER BY timestamp DESC LIMIT $limit} values {
        puts [format "%-40s %-10s %-15s %-15s %4d obs  %s" \
            $values(filename) $values(subject) $values(system) $values(protocol) $values(n_obs) $values(date)]
    }
}

#
# Initialize
#

setup_database $df_db_path

# Subscribe to system path and project - will set ess_root for df module
dservAddExactMatch ess/system_path
dpointSetScript    ess/system_path process_system_path

dservAddExactMatch ess/project
dpointSetScript    ess/project process_project

# Subscribe to data directory - will trigger scan when received
dservAddExactMatch ess/data_dir
dpointSetScript    ess/data_dir process_data_dir

# Subscribe to datafile changes for close detection
dservAddExactMatch ess/datafile
dpointSetScript    ess/datafile process_ess_datafile

# Touch to get current values (fails silently if not yet set)
catch {dservTouch ess/system_path}
catch {dservTouch ess/project}
catch {dservTouch ess/data_dir}

puts "dfconf: Datafile manager ready (df 2.0)"
