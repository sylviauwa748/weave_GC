#!/usr/bin/env bash
# Weave Stage 0 - Test 4: Observer impairment.
#
# Runs the Target NORMALLY (unimpaired) while the JMX Observer deliberately
# stresses itself (--self-stress: burns CPU and allocates garbage between
# samples). Compare this run's jmx_observer.log against a Test 1 baseline
# run to see whether OBSERVER_OVERRUN events, sampling gaps, or measurement
# latency increase - and whether the Target's own numbers (as seen by the
# native/OS observers, which are NOT impaired) stayed normal throughout.
#
# This directly tests: "can we trust a measurement just because it came
# from an external process?" An impaired observer can still produce
# misleading data even though it is fully independent of the Target.
#
# Usage:
#   scripts/test4_observer_impairment.sh [duration_seconds] [jmx_port]
set -euo pipefail
cd "$(dirname "$0")/.."

DURATION="${1:-90}"
JMX_PORT="${2:-9010}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="results/test4-observer-impairment-$STAMP"
mkdir -p "$OUTDIR"
echo "Logs will be written to $OUTDIR"

scripts/run-target.sh "$JMX_PORT" > "$OUTDIR/target.log" 2>&1 &
TARGET_PID=$!
sleep 3

# The JMX observer stresses ITSELF here (--self-stress).
java -jar observer-jvm/target/weave-observer-jvm.jar 127.0.0.1 "$JMX_PORT" 1000 --self-stress \
  > "$OUTDIR/jmx_observer.log" 2>&1 &
JMX_OBS_PID=$!

# Native and OS observers run unimpaired, as a control: they watch the same
# (unimpaired) Target and should show normal, stable numbers throughout.
observer-native/native_observer "$TARGET_PID" 1000 \
  > "$OUTDIR/native_observer_control.log" 2>&1 &
NATIVE_OBS_PID=$!

observer-os/os_observer "$TARGET_PID" 1000 \
  > "$OUTDIR/os_observer_control.log" 2>&1 &
OS_OBS_PID=$!

echo "Running for ${DURATION}s with JMX observer self-stressing..."
sleep "$DURATION"

kill "$JMX_OBS_PID" "$NATIVE_OBS_PID" "$OS_OBS_PID" 2>/dev/null || true
kill -TERM "$TARGET_PID" 2>/dev/null || true
sleep 2
wait 2>/dev/null || true

echo "Done. Check:"
echo "  $OUTDIR/jmx_observer.log            -> count OBSERVER_OVERRUN lines, look at OBSERVER_SELF heap growth"
echo "  $OUTDIR/native_observer_control.log -> should look like a normal Test 1 NORMAL-phase run"
echo "  $OUTDIR/os_observer_control.log     -> should look like a normal Test 1 NORMAL-phase run"
echo ""
grep -c "OBSERVER_OVERRUN" "$OUTDIR/jmx_observer.log" | xargs -I{} echo "OBSERVER_OVERRUN count: {}"
