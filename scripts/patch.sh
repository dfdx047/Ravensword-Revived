#!/usr/bin/env bash
# Ravensword: Shadowlands 1.26 mod for the AYN Odin 3: patches YOUR OWN copy of the game.
#
#   scripts/patch.sh <ravensword-1.26.apk> [options]
#
#   --out FILE           output APK (default: output/ravensword-1.26-odin3-mod.apk)
#   --keystore FILE      signing key to use or create (default: keystore/patcher.jks)
#   --default-aa N       anti-aliasing the game starts with: 0, 2, 4 or 8 (default 4)
#
# This script never downloads the game: it only reads the APK you give it, and checks that it is
# the exact 1.26 release the mod was made for. It needs: python3 with UnityPy and fontTools (or uv,
# which fetches them), Java (keytool) and the Android build-tools (zipalign, apksigner).
# The signing key is made on first use and kept: the same key lets you install later versions
# over this one without losing your saves.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHA256=bbdbab91f302b278458e443801f050582c0c5642b8065a73d07e71844daf71a9
PREBUILT="$ROOT/prebuilt"
OUT="$ROOT/output/ravensword-1.26-odin3-mod.apk"
KS="$ROOT/keystore/patcher.jks"
KSPASS=""
ALIAS=ravensword-mod
AA=4
DEBUGGABLE=""
APK=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --keystore) KS="$2"; shift 2 ;;
    --keystore-pass) KSPASS="$2"; shift 2 ;;   # file with the password; default: <keystore>.pass
    --alias) ALIAS="$2"; shift 2 ;;
    --default-aa) AA="$2"; shift 2 ;;
    --prebuilt) PREBUILT="$2"; shift 2 ;;      # development builds (scripts/build.sh)
    --debuggable) DEBUGGABLE=--debuggable; shift ;;
    -h|--help) sed -n 2,15p "$0"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) APK="$1"; shift ;;
  esac
done
[ -n "$APK" ] || { sed -n 2,15p "$0" >&2; exit 2; }
[ -n "$KSPASS" ] || KSPASS="${KS%.jks}.pass"

fail() { echo "error: $*" >&2; exit 1; }

# --- tools
need() { command -v "$1" >/dev/null 2>&1 || fail "$1 not found. $2"; }
need python3 "Install Python 3."
need unzip "Install unzip."
need zip "Install zip."
need sha256sum "Install coreutils."
need keytool "Install a Java JDK (for keytool and apksigner)."
BT=""
for d in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Android/Sdk"; do
  [ -n "$d" ] && [ -d "$d/build-tools" ] && BT="$(ls -d "$d"/build-tools/* | sort -V | tail -1)" && break
done
ZIPALIGN="${BT:+$BT/}zipalign"
APKSIGNER="${BT:+$BT/}apksigner"
command -v "$ZIPALIGN" >/dev/null 2>&1 || fail "zipalign not found. Install the Android SDK build-tools, or set ANDROID_HOME."
command -v "$APKSIGNER" >/dev/null 2>&1 || fail "apksigner not found. Install the Android SDK build-tools, or set ANDROID_HOME."
PY=(python3 -I)
if ! python3 -c "import UnityPy, fontTools" 2>/dev/null; then
  command -v uv >/dev/null 2>&1 || fail "Python packages missing: pip install UnityPy==1.25.4 fonttools==4.68.0 (or install uv)."
  PY=(uv run --quiet --no-project --with UnityPy==1.25.4 --with fonttools==4.68.0 python -I)
fi
for f in arm64-v8a/libunity.so arm64-v8a/libmono.so arm64-v8a/lib-ouya-ndk.so RavenMod.dll PatchAssemblies; do
  [ -f "$PREBUILT/$f" ] || fail "$PREBUILT/$f missing. Use the release zip, or build it with scripts/build_prebuilt.sh."
done

# --- the user's APK
[ -f "$APK" ] || fail "$APK not found"
echo "== checking $APK"
GOT="$(sha256sum "$APK" | cut -d' ' -f1)"
[ "$GOT" = "$SHA256" ] || fail "this is not the Ravensword: Shadowlands 1.26 APK the mod was made for.
  expected SHA-256 $SHA256
  got              $GOT"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ravensword-patch.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/stage"
DATA="assets/bin/Data"
mkdir -p "$STAGE/$DATA/Managed" "$STAGE/lib/arm64-v8a" "$WORK/orig"

echo "== extracting the files to patch"
unzip -q -o "$APK" "$DATA/mainData" "$DATA/settings.xml" "$DATA/Managed/*" AndroidManifest.xml classes.dex "assets/libs/armeabi-v7a/*" -d "$WORK/orig"

echo "== classes.dex and manifest"
"${PY[@]}" "$ROOT/scripts/patch_dex.py" "$WORK/orig/classes.dex" "$STAGE/classes.dex"
"${PY[@]}" "$ROOT/scripts/patch_manifest.py" "$WORK/orig/AndroidManifest.xml" "$STAGE/AndroidManifest.xml" $DEBUGGABLE

echo "== native libraries"
cp "$PREBUILT"/arm64-v8a/*.so "$STAGE/lib/arm64-v8a/"
# The game's own 32-bit libraries move to the 64-bit folder as data that the bridge runs.
cp "$WORK/orig/assets/libs/armeabi-v7a/libmono.so" "$STAGE/lib/arm64-v8a/libravenguestmono.so"
cp "$WORK/orig/assets/libs/armeabi-v7a/libunity.so" "$STAGE/lib/arm64-v8a/libravenguestunity.so"

echo "== Unity data"
"${PY[@]}" "$ROOT/scripts/patch_unity_data.py" "$WORK/orig/$DATA/mainData" "$WORK/orig/$DATA/settings.xml" "$STAGE/$DATA" --default-aa "$AA"

echo "== font with accents, from the game's own font"
"${PY[@]}" "$ROOT/scripts/patch_font.py" extract "$APK" "$WORK/narkisim.ttf"
"${PY[@]}" "$ROOT/scripts/make_font.py" "$WORK/narkisim.ttf" "$WORK/narkisim-pt.ttf"
"${PY[@]}" "$ROOT/scripts/patch_font.py" "$APK" "$WORK/narkisim-pt.ttf" "$STAGE/$DATA"

echo "== Portuguese text, matched against the game's English"
"${PY[@]}" "$ROOT/scripts/lang_hash.py" unpack "$APK" "$ROOT/lang/pt-BR.hashed.json" "$WORK/pt-BR.json"
"${PY[@]}" "$ROOT/scripts/gen_mod_data.py" words "$WORK/pt-BR.json" "$WORK/words.txt"

echo "== game assemblies"
chmod +x "$PREBUILT/PatchAssemblies"
"$PREBUILT/PatchAssemblies" "$WORK/orig/$DATA/Managed" "$STAGE/$DATA/Managed" "$PREBUILT/RavenMod.dll" "$WORK/words.txt"

echo "== assembling"
cp "$APK" "$WORK/unsigned.apk"
chmod u+w "$WORK/unsigned.apk"
zip -qd "$WORK/unsigned.apk" 'META-INF/*' 'lib/armeabi-v7a/*' 'assets/libs/*'
(cd "$STAGE" && find . -type f | sed 's|^\./||' | zip -q -X -9 "$WORK/unsigned.apk" -@)
"$ZIPALIGN" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"

echo "== signing"
if [ ! -f "$KS" ]; then
  mkdir -p "$(dirname "$KS")"
  umask 077
  python3 -c "import secrets; print(secrets.token_urlsafe(24))" > "$KSPASS"
  keytool -genkeypair -keystore "$KS" -storepass:file "$KSPASS" -keypass:file "$KSPASS" -alias "$ALIAS" \
    -keyalg RSA -keysize 3072 -validity 36500 -dname "CN=Ravensword Mod (local)" >/dev/null 2>&1
  umask 022
  echo "new signing key: $KS (keep it to install updates over this version)"
fi
[ -f "$KSPASS" ] || fail "password file for $KS not found: $KSPASS"
mkdir -p "$(dirname "$OUT")"
"$APKSIGNER" sign --ks "$KS" --ks-pass "file:$KSPASS" --ks-key-alias "$ALIAS" \
  --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
  --out "$OUT" "$WORK/aligned.apk" 2>&1 | grep -v '^WARNING' || true
rm -f "$OUT.idsig"

echo "== verifying"
"$ZIPALIGN" -c -p 4 "$OUT"
CHECK="$("$APKSIGNER" verify "$OUT" 2>&1)" || { echo "$CHECK" >&2; fail "signature check failed"; }
sha256sum "$OUT"
echo "done: $OUT"
echo "install: adb install $OUT   (uninstall other versions of the game first; it erases their saves)"
