#
# ain subprocess - owns the MCP320x analog input acquisition
#
# Isolates the analog-in driver from the ess subprocess so ess doesn't
# carry hardware startup concerns and multiple consumers can subscribe
# to ain/vals from any other subprocess without contending for the
# module's Tcl commands.
#
# The module is loaded and configured here but acquisition is OFF by default
# (host-side analog input now lives on the extio box). Rigs with a real
# host-side MCP320x call `ainStart 1` from local/ain.tcl.
#
# Publishes:
#   ain/vals         - uint16 packed per-channel samples (DSERV_SHORT)
#   ain/interval_ms  - current acquisition interval (int)
#   ain/info         - convenience snapshot of current state (string dict)
#

package require dlsh

# disable exit (standard subprocess pattern)
proc exit {args} { error "exit not available for this subprocess" }

# enable error logging
errormon enable

# Load the ain module into this subprocess interpreter.
# On non-Linux / no-SPI hosts the module still loads and registers its
# Tcl commands; it just sits idle with fd=-1 (simulation mode).
load ${dspath}/modules/dserv_ain[info sharedlibextension]

# Default configuration: 2 channels, default "ain" prefix. This applies to
# both hardware and simulation paths. ainSetNchan is a no-op when hardware
# is present (nchan is fixed at init time).
ainSetNchan 2

# Acquisition is NOT started here. Host-side analog input has moved to the
# extio box, so the default deployment has no MCP320x on the host SPI bus.
#
# Starting it by default was actively harmful: the module decides it has
# hardware purely by whether open("/dev/spidev0.0") succeeds, which reports
# that a device node exists, not that a chip answers. On any host whose DT
# enables an LPSPI controller (e.g. the i.MX93 FRDM board) that open()
# succeeds with nothing on the bus, so ain would clock 1 kHz of SPI
# transactions, publish ain/vals as all-zeros, and cascade ~4k datapoint
# updates/sec through the slider/em Tcl subprocesses — ~67% of a core on an
# i.MX93, and a slider pinned at full deflection.
#
# Rigs that really do have a host-side ADC opt in from local/ain.tcl (sourced
# at the end of this file) with:
#     ainStart 1

# Publish a human-readable snapshot of current state for UI / debugging.
# Consumers can also call ainGetInfo directly over the dserv Tcl bridge.
namespace eval ain {
    proc publish_info {} {
        if { [catch { set info [ainGetInfo] } err] } {
            dservSet ain/info "error: $err"
            return
        }
        dservSet ain/info $info
    }
}

ain::publish_info

# Local deployment overrides (per-rig channel inversions, non-default
# rate or prefix, etc.). Not tracked in git - each deployment owns its
# own local/ain.tcl. See local/ain.tcl.EXAMPLE for the template.
if { [file exists $dspath/local/ain.tcl] } {
    source $dspath/local/ain.tcl
    ain::publish_info
}

puts "ain subprocessor started"
