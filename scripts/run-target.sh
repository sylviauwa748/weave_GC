#!/usr/bin/env bash
# Weave Stage 0 - launch the Target JVM with remote JMX enabled.
#
# JMX is configured entirely via JVM flags here, NOT inside Target.java.
# That is deliberate: what the JMX observer can see should be exactly what
# the standard platform MBeans expose under a normal remote-JMX config,
# nothing custom-built.
#
# WARNING: authenticate=false / ssl=false is intentionally insecure and is
# only appropriate for this local experiment on your own machine.
#
# Usage:
#   scripts/run-target.sh [jmx_port]
set -euo pipefail
cd "$(dirname "$0")/.."

JMX_PORT="${1:-9010}"
JAR="weave-target/target/weave-target.jar"

if [[ ! -f "$JAR" ]]; then
  echo "Target jar not found at $JAR - run scripts/build-all.sh first." >&2
  exit 1
fi

exec java \
  -Dcom.sun.management.jmxremote \
  -Dcom.sun.management.jmxremote.port="$JMX_PORT" \
  -Dcom.sun.management.jmxremote.rmi.port="$JMX_PORT" \
  -Dcom.sun.management.jmxremote.authenticate=false \
  -Dcom.sun.management.jmxremote.ssl=false \
  -Dcom.sun.management.jmxremote.local.only=false \
  -Djava.rmi.server.hostname=127.0.0.1 \
  -jar "$JAR"
