# stimconf.tcl
set stim_host ""
set stim_port 4612

proc stimSend {msg} {
    global stim_host stim_port
    
    if {$stim_host eq ""} {
        error "no host configured"
    }
    
    if {[catch {set sock [socket $stim_host $stim_port]}]} {
        error "cannot connect to stim2 at $stim_host:$stim_port"
    }

    fconfigure $sock -translation binary -buffering full
    
    set len [string length $msg]
    puts -nonewline $sock [binary format Iu $len]
    puts -nonewline $sock $msg
    flush $sock
    
    set lendata [read $sock 4]
    binary scan $lendata Iu rlen
    set result [read $sock $rlen]
    
    close $sock
    return $result
}

proc stimOpen {host {port 4612}} {
    global stim_host stim_port
    set stim_host $host
    set stim_port $port
}

# Auto-connect from env
if {[info exists ::env(ESS_RMT_HOST)]} {
    set stim_host $::env(ESS_RMT_HOST)
} else {
    set stim_host localhost
}

# Advertise proxy command
dservSet stim/proxy stimSend