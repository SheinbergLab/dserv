# trialsync subprocess — queue ess/trialinfo to SQLite outbox, POST batches over HTTP.
# Incomplete trials (numeric status < 0 in smallint range) omit top-level stiminfo in the outbox payload to save SQLite space; ingest stores stim_info NULL for those rows.
# Bootstrapped from local/pre-trial-sync.tcl. Subscribes to ess/trialinfo and ess/obs_active when the ingest secret file is readable at init.
# Per-trial reward_ml comes from ESS trialinfo JSON (set at save_trial_info).
# Obs window: ess/obs_active (triggers.tcl on evt:19 BEGINOBS / evt:20 ENDOBS) opens/closes; hold ess/trialinfo;
# on obs close enqueue held trialinfo as-is.
# On each ess/trialinfo, if ingest URL is unset or blank, no-op (no enqueue).
# URL is resolved on every call via trialsync::_ingest_base_url (env first, then datapoint).
# Base URL must be the full ingest script URL (e.g. …/ingest.php); trialsync::ingest_endpoint posts there as-is.
# Flush sends schema_version 2 JSON including ingest_key from trialsync::_ingest_secret_path (trimmed file contents).
# Remote ingest: reward_ml lives inside each trials[].payload JSON string (not a top-level POST field).

# dserv subprocess has no Tcl event loop between datapoints: `after` and
# `http::geturl -command` never run. Use synchronous HTTP and sleep/direct calls
# for flush pacing.

# Optional: env ESS_TRIAL_INGEST_TIMEOUT_MS (integer ms) overrides http_timeout_ms for POST.

package require qpcs
package require sqlite3
package require yajltcl
package require http

# Subprocess does not run dsconf.tcl; define install dir like config/dsconf.tcl and triggers.tcl.
# Use ::dspath — procs live in namespace trialsync and may not see an unqualified global.
if {![info exists ::dspath]} {
    set ::dspath [file dir [info nameofexecutable]]
}

proc exit {args} { error "exit not available for this subprocess" }

namespace eval trialsync {
    variable debug 0
    variable db_path ""
    variable outbox_opened 0
    variable last_send_end_ms 0
    variable upload_in_progress 0
    variable deferred_flush 0

    variable max_batch_rows 10
    variable max_batch_payload_bytes 524288 ;# 512 KiB sum(payload_bytes) after row 1
    variable pace_ms 500
    variable http_timeout_ms 60000

    variable ingest_key_initialized 0
    variable ingest_key_cached_val ""

    variable consecutive_failures 0
    variable backoff_until_ms 0
    variable startup_drain_active 0

    variable obs_active 0
    variable pending_trialinfo ""
}

if {[info exists ::env(ESS_TRIAL_SYNC_DEBUG)]} {
    set _trial_sync_dbg [string tolower [string trim $::env(ESS_TRIAL_SYNC_DEBUG)]]
    if {$_trial_sync_dbg in {1 true yes on}} {
        set trialsync::debug 1
    }
    unset _trial_sync_dbg
}

proc trialsync::_dbg {msg} {
    variable debug
    if {!$debug} {
        return
    }
    puts stderr "trialsync [clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S}] $msg"
    flush stderr
}

proc trialsync::_ingest_base_url {} {
    if {[info exists ::env(ESS_TRIAL_INGEST_BASE_URL)]} {
        set u [string trim $::env(ESS_TRIAL_INGEST_BASE_URL)]
        if {$u ne ""} {
            return $u
        }
    }
    if {[dservExists configs/trial_ingest_base_url]} {
        return [string trim [dservGet configs/trial_ingest_base_url]]
    }
    return ""
}

proc trialsync::_http_timeout_ms {} {
    variable http_timeout_ms
    if {[info exists ::env(ESS_TRIAL_INGEST_TIMEOUT_MS)]} {
        set t [string trim $::env(ESS_TRIAL_INGEST_TIMEOUT_MS)]
        if {[string is integer -strict $t] && $t > 0} {
            return $t
        }
    }
    return $http_timeout_ms
}

proc trialsync::ingest_endpoint {} {
    set b [trialsync::_ingest_base_url]
    if {$b eq ""} {
        return ""
    }
    return [string trimright $b /]
}

proc trialsync::_ingest_secret_path {} {
    return /etc/dserv/trial_ingest_secret
}

# Single-line (or trimmed) shared secret; cached after first read attempt per subprocess.
proc trialsync::_ingest_key {} {
    variable ingest_key_initialized
    variable ingest_key_cached_val
    if {$ingest_key_initialized} {
        return $ingest_key_cached_val
    }
    set ingest_key_initialized 1
    set path [trialsync::_ingest_secret_path]
    if {![file readable $path]} {
        set ingest_key_cached_val ""
        return ""
    }
    if {[catch {
        set f [open $path r]
        fconfigure $f -translation auto
        set contents [read $f]
        close $f
    } err]} {
        set ingest_key_cached_val ""
        return ""
    }
    set key [string trim $contents]
    if {$key eq ""} {
        set ingest_key_cached_val ""
        return ""
    }
    set ingest_key_cached_val $key
    return $key
}

# Block current thread; dserv subprocess does not service Tcl `after` timers.
proc trialsync::sleep_ms {ms} {
    if {$ms <= 0} {
        return
    }
    if {[catch {exec sleep [expr {$ms / 1000.0}]}]} {
        set end [expr {[clock milliseconds] + int($ms)}]
        while {[clock milliseconds] < $end} {}
    }
}

proc trialsync::failure_backoff_ms {n} {
    if {$n >= 10} {
        return 300000
    }
    if {$n >= 5} {
        return 60000
    }
    if {$n >= 2} {
        return 10000
    }
    return 0
}

proc trialsync::backoff_active {} {
    variable backoff_until_ms
    return [expr {[clock milliseconds] < $backoff_until_ms}]
}

proc trialsync::note_ingest_success {} {
    variable consecutive_failures
    variable backoff_until_ms
    if {$consecutive_failures > 0 || $backoff_until_ms > 0} {
        trialsync::_dbg "ingest backoff reset (was consecutive_failures=$consecutive_failures)"
    }
    set consecutive_failures 0
    set backoff_until_ms 0
}

proc trialsync::note_ingest_failure {} {
    variable consecutive_failures
    variable backoff_until_ms
    incr consecutive_failures
    set tier_ms [trialsync::failure_backoff_ms $consecutive_failures]
    if {$tier_ms > 0} {
        set until [expr {[clock milliseconds] + $tier_ms}]
        if {$until > $backoff_until_ms} {
            set backoff_until_ms $until
        }
        puts stderr "trialsync: ingest failure #$consecutive_failures, backoff ${tier_ms}ms before next POST"
        flush stderr
        trialsync::_dbg "ingest backoff: consecutive_failures=$consecutive_failures tier_ms=$tier_ms until_ms=$backoff_until_ms"
    } else {
        trialsync::_dbg "ingest failure #$consecutive_failures (no tier backoff yet)"
    }
}

# Only call from post_request_cleanup failure branch (never from on_trialinfo).
proc trialsync::sleep_backoff_remaining {} {
    variable backoff_until_ms
    set wait [expr {$backoff_until_ms - [clock milliseconds]}]
    if {$wait > 0} {
        trialsync::_dbg "ingest backoff sleep ${wait}ms"
        trialsync::sleep_ms $wait
    }
}

# Returns "" when ready to POST; else backoff, upload_in_progress, empty_url, outbox_empty, no_workgroup, no_ingest_key.
proc trialsync::flush_block_reason {} {
    variable upload_in_progress
    if {[trialsync::backoff_active]} {
        return backoff
    }
    if {$upload_in_progress} {
        return upload_in_progress
    }
    if {[trialsync::ingest_endpoint] eq ""} {
        return empty_url
    }
    trialsync::ensure_outbox_open
    if {[llength [trialsync::select_batch]] == 0} {
        return outbox_empty
    }
    if {![info exists ::env(ESS_WORKGROUP)] || $::env(ESS_WORKGROUP) eq ""} {
        return no_workgroup
    }
    if {[trialsync::_ingest_key] eq ""} {
        return no_ingest_key
    }
    return ""
}

# Local config gaps — no HTTP request; do not count toward API auth_fail_ip backoff.
proc trialsync::flush_block_reason_is_local {reason} {
    return [expr {$reason in {empty_url no_workgroup no_ingest_key}}]
}

# Tiered backoff after a real ingest HTTP failure (403, 429, transport, etc.).
proc trialsync::apply_ingest_failure_backoff {} {
    variable consecutive_failures
    variable pace_ms
    variable startup_drain_active
    trialsync::note_ingest_failure
    if {$startup_drain_active} {
        return
    }
    trialsync::sleep_backoff_remaining
    if {[trialsync::failure_backoff_ms $consecutive_failures] == 0} {
        trialsync::sleep_ms [expr {max(500, $pace_ms)}]
    }
}

# Build POST body: yajl generator (correct escaping); schema_version 2 adds ingest_key.
# trials[]: block_id, trial_id, completed_at_ms (rig outbox enqueue, Unix ms), payload (JSON string).
proc trialsync::build_json_body {workgroup ingest_key hostname hostaddr trials_tuples} {
    set enc [yajl create #auto]
    try {
        $enc map_open
        $enc string schema_version
        $enc integer 2
        $enc string workgroup
        $enc string $workgroup
        $enc string ingest_key
        $enc string $ingest_key
        $enc string hostname
        $enc string $hostname
        $enc string hostaddr
        $enc string $hostaddr
        $enc string trials
        $enc array_open
        foreach t $trials_tuples {
            lassign $t block_id trial_id payload_json completed_at_ms
            $enc map_open
            $enc string block_id
            if {$block_id eq ""} {
                $enc null
            } elseif {[string is integer -strict $block_id]} {
                $enc integer $block_id
            } else {
                $enc number $block_id
            }
            $enc string trial_id
            if {$trial_id eq ""} {
                $enc null
            } elseif {[string is integer -strict $trial_id]} {
                $enc integer $trial_id
            } else {
                $enc number $trial_id
            }
            $enc string completed_at_ms
            if {![string is integer -strict $completed_at_ms]} {
                error "trialsync internal: non-integer completed_at_ms"
            }
            $enc integer $completed_at_ms
            $enc string payload
            $enc string $payload_json
            $enc map_close
        }
        $enc array_close
        $enc map_close
        return [$enc get]
    } finally {
        catch {$enc delete}
    }
}

# True when JSON status is a whole number in smallint range (same idea as ingest.php) and status < 0.
# Missing, null, empty, non-numeric, or status >= 0 => false (keep stiminfo).
proc trialsync::trial_status_incomplete_p {jd} {
    if {![dict exists $jd status]} {
        return 0
    }
    set v [dict get $jd status]
    if {$v eq ""} {
        return 0
    }
    if {$v eq "null"} {
        return 0
    }
    set i ""
    if {[string is integer -strict $v]} {
        set i [expr {int($v)}]
    } elseif {[string is double -strict $v]} {
        set d [expr {double($v)}]
        if {$d != $d} {
            return 0
        }
        if {$d + 1 == $d || $d - 1 == $d} {
            return 0
        }
        if {$d != floor($d)} {
            return 0
        }
        set i [expr {entier($d)}]
    } else {
        return 0
    }
    if {$i < -32768 || $i > 32767} {
        return 0
    }
    expr {$i < 0}
}

# json2dict_ex: JSON arrays become dicts with keys 0,1,...,n-1.
proc trialsync::_dict_ex_is_json_array {d} {
    if {[catch {dict size $d} n]} {
        return 0
    }
    if {$n == 0} {
        return 0
    }
    set mx -1
    foreach k [dict keys $d] {
        if {![string is integer -strict $k]} {
            return 0
        }
        if {$k < 0} {
            return 0
        }
        if {$k > $mx} {
            set mx $k
        }
    }
    expr {$mx == $n - 1}
}

proc trialsync::_emit_json_scalar {enc v} {
    if {$v eq "true"} {
        $enc bool true
        return
    }
    if {$v eq "false"} {
        $enc bool false
        return
    }
    if {$v eq "null"} {
        $enc null
        return
    }
    if {[string is integer -strict $v]} {
        $enc integer $v
        return
    }
    if {[string is double -strict $v]} {
        set d [expr {double($v)}]
        if {$d != $d || $d + 1 == $d || $d - 1 == $d} {
            error "trialsync: non-finite number in JSON re-encode"
        }
        if {$d == floor($d)} {
            $enc integer [expr {entier($d)}]
        } else {
            $enc number $v
        }
        return
    }
    $enc string $v
}

proc trialsync::_emit_json_value {enc v} {
    if {![catch {dict size $v} _n]} {
        if {[trialsync::_dict_ex_is_json_array $v]} {
            set n [dict size $v]
            $enc array_open
            for {set i 0} {$i < $n} {incr i} {
                trialsync::_emit_json_value $enc [dict get $v $i]
            }
            $enc array_close
        } else {
            $enc map_open
            dict for {k val} $v {
                $enc string $k
                trialsync::_emit_json_value $enc $val
            }
            $enc map_close
        }
    } else {
        trialsync::_emit_json_scalar $enc $v
    }
}

# Serialize structure from ::yajl::json2dict_ex (requires yajl-tcl json2dict_ex).
proc trialsync::_json_dict_ex_to_string {root} {
    set enc [yajl create #auto]
    try {
        trialsync::_emit_json_value $enc $root
        return [$enc get]
    } finally {
        catch {$enc delete}
    }
}

# Returns: payload, payload_bytes, trial_id (or ""), parse_ok (1 if JSON object parsed).
proc trialsync::prepare_outbox_payload {data} {
    set rawlen [string length $data]
    set jd {}
    set parse_ex 0
    if {[info commands ::yajl::json2dict_ex] ne "" && ![catch {::yajl::json2dict_ex $data} jd]} {
        set parse_ex 1
    } elseif {![catch {::yajl::json2dict $data} jd]} {
        set parse_ex 0
    } else {
        return [list $data $rawlen "" 0]
    }
    set tid ""
    if {[dict exists $jd trialid]} {
        set raw [dict get $jd trialid]
        if {[string is integer -strict $raw]} {
            set tid $raw
        }
    }
    set payload $data
    set pbytes $rawlen
    if {$parse_ex && [trialsync::trial_status_incomplete_p $jd] && [dict exists $jd stiminfo]} {
        set work [dict merge $jd {}]
        dict unset work stiminfo
        if {![catch {trialsync::_json_dict_ex_to_string $work} out]} {
            set payload $out
            set pbytes [string length $out]
            trialsync::_dbg "prepare_outbox_payload: stripped stiminfo (status<0), bytes $rawlen -> $pbytes"
        } else {
            trialsync::_dbg "prepare_outbox_payload: re-encode failed after stiminfo strip; keeping raw ($out)"
        }
    }
    return [list $payload $pbytes $tid 1]
}

proc trialsync::_outbox_column_names {} {
    return [trialob eval {SELECT name FROM pragma_table_info('outbox')}]
}

# Upgrade trial_outbox.db from created_ms (Tcl-set) to completed_at_ms (SQLite DEFAULT).
proc trialsync::migrate_outbox_schema {} {
    set names [trialsync::_outbox_column_names]
    if {[llength $names] == 0} {
        return
    }
    if {[lsearch -exact $names completed_at_ms] >= 0} {
        if {[lsearch -exact $names created_ms] >= 0} {
            catch { trialob eval {ALTER TABLE outbox DROP COLUMN created_ms} }
        }
        return
    }
    if {[lsearch -exact $names created_ms] >= 0} {
        trialob eval {ALTER TABLE outbox ADD COLUMN completed_at_ms INTEGER}
        trialob eval {
            UPDATE outbox SET completed_at_ms = created_ms
            WHERE created_ms IS NOT NULL
        }
        trialob eval {
            UPDATE outbox SET completed_at_ms = CAST(unixepoch('subsec') * 1000 AS INTEGER)
            WHERE completed_at_ms IS NULL
        }
        catch { trialob eval {ALTER TABLE outbox DROP COLUMN created_ms} }
        trialsync::_dbg "migrate_outbox_schema: created_ms -> completed_at_ms"
        return
    }
    trialob eval {
        ALTER TABLE outbox ADD COLUMN completed_at_ms INTEGER NOT NULL
        DEFAULT (CAST(unixepoch('subsec') * 1000 AS INTEGER))
    }
    trialsync::_dbg "migrate_outbox_schema: added completed_at_ms"
}

proc trialsync::open_outbox {} {
    variable db_path
    set db_path [file join $::dspath db trial_outbox.db]
    set dir [file dirname $db_path]
    if {![file exists $dir]} {
        file mkdir $dir
    }
    sqlite3 trialob $db_path
    trialsync::_dbg "open_outbox path=$db_path"
    trialob eval {
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous = NORMAL;
        PRAGMA temp_store = MEMORY;
        PRAGMA busy_timeout = 8000;
    }
    trialob eval {
        CREATE TABLE IF NOT EXISTS outbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            block_id INTEGER NULL,
            trial_id INTEGER NULL,
            payload TEXT NOT NULL,
            payload_bytes INTEGER NOT NULL,
            completed_at_ms INTEGER NOT NULL DEFAULT (
                CAST(unixepoch('subsec') * 1000 AS INTEGER)
            )
        );
    }
    trialsync::migrate_outbox_schema
}

proc trialsync::ensure_outbox_open {} {
    variable outbox_opened
    if {!$outbox_opened} {
        trialsync::open_outbox
        set outbox_opened 1
    }
}

proc trialsync::select_batch {} {
    variable max_batch_rows
    variable max_batch_payload_bytes
    set flat [trialob eval [format {
        SELECT id, block_id, trial_id, payload, payload_bytes, completed_at_ms
        FROM outbox ORDER BY id ASC LIMIT %d
    } $max_batch_rows]]
    if {[llength $flat] == 0} {
        return {}
    }
    set batch {}
    set sum_pb 0
    for {set idx 0} {$idx < [llength $flat]} {incr idx 6} {
        set row_id [lindex $flat $idx]
        set block_id [lindex $flat [expr {$idx + 1}]]
        set trial_id [lindex $flat [expr {$idx + 2}]]
        set payload [lindex $flat [expr {$idx + 3}]]
        set pb [lindex $flat [expr {$idx + 4}]]
        set completed_at_ms [lindex $flat [expr {$idx + 5}]]
        if {[llength $batch] == 0} {
            lappend batch [list $row_id $block_id $trial_id $payload $pb $completed_at_ms]
            set sum_pb $pb
        } else {
            if {[llength $batch] >= $max_batch_rows} {
                break
            }
            if {$sum_pb + $pb > $max_batch_payload_bytes} {
                break
            }
            lappend batch [list $row_id $block_id $trial_id $payload $pb $completed_at_ms]
            incr sum_pb $pb
        }
    }
    return $batch
}

proc trialsync::delete_ids {ids} {
    if {[llength $ids] == 0} {
        return
    }
    foreach x $ids {
        if {![string is integer -strict $x]} {
            error "trialsync internal: non-integer id"
        }
    }
    set irows [join $ids ,]
    trialob transaction {
        trialob eval "DELETE FROM outbox WHERE id IN ($irows)"
    }
}

proc trialsync::kick_flush_scheduled {} {
    trialsync::_dbg "kick_flush_scheduled -> maybe_start_flush"
    trialsync::maybe_start_flush
}

proc trialsync::schedule_flush_after_pace {} {
    variable last_send_end_ms
    variable pace_ms
    variable upload_in_progress
    variable deferred_flush

    if {$upload_in_progress} {
        set deferred_flush 1
        trialsync::_dbg "schedule_flush: deferred (upload_in_progress)"
        return
    }

    set now [clock milliseconds]
    set wait [expr {$last_send_end_ms + $pace_ms - $now}]
    if {$wait <= 0} {
        trialsync::_dbg "schedule_flush: immediate (pace elapsed, last_ms=$last_send_end_ms pace=$pace_ms)"
        trialsync::kick_flush_scheduled
    } else {
        trialsync::_dbg "schedule_flush: blocking sleep ${wait}ms (subprocess has no Tcl event loop)"
        trialsync::sleep_ms $wait
        trialsync::kick_flush_scheduled
    }
}

proc trialsync::post_request_cleanup {ok_http} {
    variable upload_in_progress
    variable deferred_flush
    variable pace_ms
    variable last_send_end_ms
    variable startup_drain_active

    set upload_in_progress 0

    set def $deferred_flush
    set deferred_flush 0

    if {$def && $ok_http} {
        trialsync::maybe_start_flush
        return
    }

    trialsync::ensure_outbox_open
    set nleft [trialob eval {SELECT COUNT(*) FROM outbox}]
    trialsync::_dbg "post_request_cleanup: ok_http=$ok_http deferred_was=$def outbox_remaining=$nleft"
    if {$nleft <= 0} {
        return
    }

    if {!$ok_http} {
        trialsync::apply_ingest_failure_backoff
        if {!$startup_drain_active} {
            trialsync::maybe_start_flush
        }
        return
    }

    set now [clock milliseconds]
    if {$now >= $last_send_end_ms + $pace_ms} {
        trialsync::maybe_start_flush
    } else {
        trialsync::schedule_flush_after_pace
    }
}

proc trialsync::http_done {batch_ids tok} {
    if {[catch {
        set st [http::status $tok]
        set code [http::ncode $tok]
        set respBody [http::data $tok]
        http::cleanup $tok
    } err]} {
        puts stderr "trialsync: http_done internal error: $err"
        catch {http::cleanup $tok}
        trialsync::ingest_apply_result $batch_ids error 0 "" ""
        return
    }
    trialsync::ingest_apply_result $batch_ids $st $code $respBody ""
}

# Journal/syslog often truncates huge lines; write full body to disk for debugging.
proc trialsync::dump_ingest_json_for_debug {postedBody} {
    set nbytes [string length $postedBody]
    if {![info exists ::dspath]} {
        puts stderr "trialsync: invalid_json debug dump skipped (no dspath, ${nbytes} bytes lost)"
        flush stderr
        return
    }
    set path [file join $::dspath db trialsync_last_post.json]
    if {[catch {
        set f [open $path w]
        fconfigure $f -translation lf -encoding utf-8
        puts -nonewline $f $postedBody
        close $f
    } err]} {
        puts stderr "trialsync: could not write $path: $err ($nbytes bytes)"
        flush stderr
        return
    }
    regsub -all {\s+} [string range $postedBody 0 400] { } preview
    puts stderr "trialsync: invalid_json — full POST ($nbytes bytes) -> $path | preview: $preview"
    flush stderr
}

proc trialsync::ingest_apply_result {batch_ids st code respBody {postedBody {}}} {
    variable last_send_end_ms
    variable consecutive_failures
    variable backoff_until_ms

    set ok_http 0
    if {$st eq "ok" && $code >= 200 && $code < 300} {
        set ok_http 1
        if {$respBody ne "" && [catch {set d [::yajl::json2dict $respBody]}]==0 && [dict exists $d ok]} {
            set ok_http [expr {[dict get $d ok] ? 1 : 0}]
        }
    } else {
        puts stderr "trialsync: upload HTTP status=$st code=$code body=$respBody"
    }

    if {!$ok_http && $postedBody ne "" && [string match *invalid_json* $respBody]} {
        trialsync::dump_ingest_json_for_debug $postedBody
    }

    set ok_ack $ok_http
    trialsync::_dbg "ingest result: batch_ids=$batch_ids ok_http=$ok_http ok_ack=$ok_ack"
    if {$ok_ack} {
        set prev_failures $consecutive_failures
        set had_backoff [expr {$backoff_until_ms > 0}]
        trialsync::delete_ids $batch_ids
        set last_send_end_ms [clock milliseconds]
        if {$prev_failures > 0 || $had_backoff} {
            set nrows [llength $batch_ids]
            puts stderr "trialsync: ingest recovered after $prev_failures failure(s), uploaded $nrows row(s)"
            flush stderr
        }
        trialsync::note_ingest_success
    }

    trialsync::post_request_cleanup $ok_ack
}

# ngage / many hosts return 301 http->https; Tcl http does not follow redirects by default.
proc trialsync::redirect_target {current loc} {
    set loc [string trim $loc]
    if {$loc eq ""} {
        return $current
    }
    if {[string match http://* $loc] || [string match https://* $loc]} {
        return $loc
    }
    if {[string match /* $loc]} {
        if {[regexp {^(https?://[^/]+)} $current _ hostpfx]} {
            return ${hostpfx}${loc}
        }
    }
    return $loc
}

proc trialsync::location_header {tok} {
    foreach {k v} [http::meta $tok] {
        set kn [string trimleft $k -]
        if {[string equal -nocase $kn location]} {
            return [string trim $v]
        }
    }
    return ""
}

# http:// uses Tcl package http (no https). https:// uses dserv https_post (OpenSSL).
proc trialsync::ingest_http_post_follow {url body timeout} {
    set current $url
    for {set hop 0} {$hop < 6} {incr hop} {
        if {[string match https://* $current]} {
            if {![llength [info commands https_post]]} {
                error "https_post not available (need dserv with TclHttps); cannot POST to HTTPS URL"
            }
            if {[catch {https_post $current $body -timeout $timeout} respBody]} {
                if {[regexp {^HTTP ([0-9]+): (.*)$} $respBody -> c bdy]} {
                    return [list ok $c $bdy]
                }
                return [list error 0 $respBody]
            }
            return [list ok 200 $respBody]
        }

        set tok [http::geturl $current \
            -method POST \
            -type application/json \
            -query $body \
            -timeout $timeout]
        set code [http::ncode $tok]
        set st [http::status $tok]
        if {$code eq 301 || $code eq 302 || $code eq 303 || $code eq 307 || $code eq 308} {
            set loc [trialsync::location_header $tok]
            http::cleanup $tok
            if {$loc eq ""} {
                error "HTTP $code redirect without Location"
            }
            set next [trialsync::redirect_target $current $loc]
            if {$next eq $current} {
                error "HTTP $code redirect to same URL"
            }
            trialsync::_dbg "ingest HTTP $code redirect $current -> $next"
            set current $next
            continue
        }
        set respBody [http::data $tok]
        http::cleanup $tok
        return [list $st $code $respBody]
    }
    error "too many HTTP redirects (ingest)"
}

proc trialsync::maybe_start_flush {} {
    variable upload_in_progress
    variable deferred_flush

    set reason [trialsync::flush_block_reason]
    if {$reason ne ""} {
        switch -- $reason {
            backoff {
                trialsync::_dbg "maybe_start_flush: skip (ingest backoff active)"
            }
            upload_in_progress {
                set deferred_flush 1
                trialsync::_dbg "maybe_start_flush: skip (upload_in_progress)"
            }
            empty_url {
                trialsync::_dbg "maybe_start_flush: skip (empty ingest endpoint)"
            }
            outbox_empty {
                trialsync::_dbg "maybe_start_flush: skip (outbox empty)"
            }
            no_workgroup {
                trialsync::_dbg "maybe_start_flush: skip (ESS_WORKGROUP unset)"
            }
            no_ingest_key {
                trialsync::_dbg "maybe_start_flush: skip (no ingest_key)"
            }
        }
        return
    }

    trialsync::ensure_outbox_open
    set batch [trialsync::select_batch]
    set url [trialsync::ingest_endpoint]
    set ingest_key [trialsync::_ingest_key]

    set batch_ids [lmap r $batch {lindex $r 0}]
    set tuples {}
    foreach r $batch {
        lassign $r _rowid block_id trial_id payload _pb completed_at_ms
        lappend tuples [list $block_id $trial_id $payload $completed_at_ms]
    }

    set workgroup $::env(ESS_WORKGROUP)
    set hostname [info hostname]
    set hostaddr ""
    catch {
        set hostaddr [dservGet system/hostaddr]
    }

    set body [trialsync::build_json_body $workgroup $ingest_key $hostname $hostaddr $tuples]

    set http_timeout_ms [trialsync::_http_timeout_ms]
    trialsync::_dbg "maybe_start_flush: POST url=$url rows=[llength $batch] ids=$batch_ids body_bytes=[string length $body] timeout_ms=$http_timeout_ms"
    set upload_in_progress 1
    if {[catch {
        lassign [trialsync::ingest_http_post_follow $url $body $http_timeout_ms] st code respBody
        trialsync::ingest_apply_result $batch_ids $st $code $respBody $body
    } err2]} {
        puts stderr "trialsync: ingest POST failed: $err2"
        set upload_in_progress 0
        trialsync::post_request_cleanup 0
    }
}

# Best-effort drain of pending outbox rows at subprocess start. Successful batches keep
# draining; HTTP/auth failures defer remaining rows to runtime (no tiered sleeps during init).
# Local-only blocks (missing URL, workgroup, key) exit without backoff — rows stay until fixed.
# Returns {initial_count final_count}.
proc trialsync::startup_drain_outbox {} {
    variable startup_drain_active
    variable consecutive_failures
    trialsync::ensure_outbox_open
    set initial [trialob eval {SELECT COUNT(*) FROM outbox}]
    if {$initial <= 0} {
        return [list 0 0]
    }
    set startup_drain_active 1
    try {
        while {[trialob eval {SELECT COUNT(*) FROM outbox}] > 0} {
            if {[trialsync::backoff_active]} {
                trialsync::_dbg "startup_drain: defer (ingest backoff active)"
                break
            }
            set reason [trialsync::flush_block_reason]
            if {$reason ne "" && $reason ne "outbox_empty"} {
                if {[trialsync::flush_block_reason_is_local $reason]} {
                    trialsync::_dbg "startup_drain: paused (local block $reason)"
                    break
                }
                trialsync::_dbg "startup_drain: blocked ($reason)"
                continue
            }
            set nbefore [trialob eval {SELECT COUNT(*) FROM outbox}]
            set fails_before $consecutive_failures
            trialsync::maybe_start_flush
            set nafter [trialob eval {SELECT COUNT(*) FROM outbox}]
            if {$nafter >= $nbefore && $consecutive_failures > $fails_before} {
                trialsync::_dbg "startup_drain: defer after ingest failure ($nafter rows remain)"
                break
            }
            if {$nafter >= $nbefore && $consecutive_failures == $fails_before} {
                trialsync::_dbg "startup_drain: no progress, stopping"
                break
            }
        }
    } finally {
        set startup_drain_active 0
    }
    set final [trialob eval {SELECT COUNT(*) FROM outbox}]
    return [list $initial $final]
}

proc trialsync::on_obs_active {dpoint data} {
    if {[catch { trialsync::_on_obs_active_body $dpoint $data } err]} {
        puts stderr "trialsync: on_obs_active failed ($data): $err"
        flush stderr
    }
}

proc trialsync::_on_obs_active_body {dpoint data} {
    variable obs_active
    variable pending_trialinfo

    if {$data == 1 || $data eq "1"} {
        set obs_active 1
        set pending_trialinfo ""
        trialsync::_dbg "on_obs_active: obs open"
        return
    }
    if {$data == 0 || $data eq "0"} {
        set obs_active 0
        if {$pending_trialinfo ne ""} {
            trialsync::_enqueue_trialinfo_from_data $pending_trialinfo
            set pending_trialinfo ""
            trialsync::_dbg "on_obs_active: obs close enqueue"
        }
    }
}

proc trialsync::_enqueue_trialinfo_from_data {data} {
    lassign [trialsync::prepare_outbox_payload $data] payload pbytes tid _parse_ok

    set bid ""
    if {![catch {dservGet ess/block_id} btmp]} {
        if {[string is integer -strict $btmp]} {
            set bid $btmp
        }
    }

    if {[catch {
        trialob eval {
            INSERT INTO outbox (block_id, trial_id, payload, payload_bytes)
            VALUES (
                CAST(NULLIF($bid, '') AS INTEGER),
                CAST(NULLIF($tid, '') AS INTEGER),
                $payload, $pbytes)
        }
    } err]} {
        puts stderr "trialsync: outbox insert failed: $err"
        flush stderr
        return
    }

    set nq [trialob eval {SELECT COUNT(*) FROM outbox}]
    trialsync::_dbg "on_trialinfo: enqueued block_id=$bid trial_id=$tid outbox_count=$nq"
    if {[trialsync::backoff_active]} {
        variable deferred_flush
        set deferred_flush 1
        trialsync::_dbg "on_trialinfo: deferred flush (ingest backoff active, outbox=$nq)"
        return
    }
    trialsync::schedule_flush_after_pace
}

proc trialsync::on_trialinfo {dpoint data} {
    set nbytes [string length $data]
    trialsync::_dbg "on_trialinfo: len=$nbytes"
    if {[catch { trialsync::_on_trialinfo_body $dpoint $data $nbytes } err]} {
        puts stderr "trialsync: on_trialinfo failed ($nbytes bytes): $err"
        flush stderr
        return
    }
}

proc trialsync::_on_trialinfo_body {dpoint data nbytes} {
    set base [trialsync::_ingest_base_url]
    if {$base eq ""} {
        trialsync::_dbg "on_trialinfo: no ingest URL (skip)"
        return
    }
    trialsync::_dbg "on_trialinfo: ingest_base=[string range $base 0 80]"

    trialsync::ensure_outbox_open

    variable obs_active
    variable pending_trialinfo
    if {$obs_active} {
        set pending_trialinfo $data
        trialsync::_dbg "on_trialinfo: hold until obs close"
        return
    }
    trialsync::_enqueue_trialinfo_from_data $data
}

if {[trialsync::_ingest_key] eq ""} {
    puts stderr "trialsync: ingest key not found — set the shared secret in /etc/dserv/trial_ingest_secret. Exiting."
    flush stderr
    return
}

if {[trialsync::_ingest_base_url] eq ""} {
    puts stderr "trialsync: ingest server not configured — set ESS_TRIAL_INGEST_BASE_URL in local/pre-remoteservers.tcl. Exiting."
    flush stderr
    return
}

trialsync::ensure_outbox_open
lassign [trialsync::startup_drain_outbox] _initial _final
set _drained [expr {$_initial - $_final}]
set _prefix "trialsync started. API key and target server loaded."
if {$_final == 0} {
    if {$_drained == 0} {
        puts stderr "$_prefix Outbox empty."
    } else {
        puts stderr "$_prefix Outbox emptied of $_drained trials."
    }
} elseif {$trialsync::consecutive_failures > 0} {
    puts stderr "$_prefix Outbox has $_final pending trials (startup drain deferred after ingest failure)."
} else {
    puts stderr "$_prefix Outbox has $_final pending trials."
}
flush stderr
unset _initial _final _drained _prefix

dservAddExactMatch ess/trialinfo
dpointSetScript ess/trialinfo trialsync::on_trialinfo
trialsync::_dbg "init: subscribed ess/trialinfo -> trialsync::on_trialinfo"

dservAddExactMatch ess/obs_active
dpointSetScript ess/obs_active trialsync::on_obs_active
trialsync::_dbg "init: subscribed ess/obs_active -> trialsync::on_obs_active"
