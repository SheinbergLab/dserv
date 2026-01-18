package provide df 2.0

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
                type [dl_type $g:$col] \
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
        if {[dg_exists $g:e_types]} {
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
        
        # Open file
        set f [df::File new $filepath]
        
        # Run system extractor (required)
        set sys_proc "${system}::extract_trials"
        if {[info commands $sys_proc] eq ""} {
            $f destroy
            error "No extractor found: $sys_proc (looked in $sys_extract)"
        }
        set result [{*}$sys_proc $f {*}$args]
        
        # Run protocol extractor if exists (optional second pass)
        if {$protocol ne ""} {
            set proto_proc "${system}::${protocol}::extract_trials"
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
    oo::class create File {
        variable filepath g predg meta
        variable type_names type_ids subtypes
        
        constructor {path} {
            set filepath $path
            
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
            set subtype_dicts [dl_tcllist [dl_select $predg:data $subtype_evts]]
            
            foreach type_id $type_ids_list subtype_dict $subtype_dicts {
                # payload is {subtype_id subtype_name ...}
                # reverse to {subtype_name subtype_id}
                set reversed [dict create]
                dict for {sub_id sub_name} $subtype_dict {
                    dict set reversed $sub_name $sub_id
                }
                dict set subtypes $type_id $reversed
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
            
            # TIME/OPEN - file timestamp
            lassign [my evt TIME OPEN] t s
            set ts [my FindPreEvent $t $s]
            if {$ts ne ""} {
                dict set meta timestamp $ts
                dict set meta date [clock format $ts -format "%Y-%m-%d"]
                dict set meta time [clock format $ts -format "%H:%M:%S"]
            }
            
            # ID/VARIANT - system:protocol:variant
            lassign [my evt ID VARIANT] t s
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
            
            # ID/SUBJECT
            lassign [my evt ID SUBJECT] t s
            set subj [my FindPreEvent $t $s]
            if {$subj ne ""} {
                dict set meta subject $subj
            }
            
            # Obs period count
            if {[dg_exists $g:e_types]} {
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
            } else {
                set t [dict get $type_ids $type_name]
            }
            
            if {$subtype_name eq ""} {
                return $t
            }
            
            if {[string is integer -strict $subtype_name]} {
                set s $subtype_name
            } else {
                set s [dict get $subtypes $t $subtype_name]
            }
            
            return [list $t $s]
        }
        
        method select_evt {type_name subtype_name} {
            lassign [my evt $type_name $subtype_name] t s
            dl_return [dl_and [dl_eq $g:e_types $t] [dl_eq $g:e_subtypes $s]]
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
