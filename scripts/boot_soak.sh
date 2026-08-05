#!/bin/bash
#
# boot_soak.sh -- restart dserv N times and catch a truncated boot.
#
# config/dsconf.tcl runs as ONE Tcl_Eval, so a script-level error anywhere in
# it silently stops the boot: everything below that line never runs. It is
# intermittent, so the only way to catch it is repetition. After each restart
# this checks the boot breadcrumbs dsconf.tcl publishes:
#
#   system/boot_complete  1 only if dsconf.tcl reached its last line
#   system/boot_stage     the last step it attempted
#
# and on a bad run dumps everything needed to diagnose it -- the journal, the
# per-subprocess <name>/init_error dpoints, and each interp's stdout -- into
# /tmp/dserv_badboot_<n>/ before stopping so the box is left in the failed
# state for a live poke.
#
# Boxes still running an older dsconf.tcl have no boot_complete dpoint; this
# falls back to "is the LAST subprocess dsconf starts present?", which detects
# the same truncation without the breadcrumb telling you where.
#
# Usage:  scripts/boot_soak.sh [runs]        (default 50)
#         must run ON the dserv box, as a user who can `sudo systemctl`
#
# SETTLE=<seconds> controls how long to let the boot tail finish before the
# next restart (default 5). SETTLE=0 deliberately restarts INTO that window --
# deferred registry sync, extio reconnect, per-subprocess timers, SendClient
# teardown -- which is where the Aug-2026 SIGSEGV cluster on office-stim lived
# (those were Restart=on-failure retries landing on a still-settling system).
# Run both: the settled pass tests boot, the SETTLE=0 pass tests boot-vs-teardown.
#
set -u

RUNS=${1:-50}
SETTLE=${SETTLE:-5}
PORT=2570
LAST_SUBPROCESS=virtual_slider   # fallback probe: last thing dsconf.tcl starts

# Send one script to the dserv main interp; empty string if it does not answer.
dsq() {
    printf '%s\n' "$1" | timeout 5 nc -q1 -w3 localhost $PORT 2>/dev/null
}

# dservGet that yields "" instead of erroring when the dpoint is absent.
dsget() {
    dsq "if {[dservExists $1]} { dservGet $1 } else { return \"\" }"
}

main_pid() { systemctl show dserv --property=MainPID --value 2>/dev/null; }

wait_for_dserv() {
    # The control port answers as soon as the listener thread is up, which is
    # BEFORE dsconf.tcl finishes -- requests queue behind it. So a reply to
    # `expr 1+1` already means dsconf.tcl returned (or died); that is exactly
    # the barrier we want, and it distinguishes a truncation (answers, missing
    # subprocesses) from a hang (never answers).
    local t
    for t in $(seq 1 120); do
        [ "$(dsq 'expr 1+1')" = "2" ] && return 0
        sleep 0.5
    done
    return 1
}

capture() {
    local run=$1 why=$2
    local dir=/tmp/dserv_badboot_$run
    mkdir -p "$dir"
    echo "  !!! $why -- capturing to $dir"

    sudo journalctl -u dserv -b --no-pager | tail -200 > "$dir/journal.log"
    dsq 'subprocessInfo'          > "$dir/subprocesses.txt"
    dsq 'dservKeys *init_error*'  > "$dir/init_error_keys.txt"

    local name
    for name in $(dsq 'dservKeys *init_error*'); do
        echo "=== $name ===" >> "$dir/init_errors.txt"
        dsget "$name"        >> "$dir/init_errors.txt"
    done

    for name in em ain slider input juicer extio ptp sound openephys powermon \
                ess scripts viz configs df docs stim mesh db trialsync; do
        {
            echo "=== $name/stdout ==="
            dsget "$name/stdout"
            echo "=== $name/init_error ==="
            dsget "$name/init_error"
        } >> "$dir/subprocess_output.txt"
    done

    {
        echo "boot_complete = $(dsget system/boot_complete)"
        echo "boot_stage    = $(dsget system/boot_stage)"
        echo "ess/ipaddr    = $(dsget ess/ipaddr)"
        echo "ess ::nTimers = $(dsq 'send ess {info exists ::nTimers}')"
        echo "ess errorInfo = $(dsq 'send ess {set ::errorInfo}')"
    } > "$dir/summary.txt"

    echo "  --- summary ---"
    sed 's/^/  /' "$dir/summary.txt"
}

for i in $(seq 1 "$RUNS"); do
    sudo systemctl restart dserv

    if ! wait_for_dserv; then
        capture "$i" "dserv control port never answered (hang or crash loop)"
        exit 1
    fi

    # PID at the moment the port first answered. Restart=on-failure means a
    # boot that SIGSEGVs is silently replaced ~10s later by a healthy one, and
    # every check below would then pass against the REPLACEMENT and score the
    # run clean. Comparing the pid across the scoring window catches that.
    pid_before=$(main_pid)

    complete=$(dsget system/boot_complete)
    stage=$(dsget system/boot_stage)
    subs=$(dsq 'subprocessInfo')
    nsub=$(( $(echo "$subs" | wc -w) / 2 ))

    if [ -n "$complete" ]; then
        ok=$([ "$complete" = "1" ] && echo yes || echo no)
    else
        # older dsconf.tcl: no breadcrumbs, probe the last subprocess instead
        stage="(no breadcrumbs -- old dsconf.tcl)"
        case " $subs " in
            *" $LAST_SUBPROCESS "*) ok=yes ;;
            *)                      ok=no  ;;
        esac
    fi

    echo "run $i/$RUNS  complete=$ok  nsub=$nsub  stage=$stage"

    if [ "$ok" != "yes" ]; then
        capture "$i" "TRUNCATED BOOT"
        echo "Stopping so the box stays in the failed state."
        exit 1
    fi

    [ "$SETTLE" != "0" ] && sleep "$SETTLE"

    pid_after=$(main_pid)
    if [ "$pid_before" != "$pid_after" ]; then
        capture "$i" "dserv DIED and was auto-restarted (pid $pid_before -> $pid_after)"
        ls -la /tmp/core.* 2>/dev/null | tee -a "/tmp/dserv_badboot_$i/cores.txt"
        echo "Stopping -- this is a crash, not a truncation; check /tmp/core.*"
        exit 1
    fi
done

echo "$RUNS/$RUNS boots complete (SETTLE=$SETTLE) -- no truncation seen."
