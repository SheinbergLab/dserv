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


    # ------------------------------------------------------------------
    # Output discovery.
    #
    # Everything below enumerates playback PCMs, NOT cards, because a card
    # is not an output. An Intel HDA card carries analog on device 0 and
    # HDMI on devices 3/7/8/9, and office-stim (NUC14) has no device 0 at
    # all -- its only playback PCMs are four HDMI ones. Any code that
    # assumes DEV=0 hands back a device that does not exist on that entire
    # class of machine, which is how that box ended up needing a dongle.
    #
    # $root exists so a captured /proc/asound from another box can be
    # replayed as a fixture; it is always /proc/asound in production.
    # ------------------------------------------------------------------

    # Every playback PCM on the box, as a list of dicts:
    #   card <n>  id <cardid>  dev <n>  drv <driver>  name <pcm name>
    proc list_playback_pcms { {root /proc/asound} } {
        set pcms {}
        if { [catch {open $root/cards} f] } { return $pcms }
        set txt [read $f]; close $f
        array set cid {}
        array set cdrv {}
        foreach line [split $txt \n] {
            if { [regexp {^\s*(\d+)\s+\[(\S+)\s*\]:\s+(\S+)} $line -> num id drv] } {
                set cid($num) $id
                set cdrv($num) $drv
            }
        }
        foreach num [lsort -integer [array names cid]] {
            foreach p [lsort [glob -nocomplain $root/card$num/pcm*p]] {
                if { [catch {open $p/info} h] } { continue }
                set itxt [read $h]; close $h
                if { ![regexp -line {^device:\s*(\d+)} $itxt -> dev] } { continue }
                set pname ""
                regexp -line {^name:\s*(.*)$} $itxt -> pname
                lappend pcms [list card $num id $cid($num) dev $dev \
                                  drv $cdrv($num) name [string trim $pname]]
            }
        }
        return $pcms
    }

    # What KIND of output a PCM is. The card's DRIVER decides, never the
    # product name -- cheap dongles all call themselves "Device" or "CODEC",
    # and every DAC HAT brings its own driver string (Allo_Katana,
    # snd_rpi_hifiberry_dacplus, ...) so there is nothing common to match.
    # Anything left over after the known classes IS an I2S codec.
    #
    # TRAP: do not classify by PCM name. The Pi's vc4-hdmi PCM is called
    # "MAI PCM i2s-hifi-0" -- the string "hdmi" appears nowhere in it. PCM
    # names are only meaningful INSIDE a multi-PCM HDA card, where they are
    # literally "HDMI 0".."HDMI 3" and are the only way to tell those apart
    # from the analog jack sharing the same card.
    proc classify_pcm { p } {
        set drv [dict get $p drv]
        set name [dict get $p name]
        if { [string match USB-Audio* $drv] } { return usb }
        if { [string match -nocase *hdmi* $drv] } { return hdmi }
        if { [string match -nocase dummy* $drv] } { return virtual }
        if { [string match -nocase loopback* $drv] } { return virtual }
        if { [string match -nocase bcm2835* $drv] } { return onboard }
        if { [string match -nocase hda-* $drv] } {
            if { [string match -nocase *hdmi* $name] } { return hdmi }
            if { [string match -nocase *displayport* $name] } { return hdmi }
            return analog
        }
        return i2s
    }

    # Does this HDMI PCM have a sink that claims speakers? sad_count counts
    # the Short Audio Descriptors the display advertised, so >0 means "a
    # display that told us it has speakers" rather than "a port that
    # exists" -- an idle port still has an ELD file, just an empty one.
    #
    # Two ELD layouts in the fleet:
    #   Pi vc4-hdmi : one card per port, a single "eld#0"
    #   Intel HDA   : one card, "eld#<codec>.<k>" where k is the index in
    #                 the PCM's own name, so "HDMI 0" -> eld#2.0. On
    #                 office-stim the live CR240DM is eld#2.0 while 35
    #                 sibling ELDs sit empty, so this per-PCM correlation is
    #                 the only thing keeping us off the dead ports.
    proc pcm_has_sink { p {root /proc/asound} } {
        set num [dict get $p card]
        set files {}
        if { [regexp -nocase {hdmi\s*(\d+)} [dict get $p name] -> k] } {
            set files [glob -nocomplain $root/card$num/eld#*.$k]
        }
        if { ![llength $files] } {
            set files [glob -nocomplain $root/card$num/eld*]
        }
        foreach ef $files {
            if { [catch {open $ef} h] } { continue }
            set t [read $h]; close $h
            if { [regexp -line {^sad_count\s+([1-9]\d*)} $t] } { return 1 }
        }
        return 0
    }

    # Usable outputs of one class, best first. Optional usbid ("0d8c:0102",
    # glob ok) pins exact hardware the same way juicerconf pins its pump.
    proc find_outputs { class {root /proc/asound} {usbid *} } {
        set out {}
        foreach p [list_playback_pcms $root] {
            set c [classify_pcm $p]
            # An explicit request for "analog" also accepts the Pi's own
            # 3.5mm jack; auto never reaches for it (see auto_rank).
            if { $class eq "analog" && $c eq "onboard" } { set c analog }
            if { $c ne $class } { continue }
            if { $c eq "usb" && $usbid ne "*" } {
                set uid ""
                catch {
                    set uf [open $root/card[dict get $p card]/usbid]
                    set uid [string trim [read $uf]]
                    close $uf
                }
                if { ![string match $usbid $uid] } { continue }
            }
            if { $c eq "hdmi" && ![pcm_has_sink $p $root] } { continue }
            lappend out [list \
                dev plughw:CARD=[dict get $p id],DEV=[dict get $p dev] \
                hw hw:CARD=[dict get $p id],DEV=[dict get $p dev] \
                cardid [dict get $p id] class $c \
                desc "[dict get $p id] dev [dict get $p dev] ([dict get $p name])"]
        }
        return $out
    }

    # ------------------------------------------------------------------
    # Shared (software-mixed) access, for rigs where dserv AND stim2 both
    # need audio.
    #
    # The hardware PCM is an EXCLUSIVE open and dserv starts its device at
    # init and never releases it, so stim2 can never acquire the card --
    # proven on the rig, a second opener gets EBUSY. dmix puts a shared ring
    # buffer in front so both play. Set in local/sound.tcl:
    #     set sound::output_shared 1
    #
    # Deliberately dmix and not PipeWire: PipeWire is a USER-SESSION daemon
    # and dserv is a root system service, which needs a runtime-dir override
    # plus enable-linger and still fails SILENTLY (audioInfo says running=1
    # while producing nothing) if the session is not up. dmix is pure ALSA
    # config with no daemon and no session. Use PipeWire only on dev boxes
    # that also want desktop audio.
    # ------------------------------------------------------------------
    variable output_shared 0

    # Formats dmix can actually mix, best first. dmix sums linear PCM
    # samples, so this list is not cosmetic: a Pi's vc4-hdmi offers ONLY
    # IEC958_SUBFRAME_LE (S/PDIF framing, not summable) and dmix rejects it
    # outright with "Unsupported format". That is a real fleet constraint --
    # an in-cage box whose output is HDMI CANNOT share via dmix.
    variable dmix_formats {S32_LE S24_3LE S24_LE S16_LE}

    # Which formats a card accepts. There is no /proc source for this, so we
    # ask aplay; a box without alsa-utils simply cannot use shared access.
    proc card_formats { hwdev } {
        # aplay ALWAYS exits non-zero here -- /dev/zero is not a wav, and
        # dumping params is all we want. Tcl folds the captured output into
        # the error message, so $out carries it either way.
        catch {exec aplay --dump-hw-params -D $hwdev /dev/zero 2>@1} out
        if { [regexp -line {^FORMAT:\s*(.*)$} $out -> line] } {
            return [split [string trim $line]]
        }
        return {}
    }

    # Generate the shared PCM definition and return its device string, or ""
    # if this box cannot do it. Written to /etc/alsa/conf.d (globbed by
    # alsa.conf) rather than /etc/asound.conf, so a hand-written asound.conf
    # is never clobbered. Idempotent: only rewritten when the content
    # changes, so it is not churned on every boot.
    proc ensure_shared_pcm { hwdev } {
        variable dmix_formats
        set formats [card_formats $hwdev]
        if { ![llength $formats] } {
            # Either alsa-utils is missing, or something already holds the
            # card -- the probe has to open it. In normal startup the device
            # is still free here, because the module opens it after us.
            puts "sound: cannot probe formats for $hwdev (alsa-utils missing,\
                  or the card is already in use)"
            return ""
        }
        set fmt ""
        foreach f $dmix_formats {
            if { [lsearch -exact $formats $f] >= 0 } { set fmt $f; break }
        }
        if { $fmt eq "" } {
            puts "sound: $hwdev offers no dmix-compatible format (has: $formats)"
            puts "sound: this output CANNOT be shared -- Pi HDMI is IEC958-only"
            return ""
        }
        set conf "# GENERATED by dserv soundconf.tcl -- edits are overwritten.
# Shared access so dserv and stim2 can both play: the raw hw PCM is an
# exclusive open and dserv never releases it. Rate is pinned to 48000 to
# match the sound module (AO_SAMPLE_RATE) so nothing resamples.
pcm.dserv_dmix {
    type dmix
    ipc_key 2748
    ipc_perm 0666
    slave {
        pcm \"$hwdev\"
        format $fmt
        rate 48000
        channels 2
        period_size 256
        buffer_size 1024
    }
}
# Typeable alias for diagnostics: aplay -D dserv_shared ...
pcm.dserv_shared { type plug slave.pcm \"dserv_dmix\" }
"
        set path /etc/alsa/conf.d/60-dserv-shared.conf
        set cur ""
        catch { set h [open $path]; set cur [read $h]; close $h }
        if { $cur ne $conf } {
            if { [catch {
                file mkdir /etc/alsa/conf.d
                set h [open $path w]
                puts -nonewline $h $conf
                close $h
            } err] } {
                puts "sound: could not write $path: $err"
                return ""
            }
            puts "sound: wrote $path (dmix on $hwdev, format $fmt)"
        }
        # plug: does the conversion miniaudio needs, and the ':' is what makes
        # the module treat this as a literal ALSA name rather than a search.
        return plug:dserv_dmix
    }

    # Ranking used when the rig has NOT declared what it has, ordered by how
    # deliberate the hardware is: a dongle or a DAC HAT was bolted on for
    # this purpose, a display is whatever happened to be plugged in.
    #
    # onboard/virtual are deliberately absent -- promoting a Pi's own 3.5mm
    # jack or an snd-aloop device over HDMI would silently move audio on
    # rigs that never had either.
    variable auto_rank {usb i2s analog hdmi}

    # Declare what the rig actually has, in local/sound.tcl, BEFORE calling
    # init_software:
    #     set sound::output_class hdmi   ;# in-cage box: the display IS the plan
    # Valid: auto usb i2s analog hdmi none.
    #
    # This exists because two choices are simply not in the hardware. HDMI
    # means opposite things -- on a stim rig it is sound leaking into the
    # subject's monitor, on an in-cage box it is the entire design -- and on
    # a box with both a headphone jack and HDMI on one card, which one is
    # "the output" is a fact about how the rig was wired. A declared class
    # is also an ASSERTION: if it is not there we say so, instead of quietly
    # landing somewhere else.
    variable output_class auto

    # Publish what was picked and why. sound/audio/device (set by the module)
    # carries miniaudio's generic "Playback Device", which cannot tell a DAC
    # from the subject's monitor -- that is precisely what hid the
    # audio-out-the-stimulus-display bug. These say what was ASKED for and
    # which tier chose it, so a health check can assert intent.
    # Set by resolve_device so the publish below can say WHICH tier won even
    # when the device travels through init_software/init_fluidsynth first.
    # Stays "explicit" for a rig that hands over a device string by hand.
    variable resolved_class explicit

    proc publish_choice { dev class } {
        variable resolved_class
        set resolved_class $class
        catch { dservSet sound/audio/requested $dev }
        catch { dservSet sound/audio/class $class }
    }

    # Resolve this box's audio output. Returns an ALSA device string, or ""
    # if there is no usable output.
    #
    # Deliberately never returns ALSA's "default". On a Pi with no sound
    # hardware "default" resolves to the HDMI port driving the stimulus
    # display, so a rig that was never meant to have audio ends up playing
    # full-scale wav stimuli out of the subject's monitor -- silently.
    # Falling back to HDMI is fine; doing it without saying so is not.
    # Pass "default" explicitly if ALSA's default is genuinely what you want.
    proc resolve_device { {usbid *} {root /proc/asound} } {
        variable auto_rank
        variable output_class
        variable output_shared
        set shared $output_shared

        # Non-Linux (macOS dev boxes): no /proc/asound to inspect, and no
        # HDMI-grabs-the-stimulus-display hazard either, so the system
        # default is both correct and safe -- CoreAudio mixes, so sharing it
        # with stim2 costs nothing. Say "default" explicitly rather than ""
        # so the caller still gets an audio device.
        if { ![file isdirectory $root] } {
            publish_choice default coreaudio
            return default
        }

        if { $output_class eq "none" } {
            puts "sound: output class declared \"none\" -- audio disabled on this box"
            publish_choice "" none
            return ""
        }

        set declared [expr {$output_class ne "auto"}]
        set classes [expr {$declared ? [list $output_class] : $auto_rank}]

        foreach class $classes {
            set found [find_outputs $class $root $usbid]
            if { ![llength $found] } { continue }
            set sel [lindex $found 0]
            set dev [dict get $sel dev]
            if { [llength $found] > 1 } {
                puts "sound: multiple $class outputs, using [dict get $sel desc]"
            }
            if { $declared } {
                puts "sound: $class output (declared) $dev"
            } elseif { $class eq "hdmi" } {
                puts "sound: no USB/I2S/analog output; falling back to HDMI $dev"
                puts "sound: NOTE audio will play through the attached display"
            } else {
                puts "sound: auto-selected $class output $dev"
            }
            if { !$declared && $class eq "analog" } {
                puts "sound: NOTE analog outranked any HDMI sink here; if this box\
                      should use its display, set sound::output_class hdmi"
            }
            if { $class eq "usb" } {
                # DO NOT auto-normalize the card's hardware mixer to 0 dB.
                # Tried 2026-08-05 and REVERTED the same evening: full-scale
                # playback at 0 dB through a C-Media dongle spiked USB VBUS
                # current and HARD-CRASHED a Pi 5 -- twice, instantly, at the
                # loudest sample (vcgencmd undervoltage flags; the shipped 56%
                # (-20 dB) default was accidentally load-bearing brownout
                # protection). The mixer level is a PER-RIG electrical
                # decision (PSU headroom, powered vs passive speakers): set it
                # deliberately with amixer + alsactl store, never blindly.
                # Unmute alone is safe -- a shipped-muted card is just broken.
                foreach ctl {Speaker PCM Headphone} {
                    catch { exec amixer -c [dict get $sel cardid] sset $ctl unmute }
                }
            }
            # Nothing touches an I2S/analog/HDMI mixer: a DAC HAT ships with a
            # considered default (the Katana boots at -20 dB) and the gain is a
            # per-rig electrical decision, exactly as for the USB branch.

            # Shared access, if this rig asked for it. Degrading to exclusive
            # is announced, never silent: the consequence is that stim2 gets
            # no audio at all, which is exactly the kind of thing that used to
            # be discovered by ear weeks later.
            if { $shared } {
                set sdev [ensure_shared_pcm [dict get $sel hw]]
                if { $sdev ne "" } {
                    puts "sound: shared output $sdev (dserv + stim2 can both play)"
                    publish_choice $sdev $class
                    return $sdev
                }
                puts "sound: FALLING BACK to exclusive $dev -- stim2 will NOT\
                      be able to open this card while dserv holds it"
            }
            publish_choice $dev $class
            return $dev
        }

        if { $declared } {
            puts "sound: DECLARED output class \"$output_class\" NOT FOUND -- no audio"
            puts "sound: (a sleeping display advertises no sink; wake it and restart)"
        } else {
            puts "sound: no usable audio output (no USB, I2S, analog or HDMI sink)"
        }
        publish_choice "" none
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
	    catch { dservSet sound/audio/class failed }
	    return
	}

	# Publish the device that was ACTUALLY opened, not just the one
	# auto-detection picked. A rig whose local/sound.tcl hands over an
	# explicit string never calls resolve_device, and those are exactly the
	# hand-configured boxes where knowing the real output matters most.
	variable resolved_class
	catch { dservSet sound/audio/requested $device }
	catch { dservSet sound/audio/class $resolved_class }
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

