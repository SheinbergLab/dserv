#
# scripts subprocess — script-management engine
#
# Owns all registry-facing script management (sync/push previews, pulls,
# pushes, diffs, history) so registry HTTP and tree hashing never block
# the ess interp (experiment control) or the main interp (client
# serving). Commands live in the scripts:: namespace and publish their
# results to scripts/* datapoints — see lib/ess_scripts-1.0.tm.
#
# Invoke from a client either synchronously:
#     send scripts {scripts::sync_preview}
# or fire-and-forget with the result arriving by datapoint subscription:
#     sendNoReply scripts {scripts::sync_preview}   -> scripts/sync_preview
#
package require dlsh
package require yajltcl

tcl::tm::add $dspath/lib

# enable error logging
errormon enable

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

package require ess_scripts

# Registry url/workgroup come from ESS_REGISTRY_URL / ESS_WORKGROUP
# (local/pre-registry.tcl) with the ess/registry/* datapoints as
# fallback; system tree from ESS_SYSTEM_PATH; project tracks the
# ess/project datapoint.
scripts::init

# Deferred initial workgroup sync: dserv boots on the on-disk tree
# immediately; the registry pull lands here a few seconds later without
# blocking boot, ess, or any client. (Replaces the old timer-7 deferred
# ess::sync_base hack in essconf.tcl.)
dservAfter 5000 scripts::initial_sync

# Periodic unpushed-changes scan (scripts/dirty, feeds the Sync badge).
# Purely local (base-manifest compare, no network); self-re-arms every
# 5 minutes so out-of-band edits surface without any GUI action.
dservAfter 8000 scripts::_dirty_periodic

puts "scripts subprocess configured"
