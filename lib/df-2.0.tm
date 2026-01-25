package provide df 2.0

package require dslog

namespace eval df {
    variable ess_root ""
    
    #
    # Set the ess root directory
    #
    proc set_ess_root {path} {
        variable ess_root
        set ess_root $path
    }
    
    #
    # Get the ess root directory
    #
    proc get_ess_root {} {
        variable ess_root
        return $ess_root
    }
    
    #
    # Extract column information from a loaded group
    #
    proc extract_column_info {g} {
        set all_columns [dg_tclListnames $g]
        
        set stimdg_cols [list]
        set ds_cols [list]
        set event_cols [list]
        set other_cols [list]
        
        foreach col $all_columns {
            if {[string match "<stimdg>*" $col]} {
                lappend stimdg_cols $col
            } elseif {[string match "<ds>*" $col]} {
                lappend ds_cols $col
            } elseif {[string match "e_*" $col]} {
                lappend event_cols $col
            } else {
                lappend other_cols $col
            }
        }
        
        # Detailed info per column
        set col_info [dict create]
        foreach col $all_columns {
            dict set col_info $col [dict create \
                type [dl_datatype $g:$col] \
                depth [dl_depth $g:$col] \
                length [dl_length $g:$col]]
        }
        
        return [dict create \
            columns $all_columns \
            stimdg_columns $stimdg_cols \
            ds_columns $ds_cols \
            event_columns $event_cols \
            other_columns $other_cols \
            column_info $col_info]
    }
    
    #
    # Add standard metadata columns to a trials dg
    #
    # Arguments:
    #   trials   - dg to add columns to
    #   f        - df::File object (for metadata)
    #   n_trials - number of trials (for replication)
    #
    # Adds columns: trialid, date, time, filename, system, protocol, variant, subject
    #
    proc add_metadata_columns {trials f n_trials} {
        set meta [$f meta]
        
        # Serial trial index (0 to n-1)
        dl_set $trials:trialid [dl_fromto 0 $n_trials]
        
        # File metadata (replicated for each trial)
        dl_set $trials:date [dl_replicate [dl_slist [dict get $meta date]] $n_trials]
        dl_set $trials:time [dl_replicate [dl_slist [dict get $meta time]] $n_trials]
        dl_set $trials:filename [dl_replicate [dl_slist [file tail [dict get $meta filepath]]] $n_trials]
        dl_set $trials:system [dl_replicate [dl_slist [dict get $meta system]] $n_trials]
        dl_set $trials:protocol [dl_replicate [dl_slist [dict get $meta protocol]] $n_trials]
        dl_set $trials:variant [dl_replicate [dl_slist [dict get $meta variant]] $n_trials]
        dl_set $trials:subject [dl_replicate [dl_slist [dict get $meta subject]] $n_trials]
    }
    
    #
    # Lightweight metadata extraction for indexing
    #
    proc metadata {filepath} {
        # Hardcoded event types (fixed in ess)
        set NAME_TYPE 1
        set SUBTYPE_TYPE 6
        set TIME_TYPE 8
        set ID_TYPE 18
        
        # Hardcoded subtypes
        set TIME_OPEN 0
        set ID_SUBJECT 1
        set ID_VARIANT 3
        
        set meta [dict create \
            filepath $filepath \
            subject "" \
            system "" \
            protocol "" \
            variant "" \
            date "" \
            time "" \
            timestamp 0 \
            n_obs 0 \
            file_size 0 \
            columns [list] \
            stimdg_columns [list] \
            ds_columns [list] \
            event_columns [list] \
            other_columns [list] \
            column_info [dict create]]
        
        if {[file exists $filepath]} {
            dict set meta file_size [file size $filepath]
        }
        
        # Group name is based on filename - delete if exists to allow re-read
        set groupname [file rootname [file tail $filepath]]
        catch {dg_delete $groupname}
        
        if {[catch {set g [dslog::readESS $filepath]} err]} {
            return $meta
        }
        
        # Extract pre-event columns
        dl_local einfo [dl_unpack [dl_choose $g:e_pre [dl_llist 0]]]
        dl_local edata [dl_unpack [dl_choose $g:e_pre [dl_llist 1]]]
        dl_local ptypes [dl_unpack [dl_choose $einfo [dl_llist 0]]]
        dl_local psubtypes [dl_unpack [dl_choose $einfo [dl_llist 1]]]
        
        # TIME/OPEN - file timestamp
        dl_local mask [dl_and [dl_eq $ptypes $TIME_TYPE] [dl_eq $psubtypes $TIME_OPEN]]
        if {[dl_sum $mask] > 0} {
            set ts [dl_tcllist [dl_first [dl_select $edata $mask]]]
            dict set meta timestamp $ts
            dict set meta date [clock format $ts -format "%Y-%m-%d"]
            dict set meta time [clock format $ts -format "%H:%M:%S"]
        }
        
        # ID/VARIANT - system:protocol:variant
        dl_local mask [dl_and [dl_eq $ptypes $ID_TYPE] [dl_eq $psubtypes $ID_VARIANT]]
        if {[dl_sum $mask] > 0} {
            set sysinfo [dl_tcllist [dl_first [dl_select $edata $mask]]]
            set parts [split $sysinfo ":"]
            if {[llength $parts] >= 3} {
                dict set meta system [lindex $parts 0]
                dict set meta protocol [lindex $parts 1]
                dict set meta variant [lindex $parts 2]
            } elseif {[llength $parts] == 2} {
                dict set meta system [lindex $parts 0]
                dict set meta protocol [lindex $parts 1]
            }
        }
        
        # ID/SUBJECT
        dl_local mask [dl_and [dl_eq $ptypes $ID_TYPE] [dl_eq $psubtypes $ID_SUBJECT]]
        if {[dl_sum $mask] > 0} {
            dict set meta subject [dl_tcllist [dl_first [dl_select $edata $mask]]]
        }
        
        # Obs period count
        if {[dl_exists $g:e_types]} {
            dict set meta n_obs [dl_length $g:e_types]
        }
        
        # Column information
        set col_data [extract_column_info $g]
        dict for {key val} $col_data {
            dict set meta $key $val
        }
        
        dg_delete $g
        return $meta
    }
    
    #
    # Load and extract trial data - dispatches to system/protocol extractors
    #
    proc load_data {filepath args} {
        variable ess_root
        
        if {$ess_root eq ""} {
            error "df::ess_root not set - call df::set_ess_root first"
        }
        
        set meta [metadata $filepath]
        set system [dict get $meta system]
        set protocol [dict get $meta protocol]
        
        if {$system eq ""} {
            error "Could not determine system from file: $filepath"
        }
        
        # Look for and source extractors
        # Path structure: $ess_root/$system/${system}_extract.tcl
        set sys_extract [file join $ess_root $system ${system}_extract.tcl]
        if {[file exists $sys_extract]} {
            uplevel #0 [list source $sys_extract]
        }
        
        if {$protocol ne ""} {
            # Path structure: $ess_root/$system/$protocol/${protocol}_extract.tcl
            set proto_extract [file join $ess_root $system $protocol ${protocol}_extract.tcl]
            if {[file exists $proto_extract]} {
                uplevel #0 [list source $proto_extract]
            }
        }
        
        # Open file
        set f [df::File new $filepath]
        
        # Run system extractor (required)
        set sys_proc "::${system}::extract_trials"
        if {[info commands $sys_proc] eq ""} {
            $f destroy
            error "No extractor found: $sys_proc (looked in $sys_extract)"
        }
        set result [{*}$sys_proc $f {*}$args]
        
        # Run protocol extractor if exists (optional second pass)
        if {$protocol ne ""} {
            set proto_proc "::${system}::${protocol}::extract_trials"
            if {[info commands $proto_proc] ne ""} {
                set result [{*}$proto_proc $f $result {*}$args]
            }
        }
        
        $f destroy
        return $result
    }
    
    #
    # Full file access for analysis
    #
    catch {df::File destroy}
    
    oo::class create File {
        variable filepath g predg meta
        variable type_names type_ids subtypes
        
        constructor {path} {
            set filepath $path
            
            # Group name is based on filename - delete if exists to allow re-read
            set groupname [file rootname [file tail $filepath]]
            catch {dg_delete $groupname}
            
            # Read the ess file - we own this group
            set g [dslog::readESS $filepath]
            
            # Create group to hold pre-event data
            set predg [dg_create]
            
            # Extract pre-event columns
            my ExtractPre
            
            # Build type/subtype mappings
            my ExtractTypeMappings
            
            # Extract standard metadata
            my ExtractMetadata
        }
        
        destructor {
            if {[info exists g] && $g ne ""} {
                dg_delete $g
            }
            if {[info exists predg] && $predg ne ""} {
                dg_delete $predg
            }
        }
        
        method ExtractPre {} {
            dl_local einfo [dl_unpack [dl_choose $g:e_pre [dl_llist 0]]]
            dl_local edata [dl_unpack [dl_choose $g:e_pre [dl_llist 1]]]
            
            dl_set $predg:types [dl_unpack [dl_choose $einfo [dl_llist 0]]]
            dl_set $predg:subtypes [dl_unpack [dl_choose $einfo [dl_llist 1]]]
            dl_set $predg:data $edata
        }
        
        method ExtractTypeMappings {} {
            # Type names: event type 1
            dl_local name_evts [dl_eq $predg:types 1]
            dl_local ids [dl_slist {*}[dl_tcllist [dl_select $predg:subtypes $name_evts]]]
            dl_local names [dl_unpack [dl_select $predg:data $name_evts]]
            
            # id -> name
            dl_local pairs [dl_transpose [dl_llist $ids $names]]
            set type_names [dict create {*}[dl_tcllist [dl_collapse $pairs]]]
            
            # name -> id
            dl_local pairs_rev [dl_transpose [dl_llist $names $ids]]
            set type_ids [dict create {*}[dl_tcllist [dl_collapse $pairs_rev]]]
            
            # Subtype names: event type 6
            my ExtractSubtypes
        }
        
        method ExtractSubtypes {} {
            set subtypes [dict create]
            
            dl_local subtype_evts [dl_eq $predg:types 6]
            set type_ids_list [dl_tcllist [dl_select $predg:subtypes $subtype_evts]]
            set subtype_dicts [dl_tcllist [dl_unpack [dl_select $predg:data $subtype_evts]]]
            
            foreach type_id $type_ids_list subtype_dict $subtype_dicts {
                # payload is already {subtype_name subtype_id ...}
                # store directly keyed by type_id
                dict set subtypes $type_id $subtype_dict
            }
        }
        
        method FindPreEvent {type_id subtype_id} {
            dl_local mask [dl_and \
                [dl_eq $predg:types $type_id] \
                [dl_eq $predg:subtypes $subtype_id]]
            
            if {[dl_sum $mask] > 0} {
                return [dl_tcllist [dl_first [dl_select $predg:data $mask]]]
            }
            return ""
        }
        
        method ExtractMetadata {} {
            # Hardcoded subtypes for system events (may not have SUBTYPE events)
            set TIME_OPEN 0
            set ID_SUBJECT 1
            set ID_VARIANT 3
            
            set meta [dict create \
                filepath $filepath \
                subject "" \
                system "" \
                protocol "" \
                variant "" \
                date "" \
                time "" \
                timestamp 0 \
                n_obs 0 \
                file_size 0 \
                columns [list] \
                stimdg_columns [list] \
                ds_columns [list] \
                event_columns [list] \
                other_columns [list] \
                column_info [dict create]]
            
            if {[file exists $filepath]} {
                dict set meta file_size [file size $filepath]
            }
            
            # TIME/OPEN - file timestamp (use integer subtype)
            lassign [my evt TIME $TIME_OPEN] t s
            set ts [my FindPreEvent $t $s]
            if {$ts ne ""} {
                dict set meta timestamp $ts
                dict set meta date [clock format $ts -format "%Y-%m-%d"]
                dict set meta time [clock format $ts -format "%H:%M:%S"]
            }
            
            # ID/VARIANT - system:protocol:variant (use integer subtype)
            lassign [my evt ID $ID_VARIANT] t s
            set sysinfo [my FindPreEvent $t $s]
            if {$sysinfo ne ""} {
                set parts [split $sysinfo ":"]
                if {[llength $parts] >= 3} {
                    dict set meta system [lindex $parts 0]
                    dict set meta protocol [lindex $parts 1]
                    dict set meta variant [lindex $parts 2]
                } elseif {[llength $parts] == 2} {
                    dict set meta system [lindex $parts 0]
                    dict set meta protocol [lindex $parts 1]
                }
            }
            
            # ID/SUBJECT (use integer subtype)
            lassign [my evt ID $ID_SUBJECT] t s
            set subj [my FindPreEvent $t $s]
            if {$subj ne ""} {
                dict set meta subject $subj
            }
            
            # Obs period count
            if {[dl_exists $g:e_types]} {
                dict set meta n_obs [dl_length $g:e_types]
            }
            
            # Column information
            set col_data [df::extract_column_info $g]
            dict for {key val} $col_data {
                dict set meta $key $val
            }
        }
        
        #
        # Public API
        #
        
        method evt {type_name {subtype_name ""}} {
            if {[string is integer -strict $type_name]} {
                set t $type_name
            } elseif {[dict exists $type_ids $type_name]} {
                set t [dict get $type_ids $type_name]
            } else {
                puts "Warning: Unknown type '$type_name'"
                return ""
            }
            
            if {$subtype_name eq ""} {
                return $t
            }
            
            if {[string is integer -strict $subtype_name]} {
                set s $subtype_name
            } elseif {[dict exists $subtypes $t] && [dict exists [dict get $subtypes $t] $subtype_name]} {
                set s [dict get $subtypes $t $subtype_name]
            } else {
                puts "Warning: Unknown subtype '$subtype_name' for type '$type_name' (id=$t)"
                return ""
            }
            
            return [list $t $s]
        }
        
        method select_evt {type_name {subtype_name ""}} {
            set t [my evt $type_name]
            if {$t eq ""} {
                return ""
            }
            
            if {$subtype_name eq ""} {
                # Return all events of this type
                dl_return [dl_eq $g:e_types $t]
            } else {
                lassign [my evt $type_name $subtype_name] t s
                if {$t eq ""} {
                    return ""
                }
                dl_return [dl_and [dl_eq $g:e_types $t] [dl_eq $g:e_subtypes $s]]
            }
        }
        
        method event_times {mask} {
            dl_return [dl_select $g:e_times $mask]
        }
        
        method event_subtypes {mask} {
            dl_return [dl_select $g:e_subtypes $mask]
        }
        
        method event_params {mask} {
            dl_return [dl_select $g:e_params $mask]
        }
        
        #
        # Convenience methods that select and unpack in one step
        # These return flat lists suitable for direct use
        #
        
        # Get param values for events matching type/subtype, properly unpacked
        method event_param_values {type_name {subtype_name ""}} {
            set mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_return [dl_unpack [dl_deepUnpack [dl_select $g:e_params $mask]]]
        }
        
        # Get time values for events matching type/subtype, properly unpacked
        method event_time_values {type_name {subtype_name ""}} {
            set mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_return [dl_unpack [dl_select $g:e_times $mask]]
        }
        
        # Get subtype values for events matching type/subtype, properly unpacked
        method event_subtype_values {type_name {subtype_name ""}} {
            set mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_return [dl_unpack [dl_select $g:e_subtypes $mask]]
        }
        
        method meta {{key ""}} {
            if {$key eq ""} {
                return $meta
            }
            dict get $meta $key
        }
        
        method group {} {
            return $g
        }
        
        method type_name {id} {
            dict get $type_names $id
        }
        
        method type_id {name} {
            dict get $type_ids $name
        }
        
        method subtype_id {type_name subtype_name} {
            set t [my evt $type_name]
            dict get $subtypes $t $subtype_name
        }
        
        # Check if an event type exists in this file's event dictionary
        method has_event_type {type_name} {
            if {[string is integer -strict $type_name]} {
                return [dict exists $type_names $type_name]
            }
            return [dict exists $type_ids $type_name]
        }
        
        method type_names {} {
            return $type_names
        }
        
        method type_ids {} {
            return $type_ids
        }
        
        method subtypes {{type_name ""}} {
            if {$type_name eq ""} {
                return $subtypes
            }
            set t [my evt $type_name]
            if {[dict exists $subtypes $t]} {
                return [dict get $subtypes $t]
            }
            return [dict create]
        }
    }
}

namespace eval df {
    
    # ========================================================================
    # Export Functions - Three Levels
    # ========================================================================
    
    #
    # Level 1: Raw - direct .ess to .dgz conversion, minimal processing
    # Preserves the event stream exactly as recorded
    #
    # Arguments:
    #   filepath  - Full path to source .ess file
    #   outpath   - Full path for output .dgz file
    #
    # Returns: output path on success
    #
    proc export_raw {filepath outpath} {
        if {![file exists $filepath]} {
            error "Source file not found: $filepath"
        }
        
        # Read raw data
        set g [dslog::read $filepath]
        
        # Save compressed
        dg_write $g $outpath
        dg_delete $g
        
        return $outpath
    }
    
    #
    # Level 2: Trials - obs-period oriented structure via readESS
    # Organizes data by observation periods but doesn't run extractors
    #
    # Arguments:
    #   filepath  - Full path to source .ess file
    #   outpath   - Full path for output .dgz file
    #
    # Returns: output path on success
    #
    proc export_trials {filepath outpath} {
        if {![file exists $filepath]} {
            error "Source file not found: $filepath"
        }
        
        # Read with obs-period structure
        set g [dslog::readESS $filepath]
        
        # Save compressed
        dg_write $g $outpath
        dg_delete $g
        
        return $outpath
    }
    
    #
    # Level 3: Extracted - full extraction with system/protocol extractors
    # This is the "analysis-ready" format most users want
    #
    # Arguments:
    #   filepath  - Full path to source .ess file
    #   outpath   - Full path for output .dgz file
    #   args      - Additional arguments passed to load_data
    #
    # Returns: output path on success
    #
    proc export_extracted {filepath outpath args} {
        if {![file exists $filepath]} {
            error "Source file not found: $filepath"
        }
        
        # Use load_data which sources and runs extractors
        set g [load_data $filepath {*}$args]
        
        # Save compressed
        dg_write $g $outpath
        dg_delete $g
        
        return $outpath
    }
    
    #
    # Generate output filename for export
    # 
    # Arguments:
    #   filepath  - Source .ess filepath
    #   level     - raw | trials | extracted
    #   outdir    - Output directory
    #
    # Returns: Full output path
    #
    proc export_outpath {filepath level outdir} {
        set base [file rootname [file tail $filepath]]
        
        switch $level {
            raw {
                return [file join $outdir "${base}_raw.dgz"]
            }
            trials {
                return [file join $outdir "${base}_trials.dgz"]
            }
            extracted {
                return [file join $outdir "${base}.dgz"]
            }
            default {
                error "Unknown export level: $level (use raw, trials, or extracted)"
            }
        }
    }
    
    #
    # Export a file at specified level (convenience wrapper)
    #
    # Arguments:
    #   filepath  - Source .ess filepath
    #   level     - raw | trials | extracted
    #   outdir    - Output directory
    #
    # Returns: Output filepath
    #
    proc export {filepath level outdir} {
        file mkdir $outdir
        
        set outpath [export_outpath $filepath $level $outdir]
        
        switch $level {
            raw {
                return [export_raw $filepath $outpath]
            }
            trials {
                return [export_trials $filepath $outpath]
            }
            extracted {
                return [export_extracted $filepath $outpath]
            }
            default {
                error "Unknown export level: $level"
            }
        }
    }
    
    #
    # Generate code snippet to load file(s) in external tools
    #
    # Arguments:
    #   filepaths - List of filepaths (can be .ess or .dgz)
    #   language  - python | r | matlab
    #
    # Returns: Code string
    #
    proc generate_load_code {filepaths language} {
        set code ""
        
        switch $language {
            python {
                set code "from dgread import dg_read\n\n"
                if {[llength $filepaths] == 1} {
                    set code "${code}data = dg_read('[lindex $filepaths 0]')\n"
                } else {
                    set code "${code}files = \[\n"
                    foreach p $filepaths {
                        set code "${code}    '$p',\n"
                    }
                    set code "${code}\]\n\n"
                    set code "${code}# Load all files\n"
                    set code "${code}data = \[dg_read(f) for f in files\]\n"
                }
            }
            
            r {
                set code "# library(dgread)  # if installed as package\n"
                set code "${code}# source('dgread.R')  # or source directly\n\n"
                if {[llength $filepaths] == 1} {
                    set code "${code}data <- dg_read('[lindex $filepaths 0]')\n"
                } else {
                    set code "${code}files <- c(\n"
                    foreach p $filepaths {
                        set code "${code}  '[string map {' \\'} $p]',\n"
                    }
                    set code "${code})\n\n"
                    set code "${code}# Load all files\n"
                    set code "${code}data <- lapply(files, dg_read)\n"
                }
            }
            
            matlab {
                set code "% Requires dgread.m in path\n\n"
                if {[llength $filepaths] == 1} {
                    set code "${code}data = dg_read('[lindex $filepaths 0]');\n"
                } else {
                    set code "${code}files = {\n"
                    foreach p $filepaths {
                        set code "${code}    '$p'\n"
                    }
                    set code "${code}};\n\n"
                    set code "${code}% Load all files\n"
                    set code "${code}data = cell(length(files), 1);\n"
                    set code "${code}for i = 1:length(files)\n"
                    set code "${code}    data{i} = dg_read(files{i});\n"
                    set code "${code}end\n"
                }
            }
            
            default {
                error "Unknown language: $language (use python, r, or matlab)"
            }
        }
        
        return $code
    }
}