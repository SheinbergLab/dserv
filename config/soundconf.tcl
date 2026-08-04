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

    # USB audio cards by ALSA card id, in card order. Identity comes from
    # the DRIVER CLASS (USB-Audio), never the product name -- cheap
    # class-compliant dongles all call themselves generic things like
    # "Device" or "CODEC", but on a rig the only USB-Audio card IS the rig
    # output (the onboard codec enumerates as HDA-Intel/bcm2835/etc).
    # Optional usbid ("0d8c:0102", glob ok) pins exact hardware the same
    # way juicerconf pins its pump.
    proc find_usb_audio { {usbid *} } {
        set cards {}
        if { [catch {open /proc/asound/cards} f] } { return $cards }
        set txt [read $f]; close $f
        foreach line [split $txt \n] {
            if { [regexp {^\s*(\d+)\s+\[(\S+)\s*\]:\s+(\S+)} $line -> num id drv] } {
                if { ![string match USB-Audio* $drv] } { continue }
                set uid ""
                catch {
                    set uf [open /proc/asound/card$num/usbid]
                    set uid [string trim [read $uf]]
                    close $uf
                }
                if { [string match $usbid $uid] } { lappend cards $id }
            }
        }
        return $cards
    }

    # Software synth + wav output with automatic device selection: prefer
    # the (sole) USB audio card; fall back to the system default device.
    # A rig's local/sound.tcl can stay model-agnostic:
    #     sound::init_software
    # and swapping dongle brands never needs a config edit. Pass an
    # explicit device string to override, or a usbid glob to pin hardware.
    proc init_software { {soundfont default} {device auto} {usbid *} } {
        if { $device eq "auto" } {
            set ids [find_usb_audio $usbid]
            if { [llength $ids] > 1 } {
                puts "sound: multiple USB audio cards ($ids), using [lindex $ids 0]"
            }
            if { [llength $ids] } {
                set device plughw:CARD=[lindex $ids 0],DEV=0
                puts "sound: auto-selected USB audio device $device"
            } else {
                set device ""
                puts "sound: no USB audio card found, using default device"
            }
        }
        init_fluidsynth $soundfont $device
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

puts "Sound initialized"

