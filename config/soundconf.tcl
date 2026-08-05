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

    # HDMI audio cards whose sink actually accepts audio. A Pi exposes one
    # ALSA card per HDMI port whether or not anything is plugged into it,
    # and an idle port still has an ELD file -- just an empty one. sad_count
    # is the honest test: it counts the Short Audio Descriptors the sink
    # advertised, so >0 means "a display that told us it has speakers"
    # rather than "a port that exists".
    proc find_hdmi_audio {} {
        set cards {}
        if { [catch {open /proc/asound/cards} f] } { return $cards }
        set txt [read $f]; close $f
        foreach line [split $txt \n] {
            if { [regexp {^\s*(\d+)\s+\[(\S+)\s*\]:\s+(\S+)} $line -> num id drv] } {
                if { ![string match -nocase *hdmi* $drv] } { continue }
                set eld ""
                foreach ef [glob -nocomplain /proc/asound/card$num/eld*] {
                    catch {
                        set h [open $ef]
                        append eld [read $h]
                        close $h
                    }
                }
                if { [regexp -line {^sad_count\s+([1-9]\d*)} $eld] } {
                    lappend cards $id
                }
            }
        }
        return $cards
    }

    # Resolve this box's audio output, most-preferred first:
    #   1. a USB audio card -- when a dongle is present it IS the rig output
    #   2. an HDMI card with a sink attached -- boxes whose only speakers
    #      live in an HDMI-connected touchscreen
    # Returns an ALSA device string, or "" if the box has no usable output.
    #
    # Deliberately never returns ALSA's "default". On a Pi with no sound
    # hardware "default" resolves to the HDMI port driving the stimulus
    # display, so a rig that was never meant to have audio ends up playing
    # full-scale wav stimuli out of the subject's monitor -- silently, since
    # the device then reports itself only as "Default Playback Device".
    # Falling back to HDMI is fine; doing it without saying so is not.
    # Pass "default" explicitly if ALSA's default is genuinely what you want.
    proc resolve_device { {usbid *} } {
        # Non-Linux (macOS dev boxes): no /proc/asound to inspect, and no
        # HDMI-grabs-the-stimulus-display hazard either, so the system
        # default is both correct and safe. Say "default" explicitly rather
        # than "" so the caller still gets an audio device.
        if { ![file isdirectory /proc/asound] } { return default }

        set ids [find_usb_audio $usbid]
        if { [llength $ids] > 1 } {
            puts "sound: multiple USB audio cards ($ids), using [lindex $ids 0]"
        }
        if { [llength $ids] } {
            set dev plughw:CARD=[lindex $ids 0],DEV=0
            puts "sound: auto-selected USB audio device $dev"
            return $dev
        }
        set ids [find_hdmi_audio]
        if { [llength $ids] } {
            set dev plughw:CARD=[lindex $ids 0],DEV=0
            puts "sound: no USB audio card; falling back to HDMI audio $dev"
            puts "sound: NOTE audio will play through the attached display"
            return $dev
        }
        puts "sound: no usable audio output (no USB card, no HDMI sink)"
        return ""
    }

    # Software synth + wav output with automatic device selection: prefer
    # the (sole) USB audio card, then an HDMI display that reports speakers.
    # A rig's local/sound.tcl can stay model-agnostic:
    #     sound::init_software
    # and swapping dongle brands never needs a config edit. Pass an
    # explicit device string to override, or a usbid glob to pin hardware.
    proc init_software { {soundfont default} {device auto} {usbid *} } {
        if { $device eq "auto" } { set device [resolve_device $usbid] }
        if { $device eq "" } {
            puts "sound: software synth not started (no audio output)"
            return
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
	
	# An empty device used to mean ALSA "default", which on a box with no
	# sound hardware is the HDMI port driving the stimulus display. Resolve
	# a real output instead; pass "default" to force ALSA's default.
	if { $device eq "" } { set device [resolve_device] }
	if { $device eq "" } {
	    puts "sound: fluidsynth not started (no audio output)"
	    return
	}
	if { [catch {soundInitFluidSynth $sf $device} err] } {
	    puts "sound: error loading fluidsynth on device $device: $err"
	    return
	}
    }
}

# local system configuration in /usr/local/dserv/local/sound.tcl
if { [file exists $dspath/local/sound.tcl] } {
    source $dspath/local/sound.tcl
} else {
    sound::init_hardware
    # auto-select a USB sound card over the system default: on Pis the
    # default lands on a vc4-hdmi (a DISPLAY), and on HDMI-only x86 boxes
    # it points at a device that does not exist (officepi/office-stim,
    # 2026-08-05). A rig with neither gets the old default behaviour.
    sound::init_software
}

puts "Sound initialized"

