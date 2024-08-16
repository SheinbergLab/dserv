proc acknowledge_input { name val } {
    set output_pin 6
    if { $val } { set cmd DHI } { set cmd DLO }
    usbioSend "$cmd $output_pin"
}

proc setup {} {
    set input_pin 20
    dservAddMatch gpio/input/$input_pin
    dpointSetScript gpio/input/$input_pin acknowledge_input
    
    if { $::tcl_platform(os) == "Darwin" } {
	set usbio_name [glob /dev/cu.usbmodem*]
    } else {
	set usbio_name [glob /dev/ttyAC*]
    }
    usbioOpen $usbio_name
}

setup 
