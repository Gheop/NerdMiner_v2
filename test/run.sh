#!/usr/bin/env bash
# Build and run every host test in this directory.
# These tests need a C++ compiler. They run on your computer, with no board.
#
#   ./test/run.sh
#
# One test parses pool JSON and needs the ArduinoJson headers. The script looks
# for them under .pio/libdeps/, which PlatformIO fills on the first build. Run
# `pio pkg install -e ESP32-devKitv1` if that test is skipped.
#
# Each test compiles to its own binary in a temporary directory and returns
# non-zero when the fixed implementation misbehaves.
set -u

cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--O0 -g -Wall -Wextra}
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

# ArduinoJson ships its public header next to a src/ directory that holds the
# implementation headers, so both paths are needed.
json_root=$(find .pio/libdeps -maxdepth 2 -type d -name ArduinoJson 2>/dev/null | head -1)
json_inc=""
if [ -n "$json_root" ]; then
    json_inc="-I $json_root -I $json_root/src"
fi

failed=0
skipped=0
for src in test/*_test.cpp; do
    name=$(basename "$src" .cpp)
    printf '\n=== %s ===\n' "$name"
    if grep -q "ArduinoJson.h" "$src" && [ -z "$json_inc" ]; then
        echo "SKIPPED: ArduinoJson headers not found under .pio/libdeps/"
        skipped=$((skipped + 1))
        continue
    fi
    if ! $CXX $CXXFLAGS $json_inc -o "$workdir/$name" "$src"; then
        echo "COMPILE FAILED: $src"
        failed=$((failed + 1))
        continue
    fi
    if ! "$workdir/$name"; then
        echo "FAILED: $name"
        failed=$((failed + 1))
    fi
done

printf '\n'
if [ "$failed" -ne 0 ]; then
    echo "$failed test(s) failed, $skipped skipped"
    exit 1
fi
echo "all tests passed, $skipped skipped"
