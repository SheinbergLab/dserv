#
# control actual ess process from main Tcl interpreter (with guards)
#

# these provide information from ess about touch and eye regions
# should be in the ess namespace but they are not...
# clients (essgui, essgui-web, essqt) call these
# they just send a parameter to the underlying ain/touch module
proc touchGetRegionInfo { reg } {
    send ess touchGetRegionInfo $reg
}

proc ainGetRegionInfo { reg } {
    send ess ainGetRegionInfo $reg
}

namespace eval ess {
    # Helper procedures

    proc query_running_state {} {
        if {[dservExists ess/status]} {
            return [expr {[dservGet ess/status] eq "running"}]
        }
        return 0
    }
    
    proc query_open_file {} {
        if {[dservExists ess/datafile]} {
            set datafile [dservGet ess/datafile]
            return [expr {$datafile ne ""}]
        }
        return 0
    }

    
    proc _protected_call {cmd check_file args} {
        if {[query_running_state]} {
            error "Cannot call $cmd while running"
        }
        if {$check_file && [query_open_file]} {
            error "Cannot call $cmd while file is open"
        }
	set noreply_functions \
	    "load_system reload_system reload_protocol reload_variant"
	
	if { [lsearch -exact $noreply_functions $cmd] } {
	    return [send ess evalNoReply \{ ::ess::$cmd {*}$args \}]
	} else {
	    puts "send ess ::ess::$cmd {*}$args"
	    return [send ess ::ess::$cmd {*}$args]
	}
    }
    
    # Always allowed
    proc start {} { send ess ::ess::start }
    proc stop {} { send ess ::ess::stop }
    
    # Blocked when running + file open
    proc reset args { _protected_call reset 1 $args }
    proc set_subject args { _protected_call set_subject 1 $args }
    proc file_open args { _protected_call file_open 1 $args }
    proc load_system args { _protected_call load_system 1 $args }
    proc reload_system args { _protected_call reload_system 1 $args }
    proc reload_protocol args { _protected_call reload_protocol 1 $args }
    proc reload_variant args { _protected_call reload_variant 1 $args }
    proc save_settings args { _protected_call save_settings 1 $args }
    proc reset_settings args { _protected_call reset_settings 1 $args }
    proc set_param args { _protected_call set_param 1 $args }
    proc set_params args { _protected_call set_params 1 $args }
    proc set_variant_args args { _protected_call set_variant_args 1 $args }
    proc save_script args { _protected_call save_script 1 $args }
    proc get_lib_files args { _protected_call get_lib_files 1 $args }
    proc get_lib_file_content args { _protected_call get_lib_file_content 1 $args }
    proc validate_script_minimal s { _protected_call validate_script_minimal $s }
    
    # Blocked when running only
    proc file_close args { _protected_call file_close 0 $args }
    proc file_suggest {} { send ess ::ess::file_suggest }
}

