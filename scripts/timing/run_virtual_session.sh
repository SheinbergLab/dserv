#!/bin/sh
#
# Drive a reproducible ESS session with no animal and no hardware, so the
# request-path timing harness measures the same thing on every platform.
#
#   ./run_virtual_session.sh start [system protocol variant]
#   ./run_virtual_session.sh stop
#
# The point of the virtual subject here is not convenience, it is CONTROL:
# it responds on a fixed schedule blind to trial type, so a Pi 5 and an
# i.MX95 receive byte-identical input and any difference in rho is
# attributable to the platform rather than to subject behaviour.
#
# See docs/request_timing.md.

set -e

DSPATH=${DSPATH:-/usr/local/dserv}
SYS=${2:-pursuit}
PROTO=${3:-ballistic}
VARIANT=${4:-ballistic_detect}
EYE_MS=${EYE_MS:-4}          # 4 ms = 250 Hz, a realistic tracker rate
NAME=${NAME:-timing_run}

case "$1" in
start)
    # virtual_subject.tcl is an EXAMPLE and is NOT shipped in the .deb, so on a
    # packaged install it will not be under $DSPATH/config. Fall back to a copy
    # sitting next to this script (scp it across with the harness).
    # virtual_subject.tcl is an EXAMPLE and is NOT shipped in the .deb, so on a
    # packaged install it will not be under $DSPATH/config. Fall back to a copy
    # next to this script (scp it across with the harness).
    #
    # The path MUST be absolute: `source` is evaluated inside dserv, which
    # resolves relative paths against ITS cwd, not this script's.
    HERE=$(cd "$(dirname "$0")" && pwd)
    VSUBJ="$DSPATH/config/virtual_subject.tcl"
    [ -f "$VSUBJ" ] || VSUBJ="$HERE/virtual_subject.tcl"
    [ -f "$VSUBJ" ] || {
        echo "virtual_subject.tcl not found (looked in $DSPATH/config and $HERE)." >&2
        echo "Copy it from the repo: scp config/virtual_subject.tcl <host>:$HERE/" >&2
        exit 1; }

    if [ "$(dservctl virtual_subject 'expr 6*7' 2>&1)" = "42" ]; then
        echo "virtual_subject already running -- re-sourcing $VSUBJ"
        dservctl virtual_subject "source $VSUBJ" >/dev/null 2>&1 || true
    else
        echo "spawning virtual_subject from $VSUBJ ..."
        # dservctl exits 0 even when the Tcl it sent raises, so its return code
        # proves nothing and must not gate anything -- the probe below is the
        # real check. Hence the `|| true`, which also keeps `set -e` from
        # killing the script here without printing anything.
        dservctl -c "subprocess virtual_subject \"source $VSUBJ\"" >/dev/null 2>&1 || true
        sleep 1
        if [ "$(dservctl virtual_subject 'expr 6*7' 2>&1)" != "42" ]; then
            echo "virtual_subject did not come up. dserv said:" >&2
            dservctl -c "subprocess virtual_subject \"source $VSUBJ\"" 2>&1 | head -3 >&2
            exit 1
        fi
    fi

    echo "loading $SYS/$PROTO/$VARIANT..."
    dservctl ess "::ess::load_system $SYS $PROTO $VARIANT" >/dev/null
    sleep 3
    dservctl ess '::ess::set_param require_fixation 0' >/dev/null

    # REQUIRED, not optional: require_fixation 0 means the paradigm never
    # asks for an eye, so without this there is no eye traffic at all and
    # rho is badly underestimated -- that stream is a top contributor.
    echo "starting virtual_eye at ${EYE_MS}ms..."
    dservctl virtual_eye "start $EYE_MS" >/dev/null

    dservctl ess "::ess::file_open $NAME" >/dev/null
    sleep 1
    dservctl ess '::ess::start' >/dev/null
    sleep 5

    echo "status: $(dservctl -c 'dservGet ess/status')  obs: $(dservctl -c 'dservGet ess/obs_id')"
    echo
    echo "now run:  scripts/timing/collect_rho.py 300"
    echo "a block is finite -- keep the window inside one, or rerun"
    ;;

stop)
    dservctl ess '::ess::stop' >/dev/null 2>&1 || true
    sleep 1
    dservctl ess '::ess::file_close' >/dev/null 2>&1 || true
    dservctl virtual_eye 'stop' >/dev/null 2>&1 || true
    echo "status: $(dservctl -c 'dservGet ess/status')  file: '$(dservctl -c 'dservGet ess/datafile')'"
    echo
    echo "note: virtual_subject stays spawned -- it shadows 'exit' by design,"
    echo "      so only a dserv restart removes it. It is inert while stopped."
    ;;

*)
    echo "usage: $0 start|stop [system protocol variant]" >&2
    exit 1
    ;;
esac
