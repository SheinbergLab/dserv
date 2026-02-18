#
# dfconf.tcl - Datafile management subprocess
#
# Uses df 2.0 module for core functionality, adds:
#   - SQLite catalog/index with column tracking
#   - Datapoint subscriptions for auto-indexing
#   - Two-stage conversion: .ess -> .obs.dgz -> .trials.dgz
#   - Re-extraction support when extractors are updated
#   - Query API for frontend
#
# Terminology:
#   - obs: observation-period oriented data (sync-line bounded epochs)
#   - trials: analysis-ready rectangular data (extracted trials)
#   - export: copy files to destination (not "sync")
#

package require dlsh
package require qpcs
package require sqlite3
package require yajltcl
package require dslog
package require em

tcl::tm::add $dspath/lib
package require df 2.0

# Configuration
set df_db_path "$dspath/db/datafiles.db"
set df_work_dir "$dspath/work"

# Paths will be set when we get datapoints
set ess_dir ""
set ess_system_path ""
set ess_project ""

# Track current datafile for close detection
set current_datafile ""

# Export configuration
variable export_destination ""
variable auto_export_enabled 0

# Disable exit
proc exit {args} { error "exit not available for this subprocess" }

# ============================================================================
# Database Setup
# ============================================================================

proc setup_database {db_path} {
    set dir_path [file dirname $db_path]
    if {![file exists $dir_path]} {
        file mkdir $dir_path
    }
    
    # Schema version - increment this when schema changes
    set SCHEMA_VERSION 2
    
    set needs_recreate 0
    
    if {[file exists $db_path]} {
        set open_status [catch {
            sqlite3 dfdb $db_path
            set result [dfdb eval {PRAGMA integrity_check;}]
        } errMsg]
        
        if {$open_status || $result ne "ok"} {
            puts "dfconf: Database corrupted, will recreate."
            set needs_recreate 1
        } else {
            # Check if schema is current by looking for required columns
            set has_status [dfdb eval {
                SELECT COUNT(*) FROM pragma_table_info('datafiles') WHERE name='status'
            }]
            set has_obs_path [dfdb eval {
                SELECT COUNT(*) FROM pragma_table_info('datafiles') WHERE name='obs_path'
            }]
            
            if {$has_status == 0 || $has_obs_path == 0} {
                puts "dfconf: Database schema outdated, will recreate."
                set needs_recreate 1
            }
        }
        
        if {$needs_recreate} {
            dfdb close
            set backup_path "$db_path.backup.[clock seconds]"
            puts "dfconf: Backing up old database to $backup_path"
            file rename -force $db_path $backup_path
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
            n_trials INTEGER,
            date TEXT,
            time TEXT,
            timestamp INTEGER,
            file_size INTEGER,
            created_at TEXT DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
            status TEXT DEFAULT 'pending',
            obs_path TEXT,
            trials_path TEXT,
            exported INTEGER DEFAULT 0
        );
    }
    
    # Indices for common queries
    dfdb eval {
        CREATE INDEX IF NOT EXISTS idx_datafiles_subject ON datafiles (subject);
        CREATE INDEX IF NOT EXISTS idx_datafiles_system ON datafiles (system);
        CREATE INDEX IF NOT EXISTS idx_datafiles_date ON datafiles (date);
        CREATE INDEX IF NOT EXISTS idx_datafiles_timestamp ON datafiles (timestamp);
        CREATE INDEX IF NOT EXISTS idx_datafiles_status ON datafiles (status);
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
    
    # Conversion tracking - tracks obs and trials conversion state
    dfdb eval {
        CREATE TABLE IF NOT EXISTS conversions (
            id INTEGER PRIMARY KEY,
            datafile_id INTEGER NOT NULL,
            stage TEXT NOT NULL,
            output_path TEXT,
            output_size INTEGER,
            output_mtime INTEGER,
            source_mtime INTEGER,
            extractor_path TEXT,
            extractor_mtime INTEGER,
            status TEXT DEFAULT 'pending',
            error_message TEXT,
            converted_at TEXT,
            FOREIGN KEY (datafile_id) REFERENCES datafiles(id) ON DELETE CASCADE,
            UNIQUE(datafile_id, stage)
        );
        
        CREATE INDEX IF NOT EXISTS idx_conversions_stage ON conversions(stage);
        CREATE INDEX IF NOT EXISTS idx_conversions_status ON conversions(status);
    }
    
    # Processing log - track processing events
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
    
    # Export history - track what was exported where
    dfdb eval {
        CREATE TABLE IF NOT EXISTS exports (
            id INTEGER PRIMARY KEY,
            datafile_id INTEGER NOT NULL,
            destination TEXT NOT NULL,
            exported_ess INTEGER DEFAULT 0,
            exported_obs INTEGER DEFAULT 0,
            exported_trials INTEGER DEFAULT 0,
            exported_at TEXT DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
            trials_mtime_at_export INTEGER,
            FOREIGN KEY (datafile_id) REFERENCES datafiles(id) ON DELETE CASCADE
        );
        
        CREATE INDEX IF NOT EXISTS idx_exports_destination ON exports(destination);
        CREATE INDEX IF NOT EXISTS idx_exports_datafile ON exports(datafile_id);
    }
    
    dfdb eval {
        PRAGMA synchronous = NORMAL;
        PRAGMA journal_mode = WAL;
        PRAGMA temp_store = MEMORY;
    }
}

# ============================================================================
# Work Directory Setup
# ============================================================================

proc setup_work_dir {work_dir} {
    if {![file exists $work_dir]} {
        file mkdir $work_dir
    }
    file mkdir [file join $work_dir obs]
    file mkdir [file join $work_dir trials]
    
    df::set_work_dir $work_dir
    puts "dfconf: Work directory set to: $work_dir"
}

# ============================================================================
# Configuration Datapoint Callbacks
# ============================================================================

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
    
    update_ess_root
}

proc process_project {dpoint data} {
    global ess_system_path ess_project
    
    if {$data eq ""} {
        return
    }
    
    set ess_project $data
    puts "dfconf: ESS project set to: $ess_project"
    
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

proc process_export_destination {dpoint data} {
    if {$data ne ""} {
        puts "dfconf: Export destination set to: $data"
        configure_auto_export $data 1
    } else {
        puts "dfconf: Export destination cleared"
        configure_auto_export "" 0
    }
}

# ============================================================================
# Datafile Indexing
# ============================================================================

proc scan_datafiles {{dir ""}} {
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
                               n_obs, date, time, timestamp, file_size, status)
        VALUES ($filename, $filepath, 
                $meta_subject, $meta_system, $meta_protocol, $meta_variant,
                $meta_n_obs, $meta_date, $meta_time, $meta_timestamp, $meta_file_size,
                'indexed')
    }
    
    set file_id [dfdb last_insert_rowid]
    
    # Insert column information
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
    
    puts "dfconf: Indexed: $filename ($meta_system/$meta_protocol, $meta_n_obs obs)"
    return 1
}

proc reindex_datafile {filename} {
    set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $filename}]
    
    if {$filepath eq ""} {
        return 0
    }
    
    set meta [df::metadata $filepath]
    
    dfdb eval {
        UPDATE datafiles SET
            subject = :subj,
            system = :sys,
            protocol = :proto,
            variant = :var,
            n_obs = :nobs,
            date = :dt,
            time = :tm,
            timestamp = :ts,
            file_size = :fsize
        WHERE filename = :filename
    } subj [dict get $meta subject] \
      sys [dict get $meta system] \
      proto [dict get $meta protocol] \
      var [dict get $meta variant] \
      nobs [dict get $meta n_obs] \
      dt [dict get $meta date] \
      tm [dict get $meta time] \
      ts [dict get $meta timestamp] \
      fsize [dict get $meta file_size]
    
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

# ============================================================================
# Datafile Close Detection and Two-Stage Processing
# ============================================================================

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
    global ess_system_path ess_project df_work_dir
    
    puts "dfconf: Datafile closed: $filepath"
    
    set filename [file tail $filepath]
    set basename [file rootname $filename]
    
    # Index the file first
    index_datafile $filepath
    
    # Get file_id and metadata
    set file_id [dfdb eval {SELECT id FROM datafiles WHERE filename = $filename}]
    set meta [df::metadata $filepath]
    
    set system [dict get $meta system]
    set protocol [dict get $meta protocol]
    
    if {$system eq ""} {
        puts "dfconf: Warning: Could not determine system for $filepath"
        dfdb eval {UPDATE datafiles SET status = 'error' WHERE id = $file_id}
        return
    }
    
    log_processing $file_id "close_detected" "success" "system=$system protocol=$protocol"
    
    # Update status to processing
    dfdb eval {UPDATE datafiles SET status = 'processing' WHERE id = $file_id}
    
    # Stage 1: Create .obs.dgz (ALWAYS - this is the foundation)
    set obs_result [create_obs_file $filepath $file_id $basename]
    
    if {[dict get $obs_result status] ne "ok"} {
        puts "dfconf: ERROR creating obs file: [dict get $obs_result error]"
        dfdb eval {UPDATE datafiles SET status = 'error' WHERE id = $file_id}
        publish_file_closed $filepath $meta "obs_error"
        return
    }
    
    set obs_path [dict get $obs_result path]
    set n_obs [dict get $obs_result n_obs]
    
    # Stage 2: Create .trials.dgz (from obs, not from ess)
    set trials_result [create_trials_file $obs_path $file_id $basename $system $protocol]
    
    if {[dict get $trials_result status] eq "ok"} {
        set trials_path [dict get $trials_result path]
        set n_trials [dict get $trials_result n_trials]
        
        dfdb eval {
            UPDATE datafiles 
            SET status = 'ok', 
                n_trials = :n_trials,
                obs_path = :obs_path, 
                trials_path = :trials_path
            WHERE id = :file_id
        }
        
        # Run optional analyzer
        run_analyzer $system $trials_path $filepath $file_id
        
        publish_file_closed $filepath $meta "ok"
        
    } else {
        puts "dfconf: WARNING: Trials extraction failed: [dict get $trials_result error]"
        # obs exists, trials failed - file is partially processed
        dfdb eval {
            UPDATE datafiles 
            SET status = 'obs_only', 
                obs_path = :obs_path
            WHERE id = :file_id
        }
        publish_file_closed $filepath $meta "trials_error"
    }
    
    # Auto-export if enabled
    auto_export_file $filename
    
    # Clean up trials.dgz from work_dir after export.
    # The obs.dgz is the stable cache; trials are always re-extracted
    # from current extract code to avoid stale cached results.
    if {[info exists trials_path] && [file exists $trials_path]} {
        file delete $trials_path
    }
}

proc create_obs_file {ess_path file_id basename} {
    global df_work_dir
    
    set obs_dir [file join $df_work_dir obs]
    file mkdir $obs_dir
    set obs_path [file join $obs_dir $basename.obs.dgz]
    
    set ess_mtime [file mtime $ess_path]
    
    if {[catch {
        set g [dslog::readESS $ess_path]
        set n_obs [dl_length $g:e_types]
        dg_write $g $obs_path
        dg_delete $g
    } err]} {
        log_processing $file_id "obs" "error" $err
        record_conversion $file_id "obs" "" "error" $err $ess_mtime "" ""
        return [dict create status error error $err]
    }
    
    set obs_size [file size $obs_path]
    log_processing $file_id "obs" "success" "n_obs=$n_obs size=$obs_size"
    record_conversion $file_id "obs" $obs_path "ok" "" $ess_mtime "" ""
    
    return [dict create status ok path $obs_path n_obs $n_obs size $obs_size]
}

proc create_trials_file {obs_path file_id basename system protocol} {
    global df_work_dir
    
    set trials_dir [file join $df_work_dir trials]
    file mkdir $trials_dir
    set trials_path [file join $trials_dir $basename.trials.dgz]
    
    set obs_mtime [file mtime $obs_path]
    
    # Find extractor
    set ess_root [df::get_ess_root]
    if {$ess_root eq ""} {
        set err "ESS root not set"
        log_processing $file_id "trials" "error" $err
        record_conversion $file_id "trials" "" "error" $err $obs_mtime "" ""
        return [dict create status error error $err]
    }
    
    set extractor [file join $ess_root $system ${system}_extract.tcl]
    
    if {![file exists $extractor]} {
        set err "No extractor found: $extractor"
        log_processing $file_id "trials" "error" $err
        record_conversion $file_id "trials" "" "error" $err $obs_mtime "" ""
        return [dict create status error error $err]
    }
    
    set extractor_mtime [file mtime $extractor]
    
    if {[catch {
        # Source extractor
        uplevel #0 [list source $extractor]
        
        # Load obs data
        set g [dg_read $obs_path]
        
        # Get metadata from obs
        set meta [df::extract_metadata_from_obs $g]
        
        # Try obs-aware extractor first, fall back to File-based
        set trials_proc "::${system}::extract_trials_from_obs"
        if {[info commands $trials_proc] ne ""} {
            set trials [{*}$trials_proc $g $meta]
        } else {
            # Fall back: get ess path and use original load_data
            set ess_path [dfdb eval {SELECT filepath FROM datafiles WHERE id = $file_id}]
            dg_delete $g
            set trials [df::load_data $ess_path]
        }
        
        # Count trials
        set cols [dg_tclListnames $trials]
        set n_trials 0
        if {[llength $cols] > 0} {
            set n_trials [dl_length $trials:[lindex $cols 0]]
        }
        
        # Write output
        dg_write $trials $trials_path
        
        catch {dg_delete $g}
        dg_delete $trials
    } err]} {
        log_processing $file_id "trials" "error" $err
        record_conversion $file_id "trials" "" "error" $err $obs_mtime $extractor $extractor_mtime
        return [dict create status error error $err]
    }
    
    set trials_size [file size $trials_path]
    log_processing $file_id "trials" "success" "n_trials=$n_trials size=$trials_size"
    record_conversion $file_id "trials" $trials_path "ok" "" $obs_mtime $extractor $extractor_mtime
    
    return [dict create status ok path $trials_path n_trials $n_trials \
            size $trials_size extractor $extractor extractor_mtime $extractor_mtime]
}

proc record_conversion {file_id stage output_path status error source_mtime extractor extractor_mtime} {
    set now [clock format [clock seconds] -format "%Y-%m-%d %H:%M:%S"]
    set output_size ""
    set output_mt ""
    
    if {$output_path ne "" && [file exists $output_path]} {
        set output_size [file size $output_path]
        set output_mt [file mtime $output_path]
    }
    
    dfdb eval {
        INSERT OR REPLACE INTO conversions 
            (datafile_id, stage, output_path, output_size, output_mtime,
             source_mtime, extractor_path, extractor_mtime, status, error_message, converted_at)
        VALUES (:file_id, :stage, :output_path, :output_size, :output_mt,
                :source_mtime, :extractor, :extractor_mtime, :status, :error, :now)
    }
}

proc run_analyzer {system trials_path ess_path file_id} {
    set ess_root [df::get_ess_root]
    if {$ess_root eq ""} return
    
    # Source analyzer if it exists
    set analyzer_file [file join $ess_root $system ${system}_analyze.tcl]
    if {[file exists $analyzer_file]} {
        if {[catch {uplevel #0 [list source $analyzer_file]} err]} {
            puts "dfconf: Error sourcing analyzer $analyzer_file: $err"
            return
        }
    }
    
    # Run system analyzer if exists
    set analyzer_proc "::${system}::analyze"
    if {[info commands $analyzer_proc] ne ""} {
        puts "dfconf: Running analyzer $analyzer_proc"
        
        set trials [dg_read $trials_path]
        
        if {[catch {set results [{*}$analyzer_proc $trials $ess_path]} err]} {
            puts "dfconf: Analyze failed: $err"
            log_processing $file_id "analyze" "error" $err
        } else {
            puts "dfconf: Analysis complete"
            log_processing $file_id "analyze" "success" "analysis complete"
        }
        
        dg_delete $trials
    }
}

proc publish_file_closed {filepath meta status} {
    set filename [file tail $filepath]
    dservSet df/file_closed [dict create \
        filepath $filepath \
        filename $filename \
        system [dict get $meta system] \
        protocol [dict get $meta protocol] \
        n_obs [dict get $meta n_obs] \
        status $status \
        timestamp [clock seconds]]
}

proc log_processing {file_id processor status message} {
    dfdb eval {
        INSERT INTO processing_log (datafile_id, processor, status, message)
        VALUES ($file_id, $processor, $status, $message)
    }
}

# ============================================================================
# Re-extraction Support
# ============================================================================

proc reextract_trials {filename} {
    set row [dfdb eval {
        SELECT id, obs_path, system, protocol FROM datafiles WHERE filename = :filename
    }]
    
    if {[llength $row] == 0} {
        return [dict create status error error "File not found: $filename"]
    }
    
    lassign $row file_id obs_path system protocol
    
    if {$obs_path eq "" || ![file exists $obs_path]} {
        return [dict create status error error "Obs file not found - need to reprocess from .ess"]
    }
    
    set basename [file rootname [file rootname [file tail $obs_path]]]
    
    set result [create_trials_file $obs_path $file_id $basename $system $protocol]
    
    if {[dict get $result status] eq "ok"} {
        set trials_path [dict get $result path]
        set n_trials [dict get $result n_trials]
        
        dfdb eval {
            UPDATE datafiles 
            SET status = 'ok', 
                n_trials = :n_trials,
                trials_path = :trials_path
            WHERE id = :file_id
        }
    }
    
    return $result
}

proc reextract_system {system} {
    set ess_root [df::get_ess_root]
    if {$ess_root eq ""} {
        return [_json_error "ESS root not set"]
    }
    
    set extractor [file join $ess_root $system ${system}_extract.tcl]
    if {![file exists $extractor]} {
        return [_json_error "Extractor not found: $extractor"]
    }
    
    set current_mtime [file mtime $extractor]
    
    # Find files needing re-extraction
    set files [dfdb eval {
        SELECT d.filename 
        FROM datafiles d
        LEFT JOIN conversions c ON d.id = c.datafile_id AND c.stage = 'trials'
        WHERE d.system = :system 
          AND d.obs_path IS NOT NULL
          AND (c.extractor_mtime IS NULL OR c.extractor_mtime < :current_mtime)
    }]
    
    set success 0
    set failed 0
    set errors [list]
    
    foreach fn $files {
        set result [reextract_trials $fn]
        if {[dict get $result status] eq "ok"} {
            incr success
        } else {
            incr failed
            lappend errors [dict create filename $fn error [dict get $result error]]
        }
    }
    
    return [_json_ok [dict create \
        system $system \
        total [llength $files] \
        success $success \
        failed $failed]]
}

proc files_needing_reextract {{system ""}} {
    set ess_root [df::get_ess_root]
    if {$ess_root eq ""} {
        return "{}"
    }
    
    if {$system eq ""} {
        set systems [dfdb eval {SELECT DISTINCT system FROM datafiles WHERE system != ''}]
    } else {
        set systems [list $system]
    }
    
    set json [yajl create #auto]
    $json map_open
    
    foreach sys $systems {
        set extractor [file join $ess_root $sys ${sys}_extract.tcl]
        if {![file exists $extractor]} continue
        
        set current_mtime [file mtime $extractor]
        
        set files [dfdb eval {
            SELECT d.filename 
            FROM datafiles d
            LEFT JOIN conversions c ON d.id = c.datafile_id AND c.stage = 'trials'
            WHERE d.system = :sys 
              AND d.obs_path IS NOT NULL
              AND (c.extractor_mtime IS NULL OR c.extractor_mtime < :current_mtime)
        }]
        
        if {[llength $files] > 0} {
            $json string $sys array_open
            foreach f $files {
                $json string $f
            }
            $json array_close
        }
    }
    
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# ============================================================================
# Query Functions for Frontend
# ============================================================================

# Process a datafile to create obs and trials files (on-demand)
# Called when exporting files that haven't been processed yet
proc process_datafile {filename} {
    global df_work_dir
    
    set row [dfdb eval {
        SELECT id, filepath, system, protocol, obs_path, trials_path 
        FROM datafiles WHERE filename = :filename
    }]
    
    if {[llength $row] == 0} {
        return [dict create status error error "File not found: $filename"]
    }
    
    lassign $row file_id filepath system protocol obs_path trials_path
    
    if {![file exists $filepath]} {
        return [dict create status error error "Source file missing: $filepath"]
    }
    
    set basename [file rootname [file tail $filepath]]
    
    # Create obs if needed
    if {$obs_path eq "" || ![file exists $obs_path]} {
        puts "dfconf: Processing $filename -> obs"
        set obs_result [create_obs_file $filepath $file_id $basename]
        
        if {[dict get $obs_result status] ne "ok"} {
            return [dict create status error error "Obs creation failed: [dict get $obs_result error]"]
        }
        
        set obs_path [dict get $obs_result path]
        set n_obs [dict get $obs_result n_obs]
        
        dfdb eval {
            UPDATE datafiles SET obs_path = :obs_path, n_obs = :n_obs
            WHERE id = :file_id
        }
    }
    
    # Create trials if needed (and we have system info)
    if {($trials_path eq "" || ![file exists $trials_path]) && $system ne ""} {
        puts "dfconf: Processing $filename -> trials"
        set trials_result [create_trials_file $obs_path $file_id $basename $system $protocol]
        
        if {[dict get $trials_result status] eq "ok"} {
            set trials_path [dict get $trials_result path]
            set n_trials [dict get $trials_result n_trials]
            
            dfdb eval {
                UPDATE datafiles 
                SET status = 'ok', 
                    n_trials = :n_trials,
                    trials_path = :trials_path
                WHERE id = :file_id
            }
        } else {
            # Obs succeeded but trials failed - partial success
            dfdb eval {
                UPDATE datafiles SET status = 'obs_only', obs_path = :obs_path
                WHERE id = :file_id
            }
            puts "dfconf: Trials extraction failed for $filename: [dict get $trials_result error]"
        }
    }
    
    return [dict create status ok obs_path $obs_path trials_path $trials_path]
}

proc list_datafiles {{filters {}}} {
    set where_clauses {}
    
    if {[dict exists $filters subject] && [dict get $filters subject] ne ""} {
        lappend where_clauses "subject = '[dict get $filters subject]'"
    }
    if {[dict exists $filters system] && [dict get $filters system] ne ""} {
        lappend where_clauses "system = '[dict get $filters system]'"
    }
    if {[dict exists $filters protocol] && [dict get $filters protocol] ne ""} {
        lappend where_clauses "protocol = '[dict get $filters protocol]'"
    }
    if {[dict exists $filters variant] && [dict get $filters variant] ne ""} {
        lappend where_clauses "variant = '[dict get $filters variant]'"
    }
    if {[dict exists $filters date] && [dict get $filters date] ne ""} {
        lappend where_clauses "date = '[dict get $filters date]'"
    }
    if {[dict exists $filters after] && [dict get $filters after] ne ""} {
        lappend where_clauses "timestamp >= [dict get $filters after]"
    }
    if {[dict exists $filters before] && [dict get $filters before] ne ""} {
        lappend where_clauses "timestamp <= [dict get $filters before]"
    }
    if {[dict exists $filters min_obs] && [dict get $filters min_obs] ne ""} {
        lappend where_clauses "n_obs >= [dict get $filters min_obs]"
    }
    if {[dict exists $filters status] && [dict get $filters status] ne ""} {
        lappend where_clauses "status = '[dict get $filters status]'"
    }
    
    set sql "SELECT id, filename, filepath, subject, system, protocol, variant, 
                    n_obs, n_trials, date, time, timestamp, file_size, status, exported
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
        $json string "n_trials" number [expr {$values(n_trials) ne "" ? $values(n_trials) : 0}]
        $json string "date" string $values(date)
        $json string "time" string $values(time)
        $json string "timestamp" number $values(timestamp)
        $json string "file_size" number $values(file_size)
        $json string "status" string $values(status)
        $json string "exported" number [expr {$values(exported) ne "" ? $values(exported) : 0}]
        $json map_close
    }
    
    $json array_close
    set result [$json get]
    $json delete
    return $result
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
    set total_trials [dfdb eval {SELECT COALESCE(SUM(n_trials), 0) FROM datafiles}]
    set ok_count [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE status = 'ok'}]
    set error_count [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE status IN ('error', 'obs_only')}]
    set exported_count [dfdb eval {SELECT COUNT(*) FROM datafiles WHERE exported = 1}]
    
    set json [yajl create #auto]
    $json map_open
    $json string "total_files" number $total_files
    $json string "total_obs" number $total_obs
    $json string "total_trials" number $total_trials
    $json string "ok" number $ok_count
    $json string "errors" number $error_count
    $json string "exported" number $exported_count
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

proc get_datafile_info {filename} {
    set row [dfdb eval {
        SELECT id, filename, filepath, subject, system, protocol, variant, 
               n_obs, n_trials, date, time, timestamp, file_size, status, 
               obs_path, trials_path, exported
        FROM datafiles WHERE filename = $filename
    }]
    
    if {[llength $row] == 0} {
        return [_json_error "File not found: $filename"]
    }
    
    lassign $row id fn fp subj sys proto var n_obs n_trials date time ts fsize status obs_path trials_path exported
    
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
    $json string "n_trials" number [expr {$n_trials ne "" ? $n_trials : 0}]
    $json string "date" string $date
    $json string "time" string $time
    $json string "timestamp" number $ts
    $json string "file_size" number $fsize
    $json string "status" string $status
    $json string "obs_path" string [expr {$obs_path ne "" ? $obs_path : ""}]
    $json string "trials_path" string [expr {$trials_path ne "" ? $trials_path : ""}]
    $json string "exported" number [expr {$exported ne "" ? $exported : 0}]
    
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
    
    # Add conversion info
    $json string "conversions" array_open
    dfdb eval {
        SELECT stage, output_path, status, error_message, converted_at
        FROM conversions WHERE datafile_id = $id
    } conv {
        $json map_open
        $json string "stage" string $conv(stage)
        $json string "path" string [expr {$conv(output_path) ne "" ? $conv(output_path) : ""}]
        $json string "status" string $conv(status)
        $json string "error" string [expr {$conv(error_message) ne "" ? $conv(error_message) : ""}]
        $json string "converted_at" string [expr {$conv(converted_at) ne "" ? $conv(converted_at) : ""}]
        $json map_close
    }
    $json array_close
    
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

proc rescan {} {
    remove_missing
    set count [scan_datafiles]
    return [_json_ok [dict create indexed $count]]
}

# ============================================================================
# Downloads Path for Zip Bundles
# ============================================================================

proc get_downloads_path {} {
    global exports_path dspath
    
    # Try system datapoint first (set by main dserv process)
    if {[catch {set sys_exports [dservGet system/exports_path]}] == 0 && $sys_exports ne ""} {
        set downloads_path $sys_exports
    # Fall back to local variable if set
    } elseif {[info exists exports_path] && $exports_path ne ""} {
        set downloads_path $exports_path
    # Final fallback
    } else {
        set downloads_path [file join $dspath downloads]
    }
    
    if {![file exists $downloads_path]} {
        file mkdir $downloads_path
    }
    return $downloads_path
}

proc cleanup_downloads {{hours 1}} {
    set downloads_path [get_downloads_path]
    set cutoff [expr {[clock seconds] - ($hours * 3600)}]
    set deleted 0
    
    foreach f [glob -nocomplain -directory $downloads_path *] {
        if {[file isfile $f] && [file mtime $f] < $cutoff} {
            catch {file delete $f}
            incr deleted
        }
    }
    return $deleted
}

# ============================================================================
# Download Bundle for Browser
# ============================================================================

proc export_bundle {filenames level bundle_name} {
    # Validate level
    if {$level ni {raw obs trials}} {
        return [_json_error "Invalid level: $level (must be raw, obs, or trials)"]
    }
    
    set downloads_dir [get_downloads_path]
    set bundle_dir [file join $downloads_dir "bundle_${bundle_name}_[clock seconds]"]
    file mkdir $bundle_dir
    
    set exported [list]
    set errors [list]
    
    foreach fn $filenames {
        # Try to use pre-converted file if available
        if {$level eq "trials"} {
            set trials_path [dfdb eval {SELECT trials_path FROM datafiles WHERE filename = $fn}]
            if {$trials_path ne "" && [file exists $trials_path]} {
                set dest [file join $bundle_dir [file tail $trials_path]]
                if {[catch {file copy $trials_path $dest} err]} {
                    lappend errors [dict create filename $fn error $err]
                } else {
                    lappend exported $dest
                }
                continue
            }
        }
        
        if {$level eq "obs"} {
            set obs_path [dfdb eval {SELECT obs_path FROM datafiles WHERE filename = $fn}]
            if {$obs_path ne "" && [file exists $obs_path]} {
                set dest [file join $bundle_dir [file tail $obs_path]]
                if {[catch {file copy $obs_path $dest} err]} {
                    lappend errors [dict create filename $fn error $err]
                } else {
                    lappend exported $dest
                }
                continue
            }
        }
        
        # Fall back to conversion from ess
        set filepath [dfdb eval {SELECT filepath FROM datafiles WHERE filename = $fn}]
        if {$filepath eq ""} {
            lappend errors [dict create filename $fn error "Not found in catalog"]
            continue
        }
        
        if {[catch {set result [df::convert $filepath $level $bundle_dir]} err]} {
            lappend errors [dict create filename $fn error $err]
        } else {
            if {[dict exists $result path]} {
                lappend exported [dict get $result path]
            } else {
                lappend exported $result
            }
        }
    }
    
    if {[llength $exported] == 0} {
        file delete -force $bundle_dir
        # Include error details in the message
        if {[llength $errors] > 0} {
            set first_error [dict get [lindex $errors 0] error]
            return [_json_error "No files exported: $first_error"]
        }
        return [_json_error "No files exported successfully"]
    }
    
    # Create zip
    set zipname "${bundle_name}.zip"
    set zippath [file join $downloads_dir $zipname]
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
# User/Ownership Utilities for Export Operations
# ============================================================================

# Check if path is on an NFS mount
proc is_nfs_path {path} {
    # Walk up to find existing path
    while {$path ne "/" && ![file exists $path]} {
        set path [file dirname $path]
    }
    if {[catch {exec df -T $path 2>/dev/null} result]} {
        # df -T not available (e.g., macOS), try mount command
        if {[catch {exec mount} mounts]} {
            return 0
        }
        # Check if any nfs mount is a prefix of our path
        foreach line [split $mounts "\n"] {
            if {[string match "*nfs*" [string tolower $line]]} {
                # Extract mount point - format varies by OS
                if {[regexp {on\s+(/[^\s]+)} $line -> mountpoint]} {
                    if {[string match "${mountpoint}*" $path]} {
                        return 1
                    }
                }
            }
        }
        return 0
    }
    return [string match "*nfs*" [string tolower $result]]
}

# Get the owner to use for a path (walks up to find existing ancestor)
proc get_owner_for_path {path} {
    # Walk up to find existing ancestor
    while {$path ne "/" && ![file exists $path]} {
        set path [file dirname $path]
    }
    if {[file exists $path]} {
        return [file attributes $path -owner]
    }
    return ""
}

# Check if we need to switch users for operations in a directory
proc need_setuid_for_path {path} {
    set owner [get_owner_for_path $path]
    if {$owner eq ""} {
        return 0
    }
    set current_user [exec whoami]
    return [expr {$owner ne $current_user}]
}

# Copy file with correct ownership (uses sudo if needed, skips for NFS)
proc copy_as_user {src dest owner} {
    set current_user [exec whoami]
    # Use regular copy for NFS or if we're already the right user
    if {$owner eq "" || $owner eq $current_user || [is_nfs_path $dest]} {
        file copy -force $src $dest
    } else {
        exec sudo -u $owner cp $src $dest
    }
}

# Create directory with correct ownership (uses sudo if needed, skips for NFS)
proc mkdir_as_user {path owner} {
    if {[file exists $path]} {
        return
    }
    set current_user [exec whoami]
    # Use regular mkdir for NFS or if we're already the right user
    if {$owner eq "" || $owner eq $current_user || [is_nfs_path $path]} {
        file mkdir $path
    } else {
        exec sudo -u $owner mkdir -p $path
    }
}

# ============================================================================
# Export Configuration and Functions
# ============================================================================

proc configure_auto_export {destination {enabled 1}} {
    variable export_destination
    variable auto_export_enabled
    
    set export_destination $destination
    set auto_export_enabled $enabled
    
    if {$enabled && $destination ne ""} {
        file mkdir [file join $destination ess]
        file mkdir [file join $destination obs]
        file mkdir [file join $destination trials]
        puts "dfconf: Export enabled to $destination"
    } else {
        puts "dfconf: Export disabled"
    }
    
    dservSet df/export_config [dict create \
        destination $destination \
        enabled $enabled]
}

proc get_export_config {} {
    variable export_destination
    variable auto_export_enabled
    
    set json [yajl create #auto]
    $json map_open
    $json string "destination" string $export_destination
    $json string "enabled" bool $auto_export_enabled
    $json map_close
    set result [$json get]
    $json delete
    return $result
}



proc export_files {filenames level mode} {
    variable export_destination
    variable auto_export_enabled
    
    if {!$auto_export_enabled || $export_destination eq ""} {
        return [_json_error "Export destination not configured"]
    }
    
    if {$level ni {raw obs trials}} {
        return [_json_error "Invalid level: $level (must be raw, obs, or trials)"]
    }
    
    if {$mode ni {both ess dgz all}} {
        set mode "both"
    }
    
    # Determine owner for destination directory
    set owner [get_owner_for_path $export_destination]
    
    set essdir [file join $export_destination ess]
    set obsdir [file join $export_destination obs]
    set trialsdir [file join $export_destination trials]
    
    # Create directories with correct ownership
    mkdir_as_user $essdir $owner
    mkdir_as_user $obsdir $owner
    mkdir_as_user $trialsdir $owner
    
    set success 0
    set errors_count 0
    set ess_copied 0
    set obs_copied 0
    set trials_copied 0
    set error_list [list]
    
    foreach fn $filenames {
        set row [dfdb eval {
            SELECT id, filepath, obs_path, trials_path, system FROM datafiles WHERE filename = $fn
        }]
        
        if {[llength $row] == 0} {
            lappend error_list [dict create filename $fn error "Not found in catalog"]
            incr errors_count
            continue
        }
        
        lassign $row file_id filepath obs_path trials_path system
        
        if {![file exists $filepath]} {
            lappend error_list [dict create filename $fn error "Source file missing"]
            incr errors_count
            continue
        }
        
        # Ensure obs and trials are fresh before export
        if {$mode in {both dgz all}} {
            # Create obs from ess if missing
            if {$obs_path eq "" || ![file exists $obs_path]} {
                puts "dfconf: Creating obs for $fn..."
                set basename [file rootname $fn]
                set obs_result [create_obs_file $filepath $file_id $basename]
                if {[dict get $obs_result status] eq "ok"} {
                    set obs_path [dict get $obs_result path]
                    dfdb eval {UPDATE datafiles SET obs_path = :obs_path WHERE id = :file_id}
                }
            }
            
            # Always re-extract trials from obs to use latest extract code
            if {$obs_path ne "" && [file exists $obs_path] && $system ne ""} {
                puts "dfconf: Extracting trials for $fn..."
                set basename [file rootname [file rootname [file tail $obs_path]]]
                set protocol [dfdb eval {SELECT protocol FROM datafiles WHERE id = $file_id}]
                set trials_result [create_trials_file $obs_path $file_id $basename $system $protocol]
                if {[dict get $trials_result status] eq "ok"} {
                    set trials_path [dict get $trials_result path]
                    set n_trials [dict get $trials_result n_trials]
                    dfdb eval {
                        UPDATE datafiles SET trials_path = :trials_path, n_trials = :n_trials 
                        WHERE id = :file_id
                    }
                }
            }
        }
        
        set file_success 1
        
        # Copy .ess if requested
        if {$mode in {both ess all}} {
            set ess_dest [file join $essdir [file tail $filepath]]
            if {[catch {copy_as_user $filepath $ess_dest $owner} err]} {
                lappend error_list [dict create filename $fn error "ESS copy failed: $err"]
                set file_success 0
            } else {
                incr ess_copied
            }
        }
        
        # Copy .obs.dgz if requested
        if {$mode in {both dgz all}} {
            if {$obs_path ne "" && [file exists $obs_path]} {
                set obs_dest [file join $obsdir [file tail $obs_path]]
                if {[catch {copy_as_user $obs_path $obs_dest $owner} err]} {
                    lappend error_list [dict create filename $fn error "OBS copy failed: $err"]
                    set file_success 0
                } else {
                    incr obs_copied
                }
            }
        }
        
        # Copy .trials.dgz if requested
        if {$mode in {both dgz all}} {
            if {$trials_path ne "" && [file exists $trials_path]} {
                set trials_dest [file join $trialsdir [file tail $trials_path]]
                if {[catch {copy_as_user $trials_path $trials_dest $owner} err]} {
                    lappend error_list [dict create filename $fn error "Trials copy failed: $err"]
                    set file_success 0
                } else {
                    incr trials_copied
                }
            }
        }
        
        if {$file_success} {
            incr success
            dfdb eval {UPDATE datafiles SET exported = 1 WHERE id = $file_id}
            
            set trials_mtime ""
            if {$trials_path ne "" && [file exists $trials_path]} {
                set trials_mtime [file mtime $trials_path]
            }
            
            dfdb eval {
                INSERT INTO exports (datafile_id, destination, 
                                    exported_ess, exported_obs, exported_trials,
                                    trials_mtime_at_export)
                VALUES (:file_id, :export_destination,
                        :ess_copied, :obs_copied, :trials_copied,
                        :trials_mtime)
            }
            
            # Clean up trials.dgz from work_dir after successful export.
            # Obs is the stable cache; trials are re-extracted on demand.
            if {$trials_path ne "" && [file exists $trials_path]} {
                file delete $trials_path
            }
        } else {
            incr errors_count
        }
    }
    
    set json [yajl create #auto]
    $json map_open
    $json string "status" string "ok"
    $json string "total" number [llength $filenames]
    $json string "success" number $success
    $json string "errors" number $errors_count
    $json string "ess_copied" number $ess_copied
    $json string "obs_copied" number $obs_copied
    $json string "trials_copied" number $trials_copied
    $json string "destination" string $export_destination
    
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

proc auto_export_file {filename} {
    variable export_destination
    variable auto_export_enabled
    
    if {!$auto_export_enabled || $export_destination eq ""} {
        return 0
    }
    
    set row [dfdb eval {
        SELECT id, filepath, obs_path, trials_path FROM datafiles WHERE filename = $filename
    }]
    
    if {[llength $row] == 0} {
        puts "dfconf: Auto-export skipped, file not found: $filename"
        return 0
    }
    
    lassign $row file_id filepath obs_path trials_path
    
    if {![file exists $filepath]} {
        puts "dfconf: Auto-export skipped, source missing: $filename"
        return 0
    }
    
    # Determine owner for destination directory
    set owner [get_owner_for_path $export_destination]
    
    set essdir [file join $export_destination ess]
    set obsdir [file join $export_destination obs]
    set trialsdir [file join $export_destination trials]
    
    # Create directories with correct ownership
    mkdir_as_user $essdir $owner
    mkdir_as_user $obsdir $owner
    mkdir_as_user $trialsdir $owner
    
    set exported_any 0
    
    # Copy .ess
    if {[catch {copy_as_user $filepath [file join $essdir [file tail $filepath]] $owner} err]} {
        puts "dfconf: Auto-export ESS copy failed for $filename: $err"
    } else {
        set exported_any 1
    }
    
    # Copy .obs.dgz
    if {$obs_path ne "" && [file exists $obs_path]} {
        if {[catch {copy_as_user $obs_path [file join $obsdir [file tail $obs_path]] $owner} err]} {
            puts "dfconf: Auto-export OBS copy failed for $filename: $err"
        }
    }
    
    # Copy .trials.dgz
    if {$trials_path ne "" && [file exists $trials_path]} {
        if {[catch {copy_as_user $trials_path [file join $trialsdir [file tail $trials_path]] $owner} err]} {
            puts "dfconf: Auto-export trials copy failed for $filename: $err"
        }
    }
    
    if {$exported_any} {
        dfdb eval {UPDATE datafiles SET exported = 1 WHERE filename = $filename}
        puts "dfconf: Auto-exported $filename to $export_destination"
    }
    
    return $exported_any
}

# ============================================================================
# Preview for Data Manager
# ============================================================================

proc preview_datafile {filename {level trials} {limit 100}} {
    set row [dfdb eval {
        SELECT filepath, obs_path, trials_path FROM datafiles WHERE filename = $filename
    }]
    
    if {[llength $row] == 0} {
        error "File not found in catalog: $filename"
    }
    
    lassign $row filepath obs_path trials_path
    
    # Load data based on level, preferring pre-converted files
    switch $level {
        raw {
            if {![file exists $filepath]} {
                error "Source file missing: $filepath"
            }
            set g [dslog::read $filepath]
        }
        obs {
            if {$obs_path ne "" && [file exists $obs_path]} {
                set g [dg_read $obs_path]
            } elseif {[file exists $filepath]} {
                set g [dslog::readESS $filepath]
            } else {
                error "No obs data available for: $filename"
            }
        }
        trials {
            if {$trials_path ne "" && [file exists $trials_path]} {
                set g [dg_read $trials_path]
            } elseif {[file exists $filepath]} {
                set g [df::load_data $filepath]
            } else {
                error "No trials data available for: $filename"
            }
        }
        default {
            error "Unknown level: $level"
        }
    }
    
    # Limit rows for preview performance
    set cols [dg_tclListnames $g]
    if {[llength $cols] > 0} {
        set n_rows [dl_length $g:[lindex $cols 0]]
        if {$n_rows > $limit} {
            set g_subset [dg_create]
            foreach col $cols {
                dl_set $g_subset:$col [dl_choose $g:$col [dl_fromto 0 $limit]]
            }
            dg_delete $g
            set g $g_subset
        }
    }
    
    set json [dg_toHybridJSON $g]
    dg_delete $g
    return $json
}

# ============================================================================
# JSON Helpers
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

proc _json_ok {extra_dict} {
    set json [yajl create #auto]
    $json map_open
    $json string "status" string "ok"
    dict for {k v} $extra_dict {
        if {[string is integer -strict $v]} {
            $json string $k number $v
        } else {
            $json string $k string $v
        }
    }
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# ============================================================================
# Initialize
# ============================================================================

# Configure export_path from environment or defaults
if {[info exists ::env(ESS_EXPORT_PATH)]} {
    dservSet df/export_destination $::env(ESS_EXPORT_PATH)
}
    
setup_database $df_db_path
setup_work_dir $df_work_dir

# Subscribe to system path and project
dservAddExactMatch ess/system_path
dpointSetScript    ess/system_path process_system_path

dservAddExactMatch ess/project
dpointSetScript    ess/project process_project

# Subscribe to data directory
dservAddExactMatch ess/data_dir
dpointSetScript    ess/data_dir process_data_dir

# Subscribe to datafile changes for close detection
dservAddExactMatch ess/datafile_path
dpointSetScript    ess/datafile_path process_ess_datafile

# Subscribe to export destination (new name)
dservAddExactMatch df/export_destination
dpointSetScript    df/export_destination process_export_destination

# Touch to get current values
catch {dservTouch ess/system_path}
catch {dservTouch ess/project}
catch {dservTouch ess/data_dir}
catch {dservTouch df/export_destination}

# Clean up old download files
catch {cleanup_downloads 1}

puts "dfconf: Datafile manager ready (df 2.0 - obs/trials/export)"
