#!/usr/bin/env bash
# Weave Stage 0 - Test 3: Target degradation.
#
# Runs long enough to cover at least two full NORMAL -> HIGH -> EXTREME
# cycles (each phase is ~400 ticks * 50ms nominal = ~20s, so one full cycle
# is roughly ~60s; this defaults to 240s = ~4 cycles). The Target's own log
# contains PHASE_BEGIN/PHASE_END markers with wall-clock timestamps - use
# those as ground truth to align what each observer saw against what was
# actually happening.
#
# Usage:
#   scripts/test3_degradation.sh [duration_seconds] [jmx_port]
set -euo pipefail
cd "$(dirname "$0")/.."

DURATION="${1:-240}"
JMX_PORT="${2:-9010}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="results/test3-degradation-$STAMP"
mkdir -p "$OUTDIR"
echo "Logs will be written to $OUTDIR"

scripts/run-target.sh "$JMX_PORT" > "$OUTDIR/target.log" 2>&1 &
TARGET_PID=$!
sleep 3

java -jar observer-jvm/target/weave-observer-jvm.jar 127.0.0.1 "$JMX_PORT" 500 \
  > "$OUTDIR/jmx_observer.log" 2>&1 &
JMX_OBS_PID=$!

observer-native/native_observer "$TARGET_PID" 500 \
  > "$OUTDIR/native_observer.log" 2>&1 &
NATIVE_OBS_PID=$!

observer-os/os_observer "$TARGET_PID" 500 \
  > "$OUTDIR/os_observer.log" 2>&1 &
OS_OBS_PID=$!

echo "Running for ${DURATION}s across NORMAL/HIGH/EXTREME phase cycles..."
sleep "$DURATION"

kill "$JMX_OBS_PID" "$NATIVE_OBS_PID" "$OS_OBS_PID" 2>/dev/null || true
kill -TERM "$TARGET_PID" 2>/dev/null || true
sleep 2
wait 2>/dev/null || true

echo "Done."
echo "Ground truth phase transitions:"
grep -E "PHASE_BEGIN|PHASE_END" "$OUTDIR/target.log" || true
echo ""
echo "Now, for each phase window, compare in the observer logs:"
echo "  jmx_observer.log     -> deltaCount / deltaTimeMs per collector, heapUsed trend"
echo "  native_observer.log  -> deltaUserNanos / deltaSysNanos, residentSize trend, faults"
echo "  os_observer.log      -> system-wide faults/pageins, process state changes"
