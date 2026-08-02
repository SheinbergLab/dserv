#!/bin/sh
# run_session.sh -- drive one extio_test session end to end against a live
# dserv: (optionally) spawn the virtual box, load the variant, open a
# datafile, run to completion, close (which kicks the df extract+analyze
# pipeline), and report the verdict.
#
#   ./run_session.sh <variant> [options]
#
#   options:
#     --box NAME        box under test (default vbox; use box01 on a rig)
#     --virtual         ensure the virtual box subprocess is running (default
#                       when --box is vbox)
#     --no-virtual      do not spawn the virtual box even for vbox
#     --name BASE       datafile basename (default <box>_<variant>_<stamp>)
#     --timeout SEC     run timeout (default 900)
#     --fault K=V       set extio_vbox/fault/K to V for this session
#                       (repeatable); cleared afterwards
#     --expect-fail     exit 0 only if the verdict is FAIL (fault sessions)
#
# The verdict is read from the extio_test/verdict datapoint that
# extio_test_analyze.tcl publishes when the df pipeline finishes.
#
# dservctl exit codes prove nothing (it exits 0 even when the Tcl raises) --
# every step is verified by probing its effect.

set -u

VARIANT="${1:-wiring_check}"
[ $# -gt 0 ] && shift

BOX=vbox
VIRTUAL=auto
NAME=""
TIMEOUT=900
EXPECT=PASS
FAULTS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --box) BOX="$2"; shift 2 ;;
        --virtual) VIRTUAL=yes; shift ;;
        --no-virtual) VIRTUAL=no; shift ;;
        --name) NAME="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --fault) FAULTS="$FAULTS $2"; shift 2 ;;
        --expect-fail) EXPECT=FAIL; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

DSPATH="${DSPATH:-/Users/sheinb/src/dserv}"
VBOX_SRC="$DSPATH/config/virtual_extio.tcl"
[ -f "$VBOX_SRC" ] || VBOX_SRC=/usr/local/dserv/config/virtual_extio.tcl
[ -n "$NAME" ] || NAME="${BOX}_${VARIANT}_$(date +%y%m%d%H%M%S)"

die() { echo "FATAL: $*" >&2; exit 1; }

# ---------------------------------------------------------------- virtual box
if [ "$VIRTUAL" = auto ]; then
    [ "$BOX" = vbox ] && VIRTUAL=yes || VIRTUAL=no
fi
if [ "$VIRTUAL" = yes ]; then
    if [ "$(dservctl virtual_extio 'expr 6*7' 2>&1)" = "42" ]; then
        echo "virtual_extio already running -- re-sourcing"
        dservctl virtual_extio "source $VBOX_SRC" >/dev/null 2>&1 || true
    else
        echo "spawning virtual_extio from $VBOX_SRC ..."
        dservctl -c "subprocess virtual_extio \"source $VBOX_SRC\"" >/dev/null 2>&1 || true
        sleep 1
        [ "$(dservctl virtual_extio 'expr 6*7' 2>&1)" = "42" ] || {
            echo "virtual_extio did not come up. dserv said:" >&2
            dservctl -c "subprocess virtual_extio \"source $VBOX_SRC\"" 2>&1 | head -3 >&2
            exit 1
        }
    fi
    # wait for its watchdog to tick at least once (proves the timer runs)
    i=0
    while [ $i -lt 30 ]; do
        wd=$(dservctl get extio/vbox/state/watchdog 2>/dev/null || true)
        [ -n "$wd" ] && case "$wd" in *"not found"*) : ;; *) break ;; esac
        i=$((i+1)); sleep 0.5
    done
    [ $i -lt 30 ] || die "virtual box watchdog never appeared"
fi

# ---------------------------------------------------------------- faults
clear_faults() {
    for kv in $FAULTS; do
        k=${kv%%=*}
        dservctl -c "dservSet extio_vbox/fault/$k 0" >/dev/null 2>&1 || true
    done
}
for kv in $FAULTS; do
    k=${kv%%=*}; v=${kv#*=}
    echo "FAULT: extio_vbox/fault/$k = $v"
    dservctl -c "dservSet extio_vbox/fault/$k $v" >/dev/null 2>&1 || true
done
trap 'clear_faults' EXIT

# ---------------------------------------------------------------- boot hold
# A real box with no PTP grandmaster (USB rigs) holds its analog sampler for
# 60 s after boot (BOX_AIN_BOOT_HOLD_MAX_MS) so no sample ever carries an
# undisciplined clock stamp. state/watchdog counts uptime seconds -- wait it
# past the hold before any session that streams, or the first analog trial
# honestly reports "no blocks arrived".
if [ "$BOX" != "vbox" ]; then
    wd=$(dservctl get extio/$BOX/state/watchdog 2>/dev/null || echo 0)
    case "$wd" in ''|*[!0-9]*) wd=0 ;; esac
    if [ "$wd" -lt 65 ] 2>/dev/null; then
        echo "box uptime ${wd}s < analog boot-hold; waiting..."
        while [ "$wd" -lt 65 ] 2>/dev/null; do
            sleep 5
            wd=$(dservctl get extio/$BOX/state/watchdog 2>/dev/null || echo 0)
            case "$wd" in ''|*[!0-9]*) wd=0 ;; esac
        done
    fi
fi

# ---------------------------------------------------------------- load + run
echo "loading extio_test/loopback/$VARIANT (box $BOX) ..."
dservctl ess "::ess::load_system extio_test loopback $VARIANT" >/dev/null 2>&1 || true
sleep 3
loaded=$(dservctl -c 'dservGet ess/system' 2>/dev/null)
[ "$loaded" = "extio_test" ] || die "system did not load (ess/system='$loaded'); check ess/load_error"
err=$(dservctl -c 'dservGet ess/load_error' 2>/dev/null)
[ -z "$err" ] || [ "$err" = "{}" ] || die "load error: $err"

dservctl ess "::ess::set_param box $BOX" >/dev/null 2>&1 || true

dservctl -c 'catch {dservClear extio_test/verdict}' >/dev/null 2>&1 || true

echo "opening datafile $NAME ..."
dservctl ess "::ess::file_open $NAME" >/dev/null 2>&1 || true
sleep 1
df=$(dservctl -c 'dservGet ess/datafile' 2>/dev/null)
[ -n "$df" ] || die "datafile did not open"
echo "datafile: $(dservctl -c 'dservGet ess/datafile_path' 2>/dev/null)"

echo "starting ..."
dservctl ess '::ess::start' >/dev/null 2>&1 || true

# run until the state system reaches its end (status back to stopped)
elapsed=0
while [ $elapsed -lt "$TIMEOUT" ]; do
    st=$(dservctl -c 'dservGet ess/status' 2>/dev/null)
    obs=$(dservctl -c 'dservGet ess/obs_id' 2>/dev/null)
    if [ "$st" = "stopped" ] && [ $elapsed -gt 2 ]; then
        break
    fi
    [ $((elapsed % 15)) -eq 0 ] && echo "  running... obs=$obs ($elapsed s)"
    sleep 2
    elapsed=$((elapsed+2))
done
st=$(dservctl -c 'dservGet ess/status' 2>/dev/null)
if [ "$st" != "stopped" ]; then
    echo "TIMEOUT after $TIMEOUT s -- stopping" >&2
    dservctl ess '::ess::stop' >/dev/null 2>&1 || true
    sleep 1
fi

echo "closing datafile (kicks extract+analyze) ..."
dservctl ess '::ess::file_close' >/dev/null 2>&1 || true

# ---------------------------------------------------------------- verdict
verdict=""
i=0
while [ $i -lt 60 ]; do
    v=$(dservctl get extio_test/verdict 2>/dev/null || true)
    case "$v" in
        PASS*|FAIL*) verdict="$v"; break ;;
    esac
    i=$((i+1)); sleep 1
done
clear_faults
trap - EXIT

echo ""
if [ -z "$verdict" ]; then
    echo "NO VERDICT after 60 s -- df pipeline did not produce one." >&2
    echo "check: dservctl -c 'dservGet ess/lastfile' and the df subprocess log" >&2
    exit 1
fi
echo "VERDICT: $verdict"
want="$EXPECT"
got=${verdict%% *}
if [ "$got" = "$want" ]; then
    echo "session outcome matches expectation ($want)"
    exit 0
else
    echo "session outcome $got != expected $want" >&2
    exit 1
fi
