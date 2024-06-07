#
# Procs for interacting with a datahub
#

lappend auto_path /usr/local/lib
#package require Tk
package require qpcs

proc process_data { args } {
    puts $args
}

proc connect_to_server { server ess } {
    if { [qpcs::dsRegister $server] != 1 } {
	error "Unable to register with $server"
    }
    puts [qpcs::dsAddCallback process_data]
    puts [qpcs::dsAddMatch $server /qpcs/rpio/*]
}

if { $argc > 0 } { set ::dh::datahub [lindex $argv 0] }
connect_to_server 100.0.0.40 qpcs:*
vwait forever

