#!/usr/bin/env bash
# Weave Stage 0 - build everything.
# Requires: JDK 21+, Maven, a C compiler (Xcode Command Line Tools on macOS).
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== Building Target JVM =="
(cd weave-target && mvn -q -DskipTests package)

echo "== Building JMX Observer =="
(cd observer-jvm && mvn -q -DskipTests package)

echo "== Building Native Observer =="
(cd observer-native && make)

echo "== Building OS-level Observer =="
(cd observer-os && make)

echo "== Build complete =="
echo "Target jar:       weave-target/target/weave-target.jar"
echo "JMX observer jar: observer-jvm/target/weave-observer-jvm.jar"
echo "Native observer:  observer-native/native_observer"
echo "OS observer:      observer-os/os_observer"
