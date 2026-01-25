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
package require dslog

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

proc process_sync_destination {dpoint data} {
    if {$data ne ""} {
        puts "dfconf: Sync destination set to: $data"
        configure_auto_sync $data extracted 1
    } else {
        puts "dfconf: Sync destination cleared"
        configure_auto_sync "" extracted 0
    }
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
    global ess_system_path ess_project
    
    puts "dfconf: Datafile closed: $filepath"
    
    # Index the file
    index_datafile $filepath
    
    # Get file metadata
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
    
    # Try to extract and analyze
    if {[df::get_ess_root] eq ""} {
        puts "dfconf: ESS root not set, skipping extraction"
        return
    }
    
    # Source analyzer if it exists
    set ess_root [df::get_ess_root]
    set analyzer_file [file join $ess_root $system ${system}_analyze.tcl]
    if {[file exists $analyzer_file]} {
        if {[catch {uplevel #0 [list source $analyzer_file]} err]} {
            puts "dfconf: Error sourcing analyzer $analyzer_file: $err"
        }
    }
    
    # Extract trials
    if {[catch {set trials [df::load_data $filepath]} err]} {
        puts "dfconf: Extract failed for $filepath: $err"
        if {$file_id ne ""} {
            log_processing $file_id "extract" "error" $err
        }
        return
    }
    
    if {$file_id ne ""} {
        log_processing $file_id "extract" "success" "trials extracted"
    }
    
    # Run system analyzer if exists
    set analyzer_proc "::${system}::analyze"
    if {[info commands $analyzer_proc] ne ""} {
        puts "dfconf: Running analyzer $analyzer_proc"
        if {[catch {set results [{*}$analyzer_proc $trials $filepath]} err]} {
            puts "dfconf: Analyze failed for $filepath: $err"
            if {$file_id ne ""} {
                log_processing $file_id "analyze" "error" $err
            }
        } else {
            puts "dfconf: Analysis complete for $filepath"
            if {$file_id ne ""} {
                log_processing $file_id "analyze" "success" "analysis complete"
            }
            
            # Mark as processed
            dfdb eval {UPDATE datafiles SET processed = 1 WHERE id = $file_id}
        }
    }

    # Auto-sync if enabled 
    auto_sync_file $filename
    
    # Clean up
    catch {dg_delete $trials}
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
    
    if {[dict exists $filters min_obs] && [dict get $filters min_obs] ne ""} {
        set minobs [dict get $filters min_obs]
        lappend where_clauses "n_obs >= $minobs"
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
    
    $json string "subjects" array_open
    dfdb eval {SELECT DISTINCT subject FROM datafiles WHERE subject != '' ORDER BY subject} {
        $json string $subject
    }
    $json array_close
    
    $json string "systems" array_open
    dfdb eval {SELECT DISTINCT system FROM datafiles WHERE system != '' ORDER BY system} {
        $json string $system
    }
    $json array_close
    
    $json string "protocols" array_open
    dfdb eval {SELECT DISTINCT protocol FROM datafiles WHERE protocol != '' ORDER BY protocol} {
        $json string $protocol
    }
    $json array_close
    
    $json map_close
    set result [$json get]
    $json delete
    
    return $result
}

proc get_stats {} {
    set total_files [dfdb eval {SELECT COUNT(*) FROM datafiles}]
    set total_obs [dfdb eval {SELECT COALESCE(SUM(n_obs), 0) FROM datafiles}]
    set processed_count [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE processed = 1}]
    set synced_count [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE synced = 1}]
    
    set json [yajl create #auto]
    $json map_open
    $json string "total_files" number $total_files
    $json string "total_obs" number $total_obs
    $json string "processed" number $processed_count
    $json string "synced" number $synced_count
    $json map_close
    set result [$json get]
    $json delete
    
    return $result
}

proc get_datafile_info {filename} {
    set result [dfdb eval {
        SELECT id, filename, filepath, subject, system, protocol, variant, 
               n_obs, date, time, timestamp, file_size, processed, synced
        FROM datafiles WHERE filename = $filename
    }]
    
    if {[llength $result] == 0} {
        return [_json_error "File not found: $filename"]
    }
    
    lassign $result id fn fp subj sys proto var n_obs date time ts fsize proc sync
    
    set json [yajl create #auto]
    $json map_open
    $json string "id" number $id
    $json string "filename" string $fn
    $json string "filepath" string $fp
    $json string "subject" string $subj
    $json string "system" string $sys
    $json string "protocol" string $proto
    $json string "variant" string $var
    $json string "n_obs" number $n_obs
    $json string "date" string $date
    $json string "time" string $time
    $json string "timestamp" number $ts
    $json string "file_size" number $fsize
    $json string "processed" number $proc
    $json string "synced" number $sync
    
    # Add column info
    $json string "columns" array_open
    dfdb eval {
        SELECT column_name, column_type, column_depth, column_length, column_category 
        FROM file_columns WHERE datafile_id = $id
    } col {
        $json map_open
        $json string "name" string $col(column_name)
        $json string "type" string $col(column_type)
        $json string "depth" number $col(column_depth)
        $json string "length" number $col(column_length)
        $json string "category" string $col(column_category)
        $json map_close
    }
    $json array_close
    
    $json map_close
    set result [$json get]
    $json delete
    
    return $result
}

#
# Rescan for frontend
#

proc rescan {} {
    remove_missing
    set count [scan_datafiles]
    
    set json [yajl create #auto]
    $json map_open
    $json string "status" string "ok"
    $json string "indexed" number $count
    $json map_close
    set result [$json get]
    $json delete
    
    return $result
}

# ============================================================================
# Temporary Path for Zip Downloads
# ============================================================================

#
# Get temporary path for zip creation (uses $dspath/tmp)
#
proc get_tmp_path {} {
    global dspath
    set tmp_path [file join $dspath tmp]
    if {![file exists $tmp_path]} {
        file mkdir $tmp_path
    }
    return $tmp_path
}

#
# Clean up old temp files (older than N hours)
#
proc cleanup_tmp {{hours 1}} {
    set tmp_path [get_tmp_path]
    set cutoff [expr {[clock seconds] - ($hours * 3600)}]
    set deleted 0
    
    foreach f [glob -nocomplain -directory $tmp_path *] {
        if {[file isfile $f] && [file mtime $f] < $cutoff} {
            catch {file delete $f}
            incr deleted
        }
    }
    
    return $deleted
}

# ============================================================================
# Single File Export (uses catalog to resolve filename -> filepath)
# ============================================================================

#
# Export by catalog filename (looks up filepath from SQLite)
#
proc export_file {filename level {output_dir ""}} {
    if {$output_dir eq ""} {
        set output_dir [get_tmp_path]
    }
    
    # Resolve filename to filepath via catalog
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq ""} {
        error "File not found in catalog: $filename"
    }
    
    # Use df module's export function
    set outpath [df::export $filepath $level $output_dir]
    
    # Mark as processed if extracted
    if {$level eq "extracted"} {
        dfdb eval {UPDATE datafiles SET processed = 1 WHERE filename = $filename}
    }
    
    return $outpath
}

# ============================================================================
# Download Function - Zip Bundle for Browser Download
# ============================================================================

#
# Create a zip bundle of multiple exported files (for browser download)
#
proc export_bundle {filenames level bundle_name} {
    # Validate level
    if {$level ni {raw trials extracted}} {
        return [_json_error "Invalid level: $level (must be raw, trials, or extracted)"]
    }
    
    set tmp_dir [get_tmp_path]
    set bundle_dir [file join $tmp_dir "bundle_${bundle_name}_[clock seconds]"]
    file mkdir $bundle_dir
    
    # Export each file to bundle directory
    set exported [list]
    set errors [list]
    
    foreach fn $filenames {
        set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $fn}]
        if {$filepath eq ""} {
            lappend errors [dict create filename $fn error "Not found in catalog"]
            continue
        }
        
        if {[catch {set path [df::export $filepath $level $bundle_dir]} err]} {
            lappend errors [dict create filename $fn error $err]
        } else {
            lappend exported $path
            # Mark as processed if extracted
            if {$level eq "extracted"} {
                dfdb eval {UPDATE datafiles SET processed = 1 WHERE filename = $fn}
            }
        }
    }
    
    if {[llength $exported] == 0} {
        file delete -force $bundle_dir
        return [_json_error "No files exported successfully"]
    }
    
    # Create zip file
    set zipname "${bundle_name}.zip"
    set zippath [file join $tmp_dir $zipname]
    
    catch {file delete $zippath}
    
    set cwd [pwd]
    cd $bundle_dir
    
    set files_to_zip [lmap p $exported {file tail $p}]
    if {[catch {exec zip -q $zippath {*}$files_to_zip} err]} {
        cd $cwd
        file delete -force $bundle_dir
        return [_json_error "Failed to create zip: $err"]
    }
    
    cd $cwd
    file delete -force $bundle_dir
    
    # Return JSON result
    set json [yajl create #auto]
    $json map_open
    $json string "status" string "ok"
    $json string "bundle" string $zipname
    $json string "path" string $zippath
    $json string "url" string "/download/$zipname"
    $json string "size" number [file size $zippath]
    $json string "file_count" number [llength $exported]
    
    if {[llength $errors] > 0} {
        $json string "errors" array_open
        foreach e $errors {
            $json map_open
            $json string "filename" string [dict get $e filename]
            $json string "error" string [dict get $e error]
            $json map_close
        }
        $json array_close
    }
    
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# ============================================================================
# Sync Configuration and Functions
# ============================================================================

variable sync_destination ""
variable auto_sync_level "extracted"
variable auto_sync_enabled 0

#
# Configure sync destination (called at startup or via datapoint)
#
proc configure_auto_sync {destination {level "extracted"} {enabled 1}} {
    variable sync_destination
    variable auto_sync_level
    variable auto_sync_enabled
    
    set sync_destination $destination
    set auto_sync_level $level
    set auto_sync_enabled $enabled
    
    if {$enabled && $destination ne ""} {
        file mkdir [file join $destination essdat]
        file mkdir [file join $destination dgzdat]
        puts "dfconf: Sync enabled to $destination (level: $level)"
    } else {
        puts "dfconf: Sync disabled"
    }
    
    dservSet df/sync_config [dict create \
        destination $destination \
        level $level \
        enabled $enabled]
}

#
# Get sync configuration (JSON for frontend)
#
proc get_sync_config {} {
    variable sync_destination
    variable auto_sync_level
    variable auto_sync_enabled
    
    set json [yajl create #auto]
    $json map_open
    $json string "destination" string $sync_destination
    $json string "level" string $auto_sync_level
    $json string "enabled" bool $auto_sync_enabled
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

#
# Sync files to configured destination (called from frontend)
#
# Arguments:
#   filenames - List of catalog filenames
#   level     - raw | trials | extracted
#   mode      - both | ess | dgz
#
# Returns: JSON result
#
proc sync_files {filenames level mode} {
    variable sync_destination
    variable auto_sync_enabled
    
    if {!$auto_sync_enabled || $sync_destination eq ""} {
        return [_json_error "Sync destination not configured"]
    }
    
    # Validate level
    if {$level ni {raw trials extracted}} {
        return [_json_error "Invalid level: $level"]
    }
    
    # Validate mode
    if {$mode ni {both ess dgz}} {
        set mode "both"
    }
    
    set essdir [file join $sync_destination essdat]
    set dgzdir [file join $sync_destination dgzdat]
    
    # Ensure directories exist
    file mkdir $essdir
    file mkdir $dgzdir
    
    set success 0
    set errors 0
    set ess_copied 0
    set dgz_copied 0
    set error_list [list]
    
    foreach fn $filenames {
        # Get filepath from catalog
        set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $fn}]
        if {$filepath eq ""} {
            lappend error_list [dict create filename $fn error "Not found in catalog"]
            incr errors
            continue
        }
        
        if {![file exists $filepath]} {
            lappend error_list [dict create filename $fn error "Source file missing"]
            incr errors
            continue
        }
        
        set file_success 1
        
        # Copy .ess if requested
        if {$mode eq "both" || $mode eq "ess"} {
            set ess_dest [file join $essdir [file tail $filepath]]
            if {[catch {file copy -force $filepath $ess_dest} err]} {
                lappend error_list [dict create filename $fn error "ESS copy failed: $err"]
                set file_success 0
            } else {
                incr ess_copied
            }
        }
        
        # Convert and save .dgz if requested
        if {$mode eq "both" || $mode eq "dgz"} {
            if {[catch {df::export $filepath $level $dgzdir} err]} {
                lappend error_list [dict create filename $fn error "DGZ export failed: $err"]
                set file_success 0
            } else {
                incr dgz_copied
                if {$level eq "extracted"} {
                    dfdb eval {UPDATE datafiles SET processed = 1 WHERE filename = $fn}
                }
            }
        }
        
        # Mark as synced if successful
        if {$file_success} {
            incr success
            dfdb eval {UPDATE datafiles SET synced = 1 WHERE filename = $fn}
        } else {
            incr errors
        }
    }
    
    # Build JSON response
    set json [yajl create #auto]
    $json map_open
    $json string "status" string "ok"
    $json string "total" number [llength $filenames]
    $json string "success" number $success
    $json string "errors" number $errors
    $json string "ess_copied" number $ess_copied
    $json string "dgz_copied" number $dgz_copied
    $json string "destination" string $sync_destination
    
    if {[llength $error_list] > 0} {
        $json string "error_details" array_open
        foreach e $error_list {
            $json map_open
            $json string "filename" string [dict get $e filename]
            $json string "error" string [dict get $e error]
            $json map_close
        }
        $json array_close
    }
    
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

#
# Called from on_datafile_closed to auto-sync newly closed files
#
proc auto_sync_file {filename} {
    variable sync_destination
    variable auto_sync_level
    variable auto_sync_enabled
    
    if {!$auto_sync_enabled || $sync_destination eq ""} {
        return 0
    }
    
    # Get filepath
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq "" || ![file exists $filepath]} {
        puts "dfconf: Auto-sync skipped, file not found: $filename"
        return 0
    }
    
    set essdir [file join $sync_destination essdat]
    set dgzdir [file join $sync_destination dgzdat]
    
    # Copy .ess
    if {[catch {file copy -force $filepath [file join $essdir [file tail $filepath]]} err]} {
        puts "dfconf: Auto-sync ESS copy failed for $filename: $err"
        return 0
    }
    
    # Export .dgz
    if {[catch {df::export $filepath $auto_sync_level $dgzdir} err]} {
        puts "dfconf: Auto-sync DGZ export failed for $filename: $err"
        return 0
    }
    
    dfdb eval {UPDATE datafiles SET synced = 1 WHERE filename = $filename}
    
    puts "dfconf: Auto-synced $filename to $sync_destination"
    return 1
}

# ============================================================================
# Preview for Data Manager
# ============================================================================

#
# Load and convert a datafile to hybrid JSON for preview
# 
# Arguments:
#   filename - Catalog filename (e.g., "file_001.ess")
#   level    - raw | trials | extracted (default: extracted)
#   limit    - Max rows to return (default: 100, for performance)
#
# Returns: Hybrid JSON string for DGTableViewer
#
proc preview_datafile {filename {level extracted} {limit 100}} {
    # Resolve filename to filepath via catalog
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    if {$filepath eq ""} {
        error "File not found in catalog: $filename"
    }
    
    if {![file exists $filepath]} {
        error "Source file missing: $filepath"
    }
    
    # Load data based on level
    switch $level {
        raw {
            set g [dslog::read $filepath]
        }
        trials {
            set g [dslog::readESS $filepath]
        }
        extracted {
            set g [df::load_data $filepath]
        }
        default {
            error "Unknown level: $level"
        }
    }
    
    # Limit rows if needed (for performance in preview)
    set n_rows [dl_length $g:[lindex [dg_tclListnames $g] 0]]
    if {$n_rows > $limit} {
        # Create a subset - first $limit rows
        set g_subset [dg_create]
        foreach col [dg_tclListnames $g] {
            dl_set $g_subset:$col [dl_choose $g:$col [dl_fromto 0 $limit]]
        }
        dg_delete $g
        set g $g_subset
    }
    
    # Convert to hybrid JSON
    set json [dg_toHybridJSON $g]
    
    # Cleanup
    dg_delete $g
    
    return $json
}

# ============================================================================
# Helper
# ============================================================================

proc _json_error {message} {
    set json [yajl create #auto]
    $json map_open
    $json string "status" string "error"
    $json string "error" string $message
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# ============================================================================
# Initialize
# ============================================================================

setup_database $df_db_path

# Subscribe to system path and project - will set ess_root for df module
dservAddExactMatch ess/system_path
dpointSetScript    ess/system_path process_system_path

dservAddExactMatch ess/project
dpointSetScript    ess/project process_project

# Subscribe to data directory - will trigger scan when received
dservAddExactMatch ess/data_dir
dpointSetScript    ess/data_dir process_data_dir

# Subscribe to datafile changes for close detection (use full path version)
dservAddExactMatch ess/datafile_path
dpointSetScript    ess/datafile_path process_ess_datafile

# Subscribe to sync destination configuration
dservAddExactMatch df/sync_destination
dpointSetScript    df/sync_destination process_sync_destination

# Touch to get current values (fails silently if not yet set)
catch {dservTouch ess/system_path}
catch {dservTouch ess/project}
catch {dservTouch ess/data_dir}
catch {dservTouch df/sync_destination}

# Clean up old temp files on startup
catch {cleanup_tmp 1}

puts "dfconf: Datafile manager ready (df 2.0)"
