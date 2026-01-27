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
