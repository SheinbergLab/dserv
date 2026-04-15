#
# ain subprocess - owns the MCP320x analog input acquisition
#
# Isolates the analog-in driver from the ess subprocess so ess doesn't
# carry hardware startup concerns and multiple consumers can subscribe
# to ain/vals from any other subprocess without contending for the
# module's Tcl commands.
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

# Default configuration: 2 channels at 1 kHz, default "ain" prefix.
# These apply to both hardware and simulation paths. ainSetNchan is a
# no-op when hardware is present (nchan is fixed at init time).
ainSetNchan 2

# Start periodic acquisition. Wrapped in catch so simulation hosts
# (no SPI) don't error — the module reports via ainGetInfo whether it
# actually has hardware.
if { [catch { ainStart 1 } err] } {
    puts "ain: could not start acquisition: $err"
}

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
