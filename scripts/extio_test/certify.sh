#!/bin/sh
# certify.sh -- run the full extio_test certification sequence on one box
# and print an aggregate table. Exit 0 iff every session PASSes.
#
#   ./certify.sh [--box NAME] [--timeout SEC]
#
# Sessions run via run_session.sh (datafile per variant; extract+analyze
# auto-run on close; verdict from extio_test/verdict).

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BOX=vbox
TIMEOUT=900

while [ $# -gt 0 ]; do
    case "$1" in
        --box) BOX="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

stamp=$(date +%y%m%d_%H%M%S)
overall=0
results=""

for variant in wiring_check events timing analog amplitude mixed_soak; do
    echo ""
    echo "########## certify: $variant on $BOX ##########"
    if "$HERE/run_session.sh" "$variant" --box "$BOX" --timeout "$TIMEOUT"; then
        v=$(dservctl get extio_test/verdict 2>/dev/null)
        results="$results
  $variant: $v"
    else
        v=$(dservctl get extio_test/verdict 2>/dev/null)
        results="$results
  $variant: ${v:-NO VERDICT} (session failed)"
        overall=1
    fi
done

echo ""
echo "==================== certification summary ($BOX, $stamp) ===================="
echo "$results"
if [ $overall -eq 0 ]; then
    echo "OVERALL: PASS"
else
    echo "OVERALL: FAIL"
fi
exit $overall
