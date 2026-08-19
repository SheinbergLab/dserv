#
# Process visualization events sending output to designated streams
#
#  Defaults to graphics/stimulus

tcl::tm::add $dspath/lib

package require dlsh

# enable error logging
errormon enable

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

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
    # Named Event Helpers
    #########################################################################

    # Event lookup tables - populated from ess/evt_type_ids and
    # ess/evt_subtype_ids datapoints published by the ess module
    variable evt_type_ids [dict create]
    variable evt_subtype_ids [dict create]

    proc on_evt_type_ids {dpoint data} {
	variable evt_type_ids
	set evt_type_ids $data
    }

    proc on_evt_subtype_ids {dpoint data} {
	variable evt_subtype_ids
	set evt_subtype_ids $data
    }

    # evtSetScriptByName - register event handler using symbolic names
    #
    # Usage:
    #   evtSetScriptByName SAMPLE ON  [namespace current]::sample_on
    #   evtSetScriptByName ENDOBS *   [namespace current]::endobs
    #
    proc evtSetScriptByName {type_name subtype_name script} {
	variable evt_type_ids
	variable evt_subtype_ids

	if {![dict exists $evt_type_ids $type_name]} {
	    log error "evtSetScriptByName: unknown event type '$type_name'"
	    return
	}
	set type_id [dict get $evt_type_ids $type_name]

	if {$subtype_name eq "*" || $subtype_name eq "-1"} {
	    set subtype_id -1
	} else {
	    if {![dict exists $evt_subtype_ids $type_name $subtype_name]} {
		log error "evtSetScriptByName: unknown subtype '$subtype_name' for event '$type_name'"
		return
	    }
	    set subtype_id [dict get $evt_subtype_ids $type_name $subtype_name]
	}

	evtSetScript $type_id $subtype_id $script
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

	# Subscribe to event lookup tables
	dservAddExactMatch ess/evt_type_ids
	dservAddExactMatch ess/evt_subtype_ids
        
        # Set up handlers
        dpointSetScript ess/system [namespace current]::on_system_change
        dpointSetScript ess/protocol [namespace current]::on_protocol_change
        dpointSetScript ess/viz_config [namespace current]::on_viz_config_received
        dpointSetScript stimdg [namespace current]::on_stimdg_received
	dpointSetScript ess/evt_type_ids [namespace current]::on_evt_type_ids
	dpointSetScript ess/evt_subtype_ids [namespace current]::on_evt_subtype_ids

	# Subscribe to events
	dservAddExactMatch eventlog/events

	# Load event tables if already published
	if {[dservExists ess/evt_type_ids]} {
	    on_evt_type_ids ess/evt_type_ids [dservGet ess/evt_type_ids]
	}
	if {[dservExists ess/evt_subtype_ids]} {
	    on_evt_subtype_ids ess/evt_subtype_ids [dservGet ess/evt_subtype_ids]
	}
	
        # Initialize graphics
        clearwin
	setbackground 0
        setwindow -10 -10 10 10
        update_display

        log info "Visualization framework initialized - awaiting configuration"

	# Replay configuration published before this subprocess existed.
	#
	# dsconf starts ess -- and with it essconf.tcl's ess::load_system --
	# well before viz, so the first system's ess/viz_config and stimdg have
	# already come and gone by the time the handlers above are installed.
	# Nothing redelivers them, which is why the system dserv booted with had
	# no visualization until someone re-loaded it by hand.
	#
	# Deliberately NOT fixed by moving load_system later in dsconf: that
	# makes startup order load-bearing and only helps whoever happens to be
	# last. Catching up on current state at init is what the event tables
	# above already do, and what meshconf and trialsyncconf do.
	#
	# Replay order matches the order variant_init publishes in -- viz_config
	# first, since it installs the handlers that the stimdg arrival feeds --
	# and this sits AFTER the graphics init above so that clearwin/setwindow
	# cannot wipe what the config script just drew. Guarded because a bare
	# dservGet on an absent datapoint RAISES, and this runs synchronously at
	# boot where an uncaught error truncates the rest of dsconf.
	if {[dservExists ess/viz_config]} {
	    if {[catch {on_viz_config_received \
			    ess/viz_config [dservGet ess/viz_config]} err]} {
		log error "Error replaying ess/viz_config at init: $err"
	    }
	}
	if {[dservExists stimdg]} {
	    if {[catch {on_stimdg_received stimdg [dservGet stimdg]} err]} {
		log error "Error replaying stimdg at init: $err"
	    }
	}
    }
    
    #########################################################################
    # Configuration Reception
    #########################################################################
    
    # NOTE: teardown of stale viz bindings is NOT done here. ess/system and
    # ess/protocol are published indirectly (evt_put -> eventlog/events ->
    # triggers.tcl -> dservSet), so they arrive at this subprocess AFTER the
    # directly-set ess/viz_config. Tearing down here would therefore wipe the
    # handlers that on_viz_config_received just installed. Cleanup-then-install
    # is owned entirely by on_viz_config_received, which is safe because
    # variant_init publishes ess/viz_config (empty string for systems with no
    # viz config) on every load, before the new stimdg.
    proc on_system_change {dpoint data} {
        variable current_system
        set current_system $data
    }

    proc on_protocol_change {dpoint data} {
        variable current_protocol
        set current_protocol $data
    }
    
    proc on_viz_config_received {dpoint data} {
	variable current_system

	# clear out previous subscriptions and children
	cleanup_namespace

	# Resolve the target system authoritatively (synchronously) rather than
	# relying on the ess/system datapoint, which is published indirectly via
	# eventlog/triggers and may not have arrived yet when this fires. Without
	# this, the config could install into a stale ::viz::<old-system> ns.
	set current_system [getVar ess ::ess::current(system)]

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
    
    # ::viz::setup_window ?-zoom N?
    #
    # The window a viz should draw in, derived from the display rather than
    # hand-picked. Replaces the per-protocol "setwindow -13 -11 13 11",
    # which was wrong twice over: a target at 6 degrees landed at the wrong
    # fraction of the frame, and an aspect that did not match the canvas
    # drew circles as ellipses.
    #
    # Three things it does, in order:
    #
    #   extent   from ess/screen_halfx|halfy. This interp has no ess
    #            package but can read datapoints, which is why ess
    #            publishes them.
    #   zoom     how much of the display to show. 1.0 is a faithful scale
    #            model -- what the subject actually sees, which for a ring
    #            at 6 deg on a 27 deg half-width is a small ring in a lot of
    #            empty canvas. The default 2.0 fills the frame with the
    #            region experiments use, and lands close to the windows
    #            protocols had picked by hand.
    #   aspect   pad the SHORT dimension to the canvas aspect (getaspect).
    #            A window whose aspect differs from the viewport draws
    #            anisotropically, so markers come out elliptical. Padding
    #            keeps one scale in x and y and letterboxes instead.
    #
    # Zoom precedence, chosen so an operator control always works: the
    # ess/viz/zoom datapoint (live) beats -zoom (the protocol's preference)
    # beats the 2.0 default. A protocol stating -zoom is declaring what
    # suits it, not forbidding someone from looking closer.
    #
    # Safe to call repeatedly -- that is how a live zoom re-applies.
    proc setup_window { args } {
        set zoom 2.0
        foreach { k v } $args {
            switch -- $k {
                -zoom   { set zoom $v }
                default { error "::viz::setup_window: unknown option '$k'" }
            }
        }
        catch {
            set z [dservGet ess/viz/zoom]
            if { [string is double -strict $z] && $z > 0 } { set zoom $z }
        }

        set hx ""; set hy ""
        catch { set hx [dservGet ess/screen_halfx] }
        catch { set hy [dservGet ess/screen_halfy] }
        if { ![string is double -strict $hx] || $hx <= 0 ||
             ![string is double -strict $hy] || $hy <= 0 } {
            # SAY SO. A silently guessed window looks like a drawing bug --
            # markers at the wrong size and eccentricities in the wrong
            # place -- and sends you hunting in the wrong file.
            log warning "screen extents unavailable (ess/screen_halfx|halfy);\
                         using a fallback window -- sizes and positions will\
                         not match the display"
            set hx 13.0; set hy 11.0
        }

        set hx [expr {$hx/double($zoom)}]
        set hy [expr {$hy/double($zoom)}]

        set asp [expr {$hx/$hy}]
        catch { set a [getaspect]
                if { [string is double -strict $a] && $a > 0 } { set asp $a } }
        if { [expr {$hx/$hy}] > $asp } {
            set hy [expr {$hx/$asp}]
        } else {
            set hx [expr {$hy*$asp}]
        }

        setwindow [expr {-$hx}] [expr {-$hy}] $hx $hy
        return [list [expr {-$hx}] [expr {-$hy}] $hx $hy]
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
	proc evtSetScriptByName {type_name subtype_name script} {
	    ::viz::evtSetScriptByName $type_name $subtype_name $script
	}
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

