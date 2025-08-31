#
# Process visualization events sending output to designated streams
#
#  Defaults to graphics/stimulus

set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

tcl::tm::add $dspath/lib

package require dlsh

namespace eval viz {
    # Current configuration state
    variable current_system ""
    variable current_protocol ""
    variable graphics_output "graphics/stimulus"
    variable event_subscriptions [list]
    
    # Trial state
    variable current_trial_id -1
    variable stimulus_visible 0
    variable response_made [dict create]
    variable trial_result -1

    proc log { level message {category "visualization"}} {
	variable current

	
	# Pre-format timestamp once
	set timestamp [clock format [clock seconds] -format "%H:%M:%S"]
	set formatted_msg "\[$timestamp\] \[$category\] $message"

	switch -- $level {
	    error {
		dservSet ess/errorInfo $formatted_msg
	    }
	    warning {
		dservSet ess/warningInfo $formatted_msg  
	    }
	    info {
		dservSet ess/infoLog $formatted_msg
	    }
	    debug {
		dservSet ess/debugLog $formatted_msg
	    }
	    default {
		dservSet ess/generalLog $formatted_msg
	    }
	}
    }
    
    #########################################################################
    # Framework Initialization
    #########################################################################
    
    proc init {} {
        # Subscribe to system configuration changes
        dservAddExactMatch ess/system
        dservAddExactMatch ess/protocol
        dservAddExactMatch ess/variant
        
        # Subscribe to visualization configuration
        dservAddExactMatch ess/viz_config
        
        # Subscribe to stimdg (main data source)
        dservAddExactMatch stimdg
        
        # Set up handlers
        dpointSetScript ess/system [namespace current]::on_system_change
        dpointSetScript ess/protocol [namespace current]::on_protocol_change
        dpointSetScript ess/viz_config [namespace current]::on_viz_config_received
        dpointSetScript stimdg [namespace current]::on_stimdg_received

	# Subscribe to events
	dservAddExactMatch eventlog/events
	
        # Initialize graphics
        clearwin
        setwindow -10 -10 10 10  ;# Default viewport
        update_display
        
        log info "Visualization framework initialized - awaiting configuration"
    }
    
    #########################################################################
    # Configuration Reception
    #########################################################################
    
    proc on_system_change {dpoint data} {
        variable current_system
        if {$current_system ne $data} {
            set current_system $data
            cleanup_namespace
            log info "System changed to: $data - awaiting visualization config"
        }
    }
    
    proc on_protocol_change {dpoint data} {
        variable current_protocol  
        if {$current_protocol ne $data} {
            set current_protocol $data
            cleanup_namespace
            log info "Protocol changed to: $data - awaiting visualization config"
        }
    }
    
    proc on_viz_config_received {dpoint data} {
        # Configuration comes as Tcl script defining how to visualize
        log info "Received visualization configuration"

	# add path to find system modules that may be required
	set syspath [getVar ess ::ess::system_path]
	set project [getVar ess ::ess::current(project)]
	::tcl::tm::add [file join $syspath $project lib]
        
        if {[catch {eval $data} error]} {
            log error "Error setting up visualization config: $error"
        } else {
            log info "Visualization configuration applied successfully"
        }
    }
    
    proc on_stimdg_received {dpoint data} {
        # Reconstruct stimdg from data
        if {[catch {
            dg_fromString $data
            log info "stimdg updated - ready for trial visualization"
        } error]} {
            log error "Error processing stimdg: $error"
        }
    }
    
    #########################################################################
    # Event Subscription Management
    #########################################################################
    
    proc subscribe_to_event {event_id callback} {
        variable event_subscriptions
        
        # Set up event listener
        evtSetScript $event_id -1 $callback
        
        # Track subscription for cleanup
        lappend event_subscriptions [list $event_id $callback]
        
        log info "Subscribed to event $event_id"
    }
    
    proc clear_event_subscriptions {} {
        variable event_subscriptions
	evtRemoveAllScripts
        set event_subscriptions [list]
        log info "Cleared all event subscriptions"
    }
    
    #########################################################################
    # Namespace Cleanup
    #########################################################################
    
    proc cleanup_namespace {} {
        # Clear event subscriptions
        clear_event_subscriptions
        
        # Reset trial state
        variable current_trial_id -1
        variable stimulus_visible 0
        variable response_made [dict create]
        variable trial_result -1
        
        # Clear display
        clear_display
        
        # Remove system-specific procedures
        cleanup_system_procedures
        
        log info "Visualization namespace cleaned"
    }
    
    proc cleanup_system_procedures {} {
        # Delete entire system namespace if it exists
        variable current_system
        if {$current_system ne ""} {
            set system_ns "::viz::${current_system}"
            if {[namespace exists $system_ns]} {
                namespace delete $system_ns
                log debug "Deleted system namespace: $system_ns"
            }
        }
        
        # Clean up any system-specific variables in main viz namespace
        foreach var [info vars ::viz::system_*] {
            if {[info exists $var]} {
                unset $var
            }
        }
    }
    
    #########################################################################
    # Graphics Management
    #########################################################################
    
    proc set_viewport {x1 y1 x2 y2} {
        clearwin
        setwindow $x1 $y1 $x2 $y2
        update_display
    }
    
    proc clear_display {} {
        clearwin
        update_display
    }
    
    proc update_display {} {
        variable graphics_output
        dservSet $graphics_output [dumpwin json]
    }
    
    #########################################################################
    # Trial State Management
    #########################################################################
    
    proc set_trial {trial_id} {
        variable current_trial_id
        variable stimulus_visible
        variable response_made
        variable trial_result
        
        set current_trial_id $trial_id
        set stimulus_visible 0
        set response_made [dict create]
        set trial_result -1
    }

    proc trial_id {} {
	variable current_trial_id
	return $current_trial_id
    }
    
    proc show_stimulus {} {
        variable stimulus_visible
        set stimulus_visible 1
    }
    
    proc hide_stimulus {} {
        variable stimulus_visible
        set stimulus_visible 0
    }
    
    proc set_response {response_type} {
        variable response_made
        dict set response_made type $response_type
        dict set response_made time [clock seconds]
    }
    
    proc set_trial_result {result} {
        variable trial_result
        set trial_result $result
    }
    
    #########################################################################
    # Utility Functions
    #########################################################################
    
    proc get_trial_info {trial_id} {
        # Helper to extract trial data from stimdg
        if {![dg_exists stimdg]} {
            return [dict create]
        }
        
        set trial_data [dict create]
        set lists [dg_tclListnames stimdg]
        
        foreach list_name $lists {
	    dict set trial_data $list_name [dl_tcllist stimdg:$list_name:$trial_id]
        }
        
        return $trial_data
    }

    proc get_attr { trial_id attr } {
	if {![dg_exists stimdg]} {
            return
	}
	if { [lsearch [dg_tclListnames stimdg] $attr] == -1 } {
	    return
	}
	return [dl_tcllist stimdg:$attr:$trial_id]
    }
    
    proc response_color {response_type} {
        switch $response_type {
            1 { return "0.5 0.5 0.9" }  ;# Left
            2 { return "0.9 0.5 0.5" }  ;# Right
            default { return "0.7 0.7 0.7" }
        }
    }
    
    proc correctness_color {correct} {
        switch $correct {
            1 { return "0.2 0.8 0.2" }  ;# Correct
            0 { return "0.8 0.2 0.2" }  ;# Incorrect
            2 { return "0.8 0.6 0.2" }  ;# Aborted
            default { return "0.7 0.7 0.7" }
        }
    }
}

# Initialize the visualization framework
viz::init


### Original - to be removed
proc get_stimdg { name data } {
	dg_fromString $data
}

proc init {} {
	dservAddExactMatch eventlog/events
	dservAddExactMatch stimdg
	dservSetScript stimdg get_stimdg
	evtSetScript 19 -1 beginobs
	evtSetScript 20 -1 endobs
}

proc beginobs { type subtype data }	{
	clearwin
	setwindow 0 0 1 1
	dlg_text 0.5 0.5 "Beginobs $data" -size 24 -just 0
	dservSet graphics/stimulus [dumpwin json]
}

proc endobs { type subtype data }	{
	clearwin
	setwindow 0 0 1 1
	dlg_text 0.5 0.5 "Endobs $subtype" -size 24 -just 0
	dservSet graphics/stimulus [dumpwin json]
}
