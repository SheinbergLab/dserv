package provide df 2.0

package require dslog

namespace eval df {
    variable ess_root ""
    variable work_dir ""
    variable export_dir ""
    
    #
    # Configuration
    #
    proc set_ess_root {path} {
        variable ess_root
        set ess_root $path
    }
    
    proc get_ess_root {} {
        variable ess_root
        return $ess_root
    }
    
    proc set_work_dir {path} {
        variable work_dir
        set work_dir $path
        
        # Create subdirectories
        if {$path ne ""} {
            file mkdir [file join $path obs]
            file mkdir [file join $path trials]
        }
    }
    
    proc get_work_dir {} {
        variable work_dir
        return $work_dir
    }
    
    proc set_export_dir {path} {
        variable export_dir
        set export_dir $path
        
        # Create subdirectories
        if {$path ne ""} {
            file mkdir [file join $path ess]
            file mkdir [file join $path obs]
            file mkdir [file join $path trials]
        }
    }
    
    proc get_export_dir {} {
        variable export_dir
        return $export_dir
    }
    
    # Convenience accessors for subdirectories
    proc obs_dir {} {
        variable work_dir
        return [file join $work_dir obs]
    }
    
    proc trials_dir {} {
        variable work_dir
        return [file join $work_dir trials]
    }
    
    proc export_ess_dir {} {
        variable export_dir
        return [file join $export_dir ess]
    }
    
    proc export_obs_dir {} {
        variable export_dir
        return [file join $export_dir obs]
    }
    
    proc export_trials_dir {} {
        variable export_dir
        return [file join $export_dir trials]
    }
    
    #
    # Extract column information from a loaded group
    #
    proc extract_column_info {g} {
        set all_columns [dg_tclListnames $g]

        set stimdg_cols [list]
        set ds_cols [list]
        set dst_cols [list]
        set dsn_cols [list]
        set blob_cols [list]
        set session_cols [list]
        set event_cols [list]
        set other_cols [list]

        foreach col $all_columns {
            if {[string match "<stimdg>*" $col]} {
                lappend stimdg_cols $col
            } elseif {[string match "<ds>*" $col]} {
                lappend ds_cols $col
            } elseif {[string match "<dst>*" $col]} {
                lappend dst_cols $col
            } elseif {[string match "<dsn>*" $col]} {
                lappend dsn_cols $col
            } elseif {[string match "<blob>*" $col] ||
                      [string match "<blobt>*" $col]} {
                lappend blob_cols $col
            } elseif {[string match "<session>*" $col]} {
                lappend session_cols $col
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
            ds_time_columns $dst_cols \
            ds_count_columns $dsn_cols \
            blob_columns $blob_cols \
            session_columns $session_cols \
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
	dl_set $trials:hostname [dl_replicate [dl_slist [dict get $meta hostname]] $n_trials]
        dl_set $trials:filename [dl_replicate [dl_slist [file tail [dict get $meta filepath]]] $n_trials]
        dl_set $trials:system [dl_replicate [dl_slist [dict get $meta system]] $n_trials]
        dl_set $trials:protocol [dl_replicate [dl_slist [dict get $meta protocol]] $n_trials]
        dl_set $trials:variant [dl_replicate [dl_slist [dict get $meta variant]] $n_trials]
        dl_set $trials:subject [dl_replicate [dl_slist [dict get $meta subject]] $n_trials]
    }
    
    #
    # Validate that a dg is rectangular (all columns same length)
    #
    # Arguments:
    #   dg_name - name of the datagroup to validate
    #   context - optional string describing context for error message
    #
    # Returns: 1 if valid, throws error if not
    #
    proc validate_rectangular {dg_name {context ""}} {
        set cols [dg_tclListnames $dg_name]
        if {[llength $cols] == 0} {
            return 1
        }
        
        set lengths [dict create]
        foreach col $cols {
            set len [dl_length $dg_name:$col]
            dict set lengths $col $len
        }
        
        set unique_lens [lsort -unique [dict values $lengths]]
        if {[llength $unique_lens] > 1} {
            # Build detailed error message
            set msg "Non-rectangular datagroup"
            if {$context ne ""} {
                append msg " in $context"
            }
            append msg ":\n"
            
            # Group columns by length
            set by_len [dict create]
            dict for {col len} $lengths {
                dict lappend by_len $len $col
            }
            
            dict for {len cols} $by_len {
                append msg "  length $len: [join $cols {, }]\n"
            }
            
            error $msg
        }
        
        return 1
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
        
        # Validate rectangular result
        validate_rectangular $result "extract_trials for $filepath"
        
        $f destroy
        return $result
    }
    
    #
    # Load and extract from an obs dg (for re-extraction)
    # This is the preferred method when obs already exists
    #
    proc load_data_from_obs {obs_path args} {
        variable ess_root
        
        if {$ess_root eq ""} {
            error "df::ess_root not set - call df::set_ess_root first"
        }
        
        # Load the obs dg
        set g [dg_read $obs_path]
        
        # Extract metadata from obs dg
        set meta [extract_metadata_from_obs $g]
        set system [dict get $meta system]
        set protocol [dict get $meta protocol]
        
        if {$system eq ""} {
            dg_delete $g
            error "Could not determine system from obs file: $obs_path"
        }
        
        # Source extractor
        set sys_extract [file join $ess_root $system ${system}_extract.tcl]
        if {[file exists $sys_extract]} {
            uplevel #0 [list source $sys_extract]
        }
        
        if {$protocol ne ""} {
            set proto_extract [file join $ess_root $system $protocol ${protocol}_extract.tcl]
            if {[file exists $proto_extract]} {
                uplevel #0 [list source $proto_extract]
            }
        }
        
        # Create a File-like object for the extractor
        # Or call extractor directly with obs dg
        set sys_proc "::${system}::extract_trials_from_obs"
        if {[info commands $sys_proc] eq ""} {
            # Fall back to standard extractor with File wrapper
            set sys_proc "::${system}::extract_trials"
            if {[info commands $sys_proc] eq ""} {
                dg_delete $g
                error "No extractor found: $sys_proc"
            }
            # Need to create File from obs - this is a compatibility path
            # For now, error out and require obs-aware extractor
            dg_delete $g
            error "Extractor $sys_proc does not support obs input. Implement ::${system}::extract_trials_from_obs"
        }
        
        set result [{*}$sys_proc $g $meta {*}$args]
        
        # Run protocol extractor if exists
        if {$protocol ne ""} {
            set proto_proc "::${system}::${protocol}::extract_trials_from_obs"
            if {[info commands $proto_proc] ne ""} {
                set result [{*}$proto_proc $g $meta $result {*}$args]
            }
        }
        
        # Validate rectangular result
        validate_rectangular $result "extract_trials_from_obs for $obs_path"
        
        dg_delete $g
        return $result
    }
    
    #
    # Extract metadata from an obs dg (already loaded)
    #
    proc extract_metadata_from_obs {g} {
        # Hardcoded event types
        set TIME_TYPE 8
        set ID_TYPE 18
        set TIME_OPEN 0
        set ID_SUBJECT 1
        set ID_VARIANT 3
        
        set meta [dict create \
            subject "" \
            system "" \
            protocol "" \
            variant "" \
            date "" \
            time "" \
            timestamp 0 \
            n_obs 0]
        
        # Get pre-event data
        dl_local einfo [dl_unpack [dl_choose $g:e_pre [dl_llist 0]]]
        dl_local edata [dl_unpack [dl_choose $g:e_pre [dl_llist 1]]]
        dl_local ptypes [dl_unpack [dl_choose $einfo [dl_llist 0]]]
        dl_local psubtypes [dl_unpack [dl_choose $einfo [dl_llist 1]]]
        
        # TIME/OPEN
        dl_local mask [dl_and [dl_eq $ptypes $TIME_TYPE] [dl_eq $psubtypes $TIME_OPEN]]
        if {[dl_sum $mask] > 0} {
            set ts [dl_tcllist [dl_first [dl_select $edata $mask]]]
            dict set meta timestamp $ts
            dict set meta date [clock format $ts -format "%Y-%m-%d"]
            dict set meta time [clock format $ts -format "%H:%M:%S"]
        }
        
        # ID/VARIANT
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
        
        # Obs count
        if {[dl_exists $g:e_types]} {
            dict set meta n_obs [dl_length $g:e_types]
        }
        
        return $meta
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
            set TIME_OPEN   0
            set ID_SUBJECT  1
            set ID_VARIANT  3
	    set ID_HOSTNAME 4
            
            set meta [dict create \
                filepath $filepath \
		hostname "" \
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

            # ID/HOSTNAME 
            lassign [my evt ID $ID_HOSTNAME] t s
            set hostname [my FindPreEvent $t $s]
            if {$hostname ne ""} {
                dict set meta hostname $hostname
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
        # Convenience methods - get values for all obs periods
        # These unpack immediately and return one value per obs period.
        # For extraction, consider using the _valid variants which handle
        # missing events correctly when filtering to valid trials.
        #
        
        # Get param values for events matching type/subtype, properly unpacked
        method event_param_values {type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_return [dl_unpack [dl_deepUnpack [dl_select $g:e_params $mask]]]
        }
        
        # Get time values for events matching type/subtype, properly unpacked
        method event_time_values {type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_return [dl_unpack [dl_select $g:e_times $mask]]
        }
        
        # Get subtype values for events matching type/subtype, properly unpacked
        method event_subtype_values {type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_return [dl_unpack [dl_select $g:e_subtypes $mask]]
        }
        
        #
        # Safe extraction methods - select before unpacking
        #
        method event_times_valid {valid_mask type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_local times_nested [dl_select $g:e_times $mask]
            dl_local valid_indices [dl_indices $valid_mask]
            dl_local times_valid [dl_choose $times_nested $valid_indices]
            dl_return [dl_unpack $times_valid]
        }
        
        method event_params_valid {valid_mask type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_local params_nested [dl_select $g:e_params $mask]
            dl_local valid_indices [dl_indices $valid_mask]
            dl_local params_valid [dl_choose $params_nested $valid_indices]
            dl_return [dl_unpack [dl_deepUnpack $params_valid]]
        }
        
        method event_subtypes_valid {valid_mask type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return "" }
            dl_local subtypes_nested [dl_select $g:e_subtypes $mask]
            dl_local valid_indices [dl_indices $valid_mask]
            dl_local subtypes_valid [dl_choose $subtypes_nested $valid_indices]
            dl_return [dl_unpack $subtypes_valid]
        }
        
        #
        # Sparse event extraction methods
        #
        # Handle events that may not occur on every trial.
        # Fill missing trials with a scalar value to maintain rectangularity.
        # valid_indices is a dl of integer indices (not a boolean mask).
        #
        
        method event_time_sparse {valid_indices type_name subtype_name {fill -1}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            dl_local has_evt [dl_anys $mask]
            dl_local no_evt [dl_not $has_evt]
            dl_local times [dl_select $g:e_times $mask]
            dl_local times [dl_replace $times $no_evt [dl_llist [dl_ilist $fill]]]
            dl_return [dl_unpack [dl_choose $times $valid_indices]]
        }
        
        method event_subtype_sparse {valid_indices type_name subtype_name {fill -1}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            dl_local has_evt [dl_anys $mask]
            dl_local no_evt [dl_not $has_evt]
            dl_local svals [dl_select $g:e_subtypes $mask]
            dl_local svals [dl_replace $svals $no_evt [dl_llist [dl_ilist $fill]]]
            dl_return [dl_unpack [dl_choose $svals $valid_indices]]
        }
        
        method event_param_sparse {valid_indices type_name subtype_name {fill -1}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            dl_local has_evt [dl_anys $mask]
            dl_local no_evt [dl_not $has_evt]
            dl_local params [dl_select $g:e_params $mask]
            # Use a float fill list when the fill value isn't an integer, so
            # float-typed param columns (e.g. SWIPE COMMIT angles) unpack cleanly.
            if {[string is integer -strict $fill]} {
                dl_local fill_sub [dl_ilist $fill]
            } else {
                dl_local fill_sub [dl_flist $fill]
            }
            dl_local params [dl_replace $params $no_evt [dl_llist [dl_llist $fill_sub]]]
            dl_return [dl_unpack [dl_unpack [dl_choose $params $valid_indices]]]
        }
        
        #
        # Nested event extraction methods
        #
        # For events with zero or more occurrences per trial.
        # Returns nested list (one sublist per valid trial).
        #
        
        method event_times_nested {valid_indices type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            dl_local times [dl_select $g:e_times $mask]
            dl_return [dl_choose $times $valid_indices]
        }
        
        method event_params_nested {valid_indices type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            dl_local params [dl_select $g:e_params $mask]
            dl_return [dl_choose $params $valid_indices]
        }
        
        # Check if a specific event type (and optional subtype) actually
        # occurred in any obs period (not just registered in the event table)
        method has_event_occurrences {type_name {subtype_name ""}} {
            dl_local mask [my select_evt $type_name $subtype_name]
            if {$mask eq ""} { return 0 }
            return [dl_any [dl_anys $mask]]
        }
        
        #
        # Standard accessors
        #
        
        method meta {{key ""}} {
            if {$key eq ""} {
                return $meta
            }
            dict get $meta $key
        }

        method group {} {
            return $g
        }

        #
        # Datapoint streams per obs (readESS emits three parallel columns
        # per recorded stream):
        #   <ds>NAME   values, concatenated across the obs's records
        #   <dst>NAME  per-record times, ms from obs onset (e_times' axis)
        #   <dsn>NAME  per-record value counts (split <ds> back into records)
        # Returns the three column names {values times counts}; each is a
        # nested list of length n_obs.
        #
        method ds_in_obs {varname} {
            set vcol "<ds>$varname"
            if {![dl_exists $g:$vcol]} {
                error "no $vcol column in [file tail $filepath] -- stream not recorded"
            }
            if {![dl_exists $g:<dst>$varname]} {
                error "no <dst>$varname column -- file was read with a\
                       pre-timestamp readESS (rebuild dlsh)"
            }
            return [list $g:$vcol "$g:<dst>$varname" "$g:<dsn>$varname"]
        }

        #
        # Per-SAMPLE times and values for a buffered fixed-width stream.
        #
        # ds_in_obs hands back the RECORDS the logger wrote, and for a fast
        # source those records are BUFFERED: dservLoggerAddMatch's buffer
        # concatenates the data of successive datapoints and keeps only the
        # FIRST one's timestamp. A 10-sample block therefore carries one
        # time, not ten, and reading <dst> as a sample axis stacks every
        # sample of a block at the block's onset.
        #
        # THE RIGHT ANSWER IS -time_index. If the stream's payload carries
        # the sample's own time -- ess/roam/pos publishes {x y t} for
        # exactly this reason, and the extio ain blocks carry their t0 the
        # same way -- say which channel it is and NOTHING below runs. Each
        # record's stamp is its first sample's exact time and every other
        # sample is that plus its own payload offset, per record, so a
        # payload clock that restarts mid-obs costs at most one record.
        # The time channel is removed from the returned values, so a caller
        # sees the same channel numbering either way.
        #
        # Everything from here down is the FALLBACK for a stream that does
        # not: files written before a source started stamping itself, and
        # sources that genuinely are uniform. What the file still pins down
        # exactly:
        #   - the first sample of a record is at that record's time
        #   - every sample of a record falls before the next record's time
        #   - a source publishing on a fixed tick spaces them by that tick
        #
        # The tick is INFERRED from the file -- the median of span/n over
        # every non-terminal record -- rather than declared, so this follows
        # a retuned roam_tick_ms or a different sampler with no argument.
        # Pass -interval to override it.
        #
        # Reconstruction, per record:
        #
        #   span <= 1.5*n*tick   contiguous: the source ran at the tick for
        #                        the whole span, so the samples spread
        #                        evenly across it.
        #
        #   otherwise            the source PAUSED. Only a publish-on-change
        #                        source can (ess/roam/pos publishes only
        #                        when the agent moved), and the pause is put
        #                        directly after the record's first sample:
        #                        the rest are back-filled at the tick so
        #                        they end one tick before the next record.
        #                        That is exactly right for the FIRST record
        #                        of an obs -- a seed sample at onset, then a
        #                        wait for the subject's first move -- which
        #                        is the case that matters and the one an
        #                        even spread gets most wrong. Where inside a
        #                        record the pause actually fell is not
        #                        recoverable; the bound on it is.
        #
        # The last record of an obs has no successor, so its samples run
        # forward from its own time at the tick.
        #
        # A stream with one sample per record is not buffered at all: every
        # time is exact and nothing is inferred.
        #
        # width is the values per sample (3 for an x,y,t triple).
        #
        # Returns two nested dynlists of length n_obs (persistent in this
        # object's scratch dg):
        #   times - float ms from obs onset, one per sample
        #   vals  - per obs: one list per channel, each n_samples long,
        #           minus the -time_index channel if one was named
        #
        method ds_samples_in_obs {varname args} {
            set opts [dict merge {-width 1 -interval 0 -time_index -1} $args]
            set width    [dict get $opts -width]
            set interval [dict get $opts -interval]
            set tidx     [dict get $opts -time_index]
            if {$width < 1} { error "ds_samples_in_obs: -width must be >= 1" }
            if {$tidx >= $width} {
                error "ds_samples_in_obs: -time_index $tidx is outside a\
                       -width $width sample"
            }

            lassign [my ds_in_obs $varname] vcol tcol ncol
            set n_obs [dict get $meta n_obs]

            # Record times and per-record SAMPLE counts, per obs.
            set otimes {}
            set ocounts {}
            for {set o 0} {$o < $n_obs} {incr o} {
                set ts {}
                set ns {}
                foreach t [dl_tcllist $tcol:$o] c [dl_tcllist $ncol:$o] {
                    if {$c % $width} {
                        error "$varname obs $o: a record holds $c values,\
                               not a multiple of -width $width"
                    }
                    lappend ts $t
                    lappend ns [expr {$c / $width}]
                }
                lappend otimes $ts
                lappend ocounts $ns
            }

            # Infer the tick from every record that has a successor.
            # Skipped entirely when the payload carries its own time.
            if {$tidx < 0 && $interval <= 0} {
                set spans {}
                foreach ts $otimes ns $ocounts {
                    for {set k 0} {$k < [llength $ts] - 1} {incr k} {
                        set n [lindex $ns $k]
                        if {$n <= 0} continue
                        lappend spans [expr {double([lindex $ts $k+1] -
                                                    [lindex $ts $k]) / $n}]
                    }
                }
                if {[llength $spans]} {
                    set spans [lsort -real $spans]
                    set interval [lindex $spans [expr {[llength $spans]/2}]]
                }
                # No successor anywhere means one record per obs; with one
                # sample in it the time is exact, with more it is not
                # recoverable and the caller has to say what the rate was.
                if {$interval <= 0} {
                    set buffered 0
                    foreach ns $ocounts {
                        foreach n $ns { if {$n > 1} { set buffered 1 } }
                    }
                    if {$buffered} {
                        error "$varname: buffered records but no two records\
                               in any obs to infer the sample interval from\
                               -- pass -interval"
                    }
                    set interval 1.0
                }
            }

            dl_local t_out [dl_llist]
            dl_local v_out [dl_llist]

            for {set o 0} {$o < $n_obs} {incr o} {
                set ts [lindex $otimes $o]
                set ns [lindex $ocounts $o]
                set nrec [llength $ts]
                set stamps {}

                # The payload carries the sample's own time. Nothing is
                # inferred: each record's stamp is the exact obs-relative
                # time of its FIRST sample, and every other sample in that
                # record is that stamp plus its own offset from the first.
                # Taken per RECORD rather than against one origin for the
                # whole obs, so a stream whose payload clock restarts
                # mid-obs (a roam restarted for a second bout) costs at
                # most the one record the restart falls in.
                if {$tidx >= 0} {
                    set pos 0
                    foreach t $ts n $ns {
                        if {$n <= 0} continue
                        dl_local pt [dl_choose $vcol:$o \
                            [dl_series [expr {$pos + $tidx}] \
                                       [expr {$pos + ($n-1)*$width + $tidx}] \
                                       $width]]
                        set p0 [dl_get $pt 0]
                        foreach p [dl_tcllist $pt] {
                            lappend stamps [expr {$t + ($p - $p0)}]
                        }
                        incr pos [expr {$n*$width}]
                    }
                    dl_append $t_out [dl_flist {*}$stamps]
                    set nsamp [llength $stamps]
                    dl_local chans [dl_llist]
                    for {set c 0} {$c < $width} {incr c} {
                        if {$c == $tidx} continue   ;# it is the time axis now
                        if {$nsamp} {
                            dl_append $chans [dl_choose $vcol:$o \
                                [dl_series $c [expr {$nsamp*$width - 1}] \
                                     $width]]
                        } else {
                            dl_append $chans [dl_flist]
                        }
                    }
                    dl_append $v_out $chans
                    continue
                }

                for {set k 0} {$k < $nrec} {incr k} {
                    set t [lindex $ts $k]
                    set n [lindex $ns $k]
                    if {$n <= 0} continue
                    lappend stamps [expr {double($t)}]      ;# exact
                    if {$n == 1} continue
                    if {$k == $nrec - 1} {
                        for {set j 1} {$j < $n} {incr j} {
                            lappend stamps [expr {$t + $j*$interval}]
                        }
                        continue
                    }
                    set span [expr {[lindex $ts $k+1] - $t}]
                    if {$span <= 1.5*$n*$interval} {
                        set step [expr {double($span)/$n}]
                        for {set j 1} {$j < $n} {incr j} {
                            lappend stamps [expr {$t + $j*$step}]
                        }
                    } else {
                        set end [expr {$t + $span - $interval}]
                        for {set j 1} {$j < $n} {incr j} {
                            lappend stamps [expr {$end - ($n-1-$j)*$interval}]
                        }
                    }
                }

                dl_append $t_out [dl_flist {*}$stamps]

                set nsamp [llength $stamps]
                dl_local chans [dl_llist]
                for {set c 0} {$c < $width} {incr c} {
                    if {$nsamp} {
                        dl_append $chans [dl_choose $vcol:$o \
                            [dl_series $c [expr {$nsamp*$width - 1}] $width]]
                    } else {
                        dl_append $chans [dl_flist]
                    }
                }
                dl_append $v_out $chans
            }

            # Scratch columns named after the STREAM, not a fixed ds_t/ds_v:
            # a caller decoding two streams (a path and a stick) holds both
            # handles at once, and a shared name would have the second call
            # quietly overwrite the first's data under the first's name.
            set tag [string map {/ _ - _ . _} $varname]
            dl_set $predg:ds_t_$tag $t_out
            dl_set $predg:ds_v_$tag $v_out
            return [list $predg:ds_t_$tag $predg:ds_v_$tag]
        }

        #
        # Camera frames grouped per obs by REQUEST pairing.
        #
        # readESS stores frames at FILE level (<blob>NAME byte vectors +
        # <blobt>NAME capture times, ms from the file's first record)
        # because a frame requested late in an obs is encoded async and
        # can be logged after ENDOBS.
        #
        # Pairing: a DONE event's second param (rel_ms, ess-2.0.tm
        # camera_grab_complete) is the frame's capture time minus its
        # REQUEST stamp -- negative for look-back grabs
        # (camera_grab_before), which capture BEFORE the request. Each
        # DONE request claims the unused frame nearest request_time +
        # rel_ms, within half a frame period: exact even when two grabs
        # land close together, and immune to a manual console grab's
        # stray frame. Files predating rel_ms (single-param DONE, all
        # grabs next-frame) pair by first unused frame at/after the
        # request, the old invariant.
        #
        # Returns three nested dynlists of length n_obs (persistent in
        # this object's scratch dg):
        #   request_t - ms from obs onset of each CAMERA REQUEST
        #   capture_t - ms from obs onset of the paired frame's sensor
        #               capture (-1 if no frame resolved the request;
        #               can be NEGATIVE for a look-back grab requested
        #               just after BEGINOBS)
        #   jpeg      - DF_CHAR byte vectors (empty if unresolved).
        #               Recover bytes: numpy arr.astype('uint8').tobytes()
        #
        # Request ids of CAMERA <subtype> events, from BOTH the per-obs
        # event stream and e_pre (a grab's DONE/FAIL is stamped when its
        # meta arrives, which can be after ENDOBS -- between obs).
        method CameraEvtParamLists {subtype_name} {
            set plists {}
            dl_local mask [my select_evt CAMERA $subtype_name]
            if {$mask ne ""} {
                foreach obs_params [dl_tcllist [dl_select $g:e_params $mask]] {
                    foreach p $obs_params { lappend plists $p }
                }
            }
            lassign [my evt CAMERA $subtype_name] t s
            if {$t ne ""} {
                dl_local pre_mask [dl_and [dl_eq $predg:types $t] \
                                       [dl_eq $predg:subtypes $s]]
                foreach p [dl_tcllist [dl_select $predg:data $pre_mask]] {
                    lappend plists $p
                }
            }
            return $plists
        }

        method CameraEvtIds {subtype_name} {
            return [lmap p [my CameraEvtParamLists $subtype_name] \
                        { lindex $p 0 }]
        }

        method camera_frames_in_obs {{varname camera/full}} {
            set n_obs [dict get $meta n_obs]

            dl_local req_out [dl_llist]
            dl_local cap_out [dl_llist]
            dl_local jpg_out [dl_llist]

            set bcol "<blob>$varname"
            set tcol "<blobt>$varname"
            set have_frames [dl_exists $g:$bcol]

            # per-obs request times and ids; "" if CAMERA never logged
            set req_lists {}
            set req_id_lists {}
            dl_local req_mask [my select_evt CAMERA REQUEST]
            if {$req_mask ne ""} {
                set req_lists [dl_tcllist [dl_select $g:e_times $req_mask]]
                set req_id_lists [dl_tcllist [dl_select $g:e_params $req_mask]]
            }

            # The contract's resolution events are the pairing evidence:
            # only a request with CAMERA DONE consumes a frame. A FAILed
            # or never-resolved request stays empty rather than claiming
            # some later stray frame (e.g. a manual grab). DONE's rel_ms
            # param (when present) makes the claim exact.
            set done_ids {}
            set done_rel [dict create]
            foreach p [my CameraEvtParamLists DONE] {
                set id [lindex $p 0]
                lappend done_ids $id
                if {[llength $p] >= 2} {
                    dict set done_rel $id [lindex $p 1]
                }
            }
            set fail_ids [my CameraEvtIds FAIL]

            set frame_times {}
            set obs_starts {}
            if {$have_frames} {
                set frame_times [dl_tcllist $g:$tcol]
                if {![dl_exists $g:obs_start_ms]} {
                    error "no obs_start_ms column -- file was read with a\
                           pre-timestamp readESS (rebuild dlsh)"
                }
                set obs_starts [dl_tcllist $g:obs_start_ms]
            }
            set n_frames [llength $frame_times]
            set used [lrepeat $n_frames 0]

            for {set o 0} {$o < $n_obs} {incr o} {
                dl_local rts [dl_ilist]
                dl_local cts [dl_ilist]
                dl_local jps [dl_llist]
                set ostart [lindex $obs_starts $o]
                foreach r [lindex $req_lists $o] \
                    rp [lindex $req_id_lists $o] {
                    set req_id [lindex $rp 0]
                    dl_append $rts $r
                    set found 0
                    if {$have_frames && $req_id in $done_ids &&
                        $req_id ni $fail_ids} {
                        set abs_r [expr {$ostart + $r}]
                        set claim -1
                        if {[dict exists $done_rel $req_id]} {
                            # Exact: the frame this DONE announced sits at
                            # request + rel_ms. Nearest unused, gated to
                            # 15 ms -- covers the <=4 ms of accumulated
                            # ms-rounding, rejects the 33 ms neighbor, so
                            # a frame missing from the log reads as
                            # unresolved instead of claiming its neighbor.
                            set expect \
                                [expr {$abs_r + [dict get $done_rel $req_id]}]
                            set best_dt 0
                            for {set i 0} {$i < $n_frames} {incr i} {
                                if {[lindex $used $i]} { continue }
                                set dt [expr {abs([lindex $frame_times $i] \
                                                      - $expect)}]
                                if {$claim < 0 || $dt < $best_dt} {
                                    set claim $i
                                    set best_dt $dt
                                }
                            }
                            if {$claim >= 0 && $best_dt > 15} { set claim -1 }
                        } else {
                            # Legacy files (no rel_ms): every grab was
                            # next-frame, so the old invariant holds --
                            # first unused frame captured at/after the
                            # request is the one.
                            for {set i 0} {$i < $n_frames} {incr i} {
                                if {[lindex $used $i]} { continue }
                                if {[lindex $frame_times $i] >= $abs_r} {
                                    set claim $i
                                    break
                                }
                            }
                        }
                        if {$claim >= 0} {
                            lset used $claim 1
                            dl_append $cts \
                                [expr {[lindex $frame_times $claim] - $ostart}]
                            dl_append $jps [dl_get $g:$bcol $claim]
                            set found 1
                        }
                    }
                    if {!$found} {
                        dl_append $cts -1
                        dl_append $jps [dl_clist]
                    }
                }
                dl_append $req_out $rts
                dl_append $cap_out $cts
                dl_append $jpg_out $jps
            }

            dl_set $predg:camera_req_t $req_out
            dl_set $predg:camera_cap_t $cap_out
            dl_set $predg:camera_jpeg $jpg_out
            return [list $predg:camera_req_t $predg:camera_cap_t \
                        $predg:camera_jpeg]
        }

        #
        # Decode a recorded extio analog stream (record_streams) per obs.
        #
        # varname is the block datapoint, e.g. extio/box02/state/ain/eye
        # (see ess/ain/recorded in the file for what was recorded). Each
        # record is a self-describing block: 12-byte header + scan-major
        # int16 samples (lib/extio-1.0.tm is the canonical decoder).
        # Sample k of a block is at block_time + k*interval_us.
        #
        # Returns two nested dynlists of length n_obs (persistent):
        #   times - float ms from obs onset, one per scan
        #   vals  - per obs: one DF_SHORT list per channel
        #
        method ain_samples_in_obs {varname} {
            if {[catch {package require extio} err]} {
                error "ain_samples_in_obs needs lib/extio-1.0.tm on the\
                       module path: $err"
            }
            lassign [my ds_in_obs $varname] vcol tcol ncol
            set n_obs [dict get $meta n_obs]

            dl_local t_out [dl_llist]
            dl_local v_out [dl_llist]

            for {set o 0} {$o < $n_obs} {incr o} {
                dl_local obs_bytes [dl_unpack [dl_choose $vcol [dl_ilist $o]]]
                set nbytes [dl_toString $obs_bytes bytes]
                set times  [dl_tcllist [dl_unpack [dl_choose $tcol [dl_ilist $o]]]]
                set counts [dl_tcllist [dl_unpack [dl_choose $ncol [dl_ilist $o]]]]

                set pos 0
                set sample_t {}
                set nchan 0
                array unset ch
                foreach t $times n $counts {
                    set block [string range $bytes $pos [expr {$pos + $n - 1}]]
                    incr pos $n
                    set d [extio::ain_decode $block]
                    set bn [dict get $d nchan]
                    if {$bn <= 0} { continue }
                    if {$nchan == 0} {
                        set nchan $bn
                        for {set c 0} {$c < $nchan} {incr c} { set ch($c) {} }
                    }
                    set interval [dict get $d interval_us]
                    set k 0
                    foreach row [extio::ain_scans $d] {
                        lappend sample_t [expr {$t + $k * $interval / 1000.0}]
                        for {set c 0} {$c < $nchan} {incr c} {
                            lappend ch($c) [lindex $row $c]
                        }
                        incr k
                    }
                }

                dl_local ot [dl_flist {*}$sample_t]
                dl_local ov [dl_llist]
                for {set c 0} {$c < $nchan} {incr c} {
                    dl_local cv [dl_create short]
                    if {[llength $ch($c)]} {
                        dl_fromString [binary format s* $ch($c)] $cv
                    }
                    dl_append $ov $cv
                }
                dl_append $t_out $ot
                dl_append $v_out $ov
            }

            dl_set $predg:ain_t $t_out
            dl_set $predg:ain_v $v_out
            return [list $predg:ain_t $predg:ain_v]
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
    # Conversion Functions - Three Levels
    # ========================================================================
    
    #
    # Level 0: Raw - direct .ess to .dgz conversion, minimal processing
    # Preserves the event stream exactly as recorded (diagnostic only)
    #
    # Arguments:
    #   filepath  - Full path to source .ess file
    #   outpath   - Full path for output .dgz file
    #
    # Returns: output path on success
    #
    proc convert_raw {filepath outpath} {
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
    # Level 1: Obs - observation-period oriented structure via readESS
    # Organizes data by obs periods (sync-line bounded epochs)
    # This is the foundation for trial extraction and sync verification
    #
    # Arguments:
    #   filepath  - Full path to source .ess file
    #   outpath   - Full path for output .obs.dgz file
    #
    # Returns: dict with status, path, n_obs, size
    #
    proc convert_obs {filepath outpath} {
        if {![file exists $filepath]} {
            error "Source file not found: $filepath"
        }
        
        # Read with obs-period structure
        set g [dslog::readESS $filepath]
        set n_obs [dl_length $g:e_types]
        
        # Save compressed
        dg_write $g $outpath
        dg_delete $g
        
        return [dict create \
            status ok \
            path $outpath \
            n_obs $n_obs \
            size [file size $outpath]]
    }
    
    #
    # Level 2: Trials - full extraction with system/protocol extractors
    # This is the "analysis-ready" format users want
    #
    # Arguments:
    #   source_path - Full path to source file (.ess or .obs.dgz)
    #   outpath     - Full path for output .trials.dgz file
    #   args        - Additional arguments passed to extractor
    #
    # Returns: dict with status, path, n_trials, size, extractor info
    #
    proc convert_trials {source_path outpath args} {
        if {![file exists $source_path]} {
            error "Source file not found: $source_path"
        }
        
        # Determine source type and load accordingly
        if {[string match "*.obs.dgz" $source_path] || [string match "*obs/*.dgz" $source_path]} {
            # Load from obs
            set g [load_data_from_obs $source_path {*}$args]
        } else {
            # Load from ess (legacy path)
            set g [load_data $source_path {*}$args]
        }
        
        # Count trials
        set cols [dg_tclListnames $g]
        set n_trials 0
        if {[llength $cols] > 0} {
            set n_trials [dl_length $g:[lindex $cols 0]]
        }
        
        # Save compressed
        dg_write $g $outpath
        dg_delete $g
        
        return [dict create \
            status ok \
            path $outpath \
            n_trials $n_trials \
            size [file size $outpath]]
    }
    
    #
    # Generate output filename for conversion
    # 
    # Arguments:
    #   filepath  - Source .ess filepath
    #   level     - raw | obs | trials
    #   outdir    - Output directory
    #
    # Returns: Full output path
    #
    proc convert_outpath {filepath level outdir} {
        set base [file rootname [file tail $filepath]]
        # Strip any existing .obs or .trials suffix
        regsub {\.(obs|trials)$} $base {} base
        
        switch $level {
            raw {
                return [file join $outdir "${base}.raw.dgz"]
            }
            obs {
                return [file join $outdir "${base}.obs.dgz"]
            }
            trials {
                return [file join $outdir "${base}.trials.dgz"]
            }
            default {
                error "Unknown conversion level: $level (use raw, obs, or trials)"
            }
        }
    }
    
    #
    # Convert a file at specified level (convenience wrapper)
    #
    # Arguments:
    #   filepath  - Source .ess filepath
    #   level     - raw | obs | trials
    #   outdir    - Output directory
    #
    # Returns: Output filepath or result dict (for obs/trials)
    #
    proc convert {filepath level outdir} {
        file mkdir $outdir
        
        set outpath [convert_outpath $filepath $level $outdir]
        
        switch $level {
            raw {
                return [convert_raw $filepath $outpath]
            }
            obs {
                return [convert_obs $filepath $outpath]
            }
            trials {
                return [convert_trials $filepath $outpath]
            }
            default {
                error "Unknown conversion level: $level"
            }
        }
    }
    
    # ========================================================================
    # Code Generation for External Tools
    # ========================================================================
    
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
