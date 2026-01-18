#
# dfconf.tcl - Datafile management subprocess
#
# Responsibilities:
#   - Index/catalog saved datafiles (.ess)
#   - Post-close processing hooks (emcalib transforms, etc.)
#   - Load and process .ess files for export
#   - Format conversion (JSON, Arrow, CSV)
#

package require dlsh
package require qpcs
package require sqlite3
package require yajltcl
package require dslog

# Configuration
set df_db_path "$dspath/db/datafiles.db"

# ess_dir will be set when we get ess/data_dir datapoint
set ess_dir ""

# Track current datafile for close detection
set current_datafile ""

# Cache for loaded system processors
set loaded_processors {}

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
            n_trials INTEGER,
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
# Data directory handling
#

proc process_data_dir {dpoint data} {
    variable ess_dir
    
    if {$data eq ""} {
        return
    }
    
    set ess_dir $data
    puts "Data directory set to: $ess_dir"
    
    # Scan for datafiles now that we have the directory
    if {[file exists $ess_dir]} {
        puts "Scanning for existing datafiles in $ess_dir..."
        set count [scan_datafiles]
        puts "Indexed $count new datafiles"
    }
}

proc get_ess_dir {} {
    variable ess_dir
    
    # Return cached value if set
    if {$ess_dir ne ""} {
        return $ess_dir
    }
    
    # Try datapoint
    if {[catch {set dir [dservGet ess/data_dir]}] == 0 && $dir ne ""} {
        set ess_dir $dir
        return $ess_dir
    }
    
    # Fall back to environment variable
    if {[info exists ::env(ESS_DATA_DIR)]} {
        set ess_dir $::env(ESS_DATA_DIR)
        return $ess_dir
    }
    
    # Default
    set ess_dir /tmp/essdat
    return $ess_dir
}

#
# Metadata extraction from .ess files
#

proc extract_datafile_metadata {filepath} {
    # Initialize with defaults
    array set meta {
        subject "" system "" protocol "" variant ""
        n_trials 0 date "" time "" timestamp 0 file_size 0
    }
    
    # Get file size
    if {[file exists $filepath]} {
        set meta(file_size) [file size $filepath]
    }
    
    # Read the ess file
    if {[catch {
        set g [dslog::readESS $filepath]
    } err]} {
        puts "Warning: Could not read $filepath: $err"
        return [array get meta]
    }
    
    # Extract pre-event info and data
    # e_pre contains pairs: {type subtype timestamp} {payload}
    dl_local pre_einfo [dl_unpack [dl_choose $g:e_pre [dl_llist 0]]]
    dl_local pre_edata [dl_unpack [dl_choose $g:e_pre [dl_llist 1]]]
    dl_local pre_types [dl_unpack [dl_choose $pre_einfo [dl_llist 0]]]
    dl_local pre_subtypes [dl_unpack [dl_choose $pre_einfo [dl_llist 1]]]
    
    # Event type 8 = TIME
    #   Subtype 0 = file open timestamp (payload is unix seconds)
    dl_local time_evt [dl_and [dl_eq $pre_types 8] [dl_eq $pre_subtypes 0]]
    if {[dl_sum $time_evt] > 0} {
        set unix_ts [dl_tcllist [dl_first [dl_select $pre_edata $time_evt]]]
        set meta(date) [clock format $unix_ts -format "%Y-%m-%d"]
        set meta(time) [clock format $unix_ts -format "%H:%M:%S"]
        set meta(timestamp) $unix_ts
    }
    
    # Fallback: extract date from filename if TIME event not found
    if {$meta(date) eq ""} {
        set filename [file rootname [file tail $filepath]]
        if {[regexp {(\d{8})-(\d{6})} $filename -> date time]} {
            set meta(date) "[string range $date 0 3]-[string range $date 4 5]-[string range $date 6 7]"
            set meta(time) "[string range $time 0 1]:[string range $time 2 3]:[string range $time 4 5]"
        } elseif {[regexp {(\d{8})} $filename -> date]} {
            set meta(date) "[string range $date 0 3]-[string range $date 4 5]-[string range $date 6 7]"
        } else {
            # Last resort: file modification time
            set mtime [file mtime $filepath]
            set meta(date) [clock format $mtime -format "%Y-%m-%d"]
            set meta(time) [clock format $mtime -format "%H:%M:%S"]
            set meta(timestamp) $mtime
        }
    }
    
    # Event type 18 = ID
    #   Subtype 0 = ESS (system name, legacy)
    #   Subtype 1 = SUBJECT  
    #   Subtype 2 = PROTOCOL (system:protocol)
    #   Subtype 3 = VARIANT (system:protocol:variant)
    
    # Get system:protocol:variant from ID/VARIANT event (18/3)
    dl_local variant_evt [dl_and [dl_eq $pre_types 18] [dl_eq $pre_subtypes 3]]
    if {[dl_sum $variant_evt] > 0} {
        set sysinfo [dl_tcllist [dl_first [dl_select $pre_edata $variant_evt]]]
        set parts [split $sysinfo ":"]
        if {[llength $parts] >= 3} {
            set meta(system) [lindex $parts 0]
            set meta(protocol) [lindex $parts 1]
            set meta(variant) [lindex $parts 2]
        } elseif {[llength $parts] == 2} {
            set meta(system) [lindex $parts 0]
            set meta(protocol) [lindex $parts 1]
        }
    }
    
    # Fallback: get system from ID/ESS event (18/0) if not already set
    if {$meta(system) eq ""} {
        dl_local ess_evt [dl_and [dl_eq $pre_types 18] [dl_eq $pre_subtypes 0]]
        if {[dl_sum $ess_evt] > 0} {
            set meta(system) [dl_tcllist [dl_first [dl_select $pre_edata $ess_evt]]]
        }
    }
    
    # Get subject from ID/SUBJECT event (18/1)
    dl_local subject_evt [dl_and [dl_eq $pre_types 18] [dl_eq $pre_subtypes 1]]
    if {[dl_sum $subject_evt] > 0} {
        set meta(subject) [dl_tcllist [dl_first [dl_select $pre_edata $subject_evt]]]
    }
    
    # Count obs periods - length of e_types
    if {[dl_exists $g:e_types]} {
        set meta(n_trials) [dl_length $g:e_types]
    }
    
    dg_delete $g
    
    return [array get meta]
}

#
# Datafile indexing
#

proc scan_datafiles { {dir ""} } {
    if {$dir eq ""} {
        set dir [get_ess_dir]
    }
    
    if {$dir eq "" || ![file exists $dir]} {
        puts "Data directory does not exist or not configured: $dir"
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
    
    # Extract metadata from file
    array set meta [extract_datafile_metadata $filepath]
    
    dfdb eval {
        INSERT INTO datafiles (filename, filepath, subject, system, protocol, variant, 
                               n_trials, date, time, timestamp, file_size)
        VALUES ($filename, $filepath, 
                $meta(subject), $meta(system), $meta(protocol), $meta(variant),
                $meta(n_trials), $meta(date), $meta(time), $meta(timestamp), $meta(file_size))
    }
    
    puts "Indexed: $filename ($meta(system)/$meta(protocol), $meta(n_trials) trials)"
    return 1
}

proc reindex_datafile {filename} {
    # Re-extract metadata and update database
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    
    if {$filepath eq ""} {
        return 0
    }
    
    array set meta [extract_datafile_metadata $filepath]
    
    dfdb eval {
        UPDATE datafiles SET
            subject = $meta(subject),
            system = $meta(system),
            protocol = $meta(protocol),
            variant = $meta(variant),
            n_trials = $meta(n_trials),
            date = $meta(date),
            time = $meta(time),
            timestamp = $meta(timestamp),
            file_size = $meta(file_size)
        WHERE filename = $filename
    }
    
    return 1
}

proc reindex_all {} {
    # Re-extract metadata for all indexed files
    set count 0
    dfdb eval {SELECT filename FROM datafiles} {
        if {[reindex_datafile $filename]} {
            incr count
        }
    }
    puts "Reindexed $count datafiles"
    return $count
}

proc remove_missing {} {
    # Remove entries for files that no longer exist
    set count 0
    dfdb eval {SELECT id, filename, filepath FROM datafiles} values {
        if {![file exists $values(filepath)]} {
            dfdb eval {DELETE FROM datafiles WHERE id = $values(id)}
            puts "Removed missing file: $values(filename)"
            incr count
        }
    }
    puts "Removed $count missing datafiles"
    return $count
}

#
# Datafile close detection and post-processing hooks
#

proc process_ess_datafile {dpoint data} {
    variable current_datafile
    
    set new_datafile $data
    
    # Detect file close: was something, now empty
    if {$current_datafile ne "" && $new_datafile eq ""} {
        on_datafile_closed $current_datafile
    }
    
    set current_datafile $new_datafile
}

proc on_datafile_closed {filepath} {
    puts "Datafile closed: $filepath"
    
    # Index the file
    index_datafile $filepath
    
    # Get file metadata for processor lookup
    array set meta [extract_datafile_metadata $filepath]
    
    if {$meta(system) eq ""} {
        puts "Warning: Could not determine system for $filepath"
        return
    }
    
    # Run system-level post-close hook if defined
    run_post_close_hook $filepath $meta(system)
}

proc run_post_close_hook {filepath system} {
    set hook_proc "${system}_on_file_close"
    
    if {[load_system_processor $system]} {
        if {[info commands $hook_proc] ne ""} {
            puts "Running post-close hook: $hook_proc"
            if {[catch {
                $hook_proc $filepath
                log_processing $filepath $hook_proc "success" ""
            } err]} {
                puts "Error in post-close hook: $err"
                log_processing $filepath $hook_proc "error" $err
            }
        }
    }
}

proc load_system_processor {system} {
    variable loaded_processors
    
    # Already loaded?
    if {$system in $loaded_processors} {
        return 1
    }
    
    # Find processor file
    set system_path [dservGet ess/system_path]
    set project [dservGet ess/project]
    if {$project eq ""} { set project "ess" }
    
    set process_file [file join $system_path $project $system ${system}_process.tcl]
    
    if {![file exists $process_file]} {
        puts "No processor found for system $system: $process_file"
        return 0
    }
    
    if {[catch {source $process_file} err]} {
        puts "Error loading processor for $system: $err"
        return 0
    }
    
    lappend loaded_processors $system
    puts "Loaded processor for system: $system"
    return 1
}

proc log_processing {filepath proc_name status message} {
    set filename [file tail $filepath]
    
    set file_id [dfdb eval {
        SELECT id FROM datafiles WHERE filename = $filename
    }]
    
    if {$file_id ne ""} {
        dfdb eval {
            INSERT INTO processing_log (datafile_id, processor, status, message)
            VALUES ($file_id, $proc_name, $status, $message)
        }
    }
}

#
# Datafile listing and querying (for frontend)
#

proc get_datafile_list { {filters {}} } {
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
    
    if {[dict exists $filters date_from] && [dict get $filters date_from] ne ""} {
        set df [dict get $filters date_from]
        lappend where_clauses "date >= '$df'"
    }
    
    if {[dict exists $filters date_to] && [dict get $filters date_to] ne ""} {
        set dt [dict get $filters date_to]
        lappend where_clauses "date <= '$dt'"
    }
    
    if {[dict exists $filters search] && [dict get $filters search] ne ""} {
        set search [dict get $filters search]
        lappend where_clauses "(filename LIKE '%$search%' OR subject LIKE '%$search%')"
    }
    
    set sql "SELECT id, filename, subject, system, protocol, variant, n_trials, date, time, timestamp, file_size, processed, synced FROM datafiles"
    
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
        $json string "subject" string $values(subject)
        $json string "system" string $values(system)
        $json string "protocol" string $values(protocol)
        $json string "variant" string $values(variant)
        $json string "n_trials" number $values(n_trials)
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
    # Return available filter values for frontend dropdowns
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
        $json map_open
        foreach col {id filename filepath subject system protocol variant n_trials date time timestamp file_size processed synced} {
            $json string $col
            if {$col in {id n_trials timestamp file_size processed synced}} {
                $json number $values($col)
            } else {
                $json string $values($col)
            }
        }
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

#
# Datafile processing and export
#

proc process_datafile {filename} {
    # Look up file info
    set found 0
    dfdb eval {SELECT filepath, system, protocol, variant FROM datafiles WHERE filename = $filename} values {
        set found 1
        set filepath $values(filepath)
        set system $values(system)
        set protocol $values(protocol)
        set variant $values(variant)
    }
    
    if {!$found} {
        error "File not found in catalog: $filename"
    }
    
    # Load system processor
    if {![load_system_processor $system]} {
        error "No processor available for system: $system"
    }
    
    # Run system-level processing
    set process_proc "${system}_process_datafile"
    if {[info commands $process_proc] eq ""} {
        error "System processor not defined: $process_proc"
    }
    
    set g [$process_proc $filepath]
    
    # Run protocol-level processing if available
    set protocol_proc "${protocol}_process_datafile"
    if {[info commands $protocol_proc] ne ""} {
        set g [$protocol_proc $g]
    }
    
    # Mark as processed in database
    dfdb eval {UPDATE datafiles SET processed = 1 WHERE filename = $filename}
    
    return $g
}

proc export_datafile {filename format} {
    set g [process_datafile $filename]
    
    set result [export_dg $g $format]
    
    dg_delete $g
    return $result
}

proc export_datafiles {filenames format} {
    # Process and concatenate multiple files
    set combined ""
    set errors {}
    
    foreach filename $filenames {
        if {[catch {
            set g [process_datafile $filename]
        } err]} {
            puts "Warning: Failed to process $filename: $err"
            lappend errors [list $filename $err]
            continue
        }
        
        # Add filename column for identification
        set nrows [dl_length $g:[lindex [dg_tclListnames $g] 0]]
        dl_set $g:source_file [dl_replicate [dl_slist $filename] $nrows]
        
        if {$combined eq ""} {
            set combined $g
        } else {
            # Append columns from g to combined
            foreach col [dg_tclListnames $g] {
                if {[dl_exists $combined:$col]} {
                    dl_append $combined:$col $g:$col
                }
            }
            dg_delete $g
        }
    }
    
    if {$combined eq ""} {
        error "No files processed successfully. Errors: $errors"
    }
    
    set result [export_dg $combined $format]
    
    dg_delete $combined
    return $result
}

proc export_dg {g format} {
    switch $format {
        json {
            return [dg_toJSON $g]
        }
        arrow {
            return [dg_toArrow $g]
        }
        csv {
            return [dg_toCSV $g]
        }
        default {
            error "Unknown export format: $format"
        }
    }
}

#
# Utility procs
#

proc get_export_formats {} {
    return {["json", "arrow", "csv"]}
}

proc get_stats {} {
    set json [yajl create #auto]
    $json map_open
    
    set total [dfdb eval {SELECT COUNT(*) FROM datafiles}]
    set processed [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE processed = 1}]
    set synced [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE synced = 1}]
    set total_trials [dfdb eval {SELECT SUM(n_trials) FROM datafiles}]
    set total_size [dfdb eval {SELECT SUM(file_size) FROM datafiles}]
    
    $json string "total_files" number $total
    $json string "processed_files" number $processed
    $json string "synced_files" number $synced
    $json string "total_trials" number [expr {$total_trials ne "" ? $total_trials : 0}]
    $json string "total_size_bytes" number [expr {$total_size ne "" ? $total_size : 0}]
    
    $json map_close
    set result [$json get]
    $json delete
    
    return $result
}

#
# Manual commands for testing/maintenance
#

proc rescan {} {
    # Full rescan: remove missing, index new
    remove_missing
    scan_datafiles
}

proc list_files { {limit 20} } {
    # Simple list for terminal use
    dfdb eval {SELECT filename, subject, system, protocol, n_trials, date FROM datafiles ORDER BY timestamp DESC LIMIT $limit} values {
        puts [format "%-40s %-10s %-15s %-15s %4d trials  %s" \
            $values(filename) $values(subject) $values(system) $values(protocol) $values(n_trials) $values(date)]
    }
}

#
# Initialize
#

setup_database $df_db_path

# Subscribe to datafile changes for close detection
dservAddExactMatch ess/datafile
dpointSetScript    ess/datafile process_ess_datafile

# Subscribe to data directory - will trigger scan when received
dservAddExactMatch ess/data_dir
dpointSetScript    ess/data_dir process_data_dir

# Touch to get current value (fails silently if not yet set)
dservTouch ess/data_dir

puts "Datafile manager ready (df)"
