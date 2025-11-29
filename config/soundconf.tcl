puts "initializing sound"

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# enable error logging
errormon enable

# load sound module
set ess_modules "sound"
foreach f $ess_modules {
    load ${dspath}/modules/dserv_${f}[info sharedlibextension]
}

namespace eval sound {
    proc init_hardware {} {
	set ports "/dev/ttyUSB0 /dev/cu.usbserial-FTD1906W"
	foreach p $ports {
	    if [file exists $p] {
		soundOpen $p 
		soundReset
		break
	    }
	}
    }

    proc init_fluidsynth { { soundfont {} } { device {} } } {

	# use either default soundfont or allow user to specify
	if { $soundfont != "" && $soundfont != "default" } {
	    set sf $soundfont
	} else {
	    set paths "/usr/local/dserv/soundfonts /usr/share/sounds/sf2"
	    set sfile "default-GM.sf2"
	    foreach p $paths {
		set sf [file join $p $sfile]
		if { [file exists $sf] } {
		    break
		} else {
		    set sf ""
		}
	    }
	}

	if { $sf == "" } {
	    puts "sound font file \"$sf\" not found"
	    return
	}
	
	if { $device != "" } {
	    if { [catch {soundInitFluidSynth $sf $device}] } {
		puts "error loading fluidsynth on device $device"
		return
	    }
	} else {
	    if { [catch {soundInitFluidSynth $sf}] } {
		puts "error loading fluidsynth on default device"
	    }
	}
    }
}

# local system configuration in /usr/local/dserv/local/sound.tcl
if { [file exists $dspath/local/sound.tcl] } {
    source $dspath/local/sound.tcl
} else {
    sound::init_hardware
    sound::init_fluidsynth
}

