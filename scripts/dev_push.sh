#!/usr/bin/env bash
# Development only: rebuild the bridge and replace it in the debuggable app, then relaunch.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG=com.crescentmoongames.ravensword2
A=(adb)  # set ANDROID_SERIAL to pick a device
cmake --build "$ROOT/build/bridge-dev" -j"$(nproc)" | tail -1
"${A[@]}" push "$ROOT/build/bridge-dev/libunity.so" /data/local/tmp/raven_libunity.so >/dev/null
"${A[@]}" shell "chmod 644 /data/local/tmp/raven_libunity.so"
# Copied then renamed: overwriting a library the running game has mapped would crash it.
"${A[@]}" shell "run-as $PKG sh -c 'mkdir -p files/dev; cp /data/local/tmp/raven_libunity.so files/dev/libunity.so.new; mv files/dev/libunity.so.new files/dev/libunity.so'"
