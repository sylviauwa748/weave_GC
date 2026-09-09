#!/usr/bin/env bash
# Weave Stage 0 - Test 1: Normal observation.
#
# Starts the Target and all three observers together, lets them run for a
# while, then stops everything cleanly. Look at the resulting logs to
# answer, for each level: what can it see, what's the real sampling
# resolution, what's missing?
#
# Usage:
#   scripts/test1_normal.sh [duration_seconds] [jmx_port]
set -euo pipefail
cd "$(dirname "$0")/.."

DURATION="${1:-90}"
JMX_PORT="${2:-9010}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="results/test1-normal-$STAMP"
mkdir -p "$OUTDIR"

echo "Logs will be written to $OUTDIR"

scripts/run-target.sh "$JMX_PORT" > "$OUTDIR/target.log" 2>&1 &
TARGET_PID=$!
echo "Target starting, pid=$TARGET_PID (will confirm actual JVM pid from its log)"
sleep 3   # let the JVM come up and open the JMX port before observers connect

java -jar observer-jvm/target/weave-observer-jvm.jar 127.0.0.1 "$JMX_PORT" 1000 \
  > "$OUTDIR/jmx_observer.log" 2>&1 &
JMX_OBS_PID=$!

observer-native/native_observer "$TARGET_PID" 1000 \
  > "$OUTDIR/native_observer.log" 2>&1 &
NATIVE_OBS_PID=$!

observer-os/os_observer "$TARGET_PID" 1000 \
  > "$OUTDIR/os_observer.log" 2>&1 &
OS_OBS_PID=$!

echo "Running for ${DURATION}s ..."
sleep "$DURATION"

echo "Stopping observers and target..."
kill "$JMX_OBS_PID" "$NATIVE_OBS_PID" "$OS_OBS_PID" 2>/dev/null || true
kill -TERM "$TARGET_PID" 2>/dev/null || true   # graceful stop
sleep 2
wait 2>/dev/null || true

echo "Done. Inspect:"
echo "  $OUTDIR/target.log"
echo "  $OUTDIR/jmx_observer.log"
echo "  $OUTDIR/native_observer.log"
echo "  $OUTDIR/os_observer.log"
