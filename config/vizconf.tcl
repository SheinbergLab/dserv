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
	setbackground 0
        setwindow -10 -10 10 10
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
        }
    }
    
    proc on_protocol_change {dpoint data} {
        variable current_protocol  
        if {$current_protocol ne $data} {
            set current_protocol $data
        }
    }
    
    proc on_viz_config_received {dpoint data} {
	variable current_system
	
	# clear out previous subscriptions and children
	cleanup_namespace

	# add path to find system modules that may be required
	set syspath [getVar ess ::ess::system_path]
	set project [getVar ess ::ess::current(project)]
	::tcl::tm::add [file join $syspath $project lib]

	# now evaluate the configuration script in the ::viz::${system} ns
        if {[catch {namespace eval ::viz::$current_system $data} error]} {
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
        evtSetScript $event_id -1 $callback
    }
    
    proc cleanup_namespace {} {
	evtRemoveAllScripts
        
        # reset trial state
        variable current_trial_id -1
        variable stimulus_visible 0
        variable response_made [dict create]
        variable trial_result -1
        
        # clear display
        clear_display

	# clean up all children of parent ::viz
	foreach child [namespace children ::viz] {
	    namespace delete $child
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

    # scripts can just call flushwin
    namespace inscope :: {
	proc flushwin {} { ::viz::update_display }
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
}

# Initialize the visualization framework
viz::init

