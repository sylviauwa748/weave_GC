#!/usr/bin/env bash
# Weave Stage 0 - Test 5: Target restart / continuity.
#
# Starts Target #1, observes it, terminates it gracefully, then starts a
# brand-new Target #2 (new PID, new JVM instance) and starts brand-new
# observer instances against it. We deliberately do NOT try to make any
# observer "follow" the restart automatically - Stage 0's baseline
# observers have no reconnect logic (see JmxObserver.java comments).
#
# What to look for across target1_*.log vs target2_*.log:
#   - GC collection counts in target2's JMX log START OVER FROM ZERO. They
#     are NOT continuous with target1's counts, even though both ran the
#     "same" program back to back. A counter like "GC count = 244" belongs
#     to one JVM lifetime, never to "the Target" as an abstract, ongoing
#     entity.
#   - The native/OS observers must be restarted with target2's NEW PID -
#     PIDs are not stable across restarts, and nothing here maps
#     "Target identity" for you automatically.
#
# Usage:
#   scripts/test5_restart_continuity.sh [observe_seconds] [jmx_port]
set -euo pipefail
cd "$(dirname "$0")/.."

OBSERVE_SECONDS="${1:-30}"
JMX_PORT="${2:-9010}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="results/test5-restart-$STAMP"
mkdir -p "$OUTDIR"
echo "Logs will be written to $OUTDIR"

### --- Target #1 lifetime ---
scripts/run-target.sh "$JMX_PORT" > "$OUTDIR/target1.log" 2>&1 &
TARGET1_PID=$!
sleep 3
echo "Target #1 pid=$TARGET1_PID"

java -jar observer-jvm/target/weave-observer-jvm.jar 127.0.0.1 "$JMX_PORT" 1000 \
  > "$OUTDIR/jmx_observer_target1.log" 2>&1 &
JMX1_PID=$!
observer-native/native_observer "$TARGET1_PID" 1000 \
  > "$OUTDIR/native_observer_target1.log" 2>&1 &
NATIVE1_PID=$!
observer-os/os_observer "$TARGET1_PID" 1000 \
  > "$OUTDIR/os_observer_target1.log" 2>&1 &
OS1_PID=$!

echo "Observing Target #1 for ${OBSERVE_SECONDS}s..."
sleep "$OBSERVE_SECONDS"

echo "Gracefully terminating Target #1 (pid=$TARGET1_PID)..."
kill -TERM "$TARGET1_PID"
sleep 3

echo "Stopping Target #1's observers (no auto-reconnect by design)..."
kill "$JMX1_PID" "$NATIVE1_PID" "$OS1_PID" 2>/dev/null || true
wait "$JMX1_PID" "$NATIVE1_PID" "$OS1_PID" 2>/dev/null || true

### --- Target #2 lifetime (fresh JVM, fresh PID) ---
sleep 2  # let the port fully free up
scripts/run-target.sh "$JMX_PORT" > "$OUTDIR/target2.log" 2>&1 &
TARGET2_PID=$!
sleep 3
echo "Target #2 pid=$TARGET2_PID (compare to Target #1 pid=$TARGET1_PID)"

java -jar observer-jvm/target/weave-observer-jvm.jar 127.0.0.1 "$JMX_PORT" 1000 \
  > "$OUTDIR/jmx_observer_target2.log" 2>&1 &
JMX2_PID=$!
observer-native/native_observer "$TARGET2_PID" 1000 \
  > "$OUTDIR/native_observer_target2.log" 2>&1 &
NATIVE2_PID=$!
observer-os/os_observer "$TARGET2_PID" 1000 \
  > "$OUTDIR/os_observer_target2.log" 2>&1 &
OS2_PID=$!

echo "Observing Target #2 for ${OBSERVE_SECONDS}s..."
sleep "$OBSERVE_SECONDS"

kill "$JMX2_PID" "$NATIVE2_PID" "$OS2_PID" 2>/dev/null || true
kill -TERM "$TARGET2_PID" 2>/dev/null || true
sleep 2
wait 2>/dev/null || true

echo "Done. Compare:"
echo "  $OUTDIR/jmx_observer_target1.log  vs  $OUTDIR/jmx_observer_target2.log"
echo "  (last GC counts of target1 vs first GC counts of target2 - continuity or reset?)"
echo "Target #1 pid was $TARGET1_PID, Target #2 pid was $TARGET2_PID"
