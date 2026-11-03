#!/usr/bin/env bash
# Starts the game on the Odin3 and prints the relevant logcat lines after N seconds (default 20).
A=(adb)  # set ANDROID_SERIAL to pick a device
PKG=com.crescentmoongames.ravensword2
T="${1:-20}"
"${A[@]}" shell am force-stop $PKG
"${A[@]}" logcat -c
"${A[@]}" shell am start -n $PKG/.OuyaUnityApplication >/dev/null
sleep "$T"
PID=$("${A[@]}" shell pidof $PKG | tr -d '\r')
echo "pid=${PID:-dead}"
"${A[@]}" logcat -d -v brief | grep -E '^./(RavenBridge|RavenStdout|Unity|Mono|mono|AndroidRuntime|DEBUG|libc\b|tombstoned|crash_dump[0-9]*|linker)' | grep -vE 'ctl.interface_start|libc *\( *[0-9]+\): Unable to set' 
