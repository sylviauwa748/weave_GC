#!/usr/bin/env bash
# Weave Stage 0 - Test 2: Target failure.
#
# Starts Target + all observers, lets them run briefly, then kills the
# Target either gracefully (SIGTERM, runs the shutdown hook) or abruptly
# (SIGKILL, no shutdown hook, no chance for the JVM to clean up JMX/RMI).
# The interesting result is HOW EACH OBSERVER'S LOG DIFFERS between the two
# modes, and how long each observer takes to notice.
#
# Usage:
#   scripts/test2_target_failure.sh graceful|abrupt [warmup_seconds] [jmx_port]
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:?usage: test2_target_failure.sh graceful|abrupt [warmup_seconds] [jmx_port]}"
WARMUP="${2:-20}"
JMX_PORT="${3:-9010}"

if [[ "$MODE" != "graceful" && "$MODE" != "abrupt" ]]; then
  echo "mode must be 'graceful' or 'abrupt'" >&2
  exit 1
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="results/test2-$MODE-$STAMP"
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

echo "Warming up for ${WARMUP}s before killing the Target ($MODE)..."
sleep "$WARMUP"

KILL_WALL_TIME="$(date -u +%Y-%m-%dT%H:%M:%S.000Z)"
echo "KILL_EVENT mode=$MODE wallTime=$KILL_WALL_TIME targetPid=$TARGET_PID" | tee "$OUTDIR/kill_event.log"

if [[ "$MODE" == "graceful" ]]; then
  kill -TERM "$TARGET_PID"
else
  kill -KILL "$TARGET_PID"
fi

# Let observers run a bit longer so we can see (or fail to see) how each one
# reacts to the Target's disappearance.
sleep 15

kill "$JMX_OBS_PID" "$NATIVE_OBS_PID" "$OS_OBS_PID" 2>/dev/null || true
wait 2>/dev/null || true

echo "Done. Compare detection latency across:"
echo "  $OUTDIR/jmx_observer.log     (look for OBSERVER_DISCONNECTED)"
echo "  $OUTDIR/native_observer.log  (look for NATIVE_OBSERVER_TARGET_GONE)"
echo "  $OUTDIR/os_observer.log      (look for OS_OBSERVER_TARGET_GONE / OS_SAMPLE_PROC_STATE_EMPTY)"
echo "  $OUTDIR/target.log           (TARGET_SHUTDOWN_HOOK present only in graceful mode)"
