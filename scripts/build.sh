#!/usr/bin/env bash
# Development build from source: compiles prebuilt/ and patches the APK with it.
#   scripts/build.sh [--apk PATH] [--name NAME] [--default-aa N] [--dev]
# --apk: the original 1.26 APK (default original/ravensword-1.26-original.apk).
# --dev: debuggable APK whose bridge can be replaced without reinstalling (scripts/dev_push.sh).
# Signs with keystore/ravensword-mod.jks when it exists, otherwise with a key made by patch.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="$ROOT/original/ravensword-1.26-original.apk"
NAME="ravensword-1.26-odin3-mod"
AA=4
DEV=0
while [ $# -gt 0 ]; do
  case "$1" in
    --apk) APK="$2"; shift 2 ;;
    --name) NAME="$2"; shift 2 ;;
    --default-aa) AA="$2"; shift 2 ;;
    --dev) DEV=1; NAME="$NAME-dev"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

PRE="$ROOT/build/prebuilt$([ "$DEV" = 1 ] && echo -dev || true)"
"$ROOT/scripts/build_prebuilt.sh" "$APK" --out "$PRE" $([ "$DEV" = 1 ] && echo --dev)

SIGN=()
if [ -f "$ROOT/keystore/ravensword-mod.jks" ]; then
  SIGN=(--keystore "$ROOT/keystore/ravensword-mod.jks" --keystore-pass "$ROOT/keystore/password.txt" --alias ravensword-mod)
fi
"$ROOT/scripts/patch.sh" "$APK" --prebuilt "$PRE" --out "$ROOT/output/$NAME.apk" --default-aa "$AA" \
  "${SIGN[@]}" $([ "$DEV" = 1 ] && echo --debuggable)
