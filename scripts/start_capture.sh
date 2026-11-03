#!/usr/bin/env bash
# Development: installs the dev APK if given --install, launches the game and keeps a filtered
# log in logs/live.log, safe from the rest of the system's log traffic.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
A=(adb)  # set ANDROID_SERIAL to pick a device
PKG=com.crescentmoongames.ravensword2
PIDFILE="$ROOT/logs/capture.pid"

if [ -f "$PIDFILE" ]; then kill "$(cat "$PIDFILE")" 2>/dev/null; rm -f "$PIDFILE"; fi
if [ "${1:-}" = "--install" ]; then
  "${A[@]}" install -r "$ROOT/output/ravensword-1.26-odin3-mod-dev.apk" | tail -1
  "$ROOT/scripts/dev_push.sh" | tail -1 | cut -c1-80
fi
"${A[@]}" shell am force-stop "$PKG"
"${A[@]}" logcat -c
nohup "${A[@]}" logcat -v time RavenBridge:V Unity:V mono:V DEBUG:F AndroidRuntime:E libc:F '*:S' > "$ROOT/logs/live.log" 2>&1 &
echo $! > "$PIDFILE"
"${A[@]}" shell am start -n "$PKG/.OuyaUnityApplication" > /dev/null
