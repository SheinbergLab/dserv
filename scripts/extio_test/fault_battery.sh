#!/bin/sh
# fault_battery.sh -- the test of the test. Inject each virtual-box fault
# and verify the analysis pipeline CATCHES it (session verdict must be
# FAIL). A detector that cannot see planted loss is itself a failure.
#
#   ./fault_battery.sh
#
# Runs against the virtual box only (faults are a vbox feature).

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
overall=0
results=""

run_expect_fail() {
    label=$1; variant=$2; shift 2
    echo ""
    echo "########## fault: $label ##########"
    if "$HERE/run_session.sh" "$variant" --box vbox --timeout 600 \
           --expect-fail "$@"; then
        results="$results
  $label: CAUGHT (verdict FAIL, as required)"
    else
        results="$results
  $label: *** NOT CAUGHT *** (verdict did not FAIL)"
        overall=1
    fi
}

# each planted defect, one axis at a time
run_expect_fail "dropped DI edges (5%)"    events --fault drop_edge_pct=5
run_expect_fail "at_abs refused"           timing --fault atabs_refuse=1
run_expect_fail "analog blocks dropped"    analog --fault ain_gap_pct=20

echo ""
echo "==================== fault battery summary ===================="
echo "$results"
if [ $overall -eq 0 ]; then
    echo "OVERALL: every planted fault was caught"
else
    echo "OVERALL: DETECTOR GAP -- some faults were not caught"
fi
exit $overall
