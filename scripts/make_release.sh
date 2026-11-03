#!/usr/bin/env bash
# Packs the patcher that users download: scripts, translation table and prebuilt/ code.
#   scripts/make_release.sh <original 1.26 APK> <version>
# The APK is only used to compile prebuilt/; nothing from it goes into the zip, which is checked.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="$1"
VERSION="$2"
NAME="ravensword-odin3-mod-patcher-$VERSION"
DIST="$ROOT/dist"
PKG="$DIST/$NAME"

"$ROOT/scripts/build_prebuilt.sh" "$APK" --out "$ROOT/prebuilt"

rm -rf "$PKG" "$DIST/$NAME.zip"
mkdir -p "$PKG/scripts" "$PKG/lang"
cp -r "$ROOT/prebuilt" "$PKG/prebuilt"
cp "$ROOT"/scripts/{patch.sh,patch_dex.py,patch_manifest.py,patch_unity_data.py,patch_font.py,make_font.py,lang_hash.py,gen_mod_data.py} "$PKG/scripts/"
cp "$ROOT/lang/pt-BR.hashed.json" "$ROOT/lang/padmap.txt" "$PKG/lang/"
cp "$ROOT"/{LICENSE,THIRD_PARTY_NOTICES.md,README.md,README.pt-BR.md,README.es.md,CHANGELOG.md} "$PKG/"
cp -r "$ROOT/licenses" "$PKG/licenses"
chmod +x "$PKG/scripts/patch.sh" "$PKG/prebuilt/PatchAssemblies"

# Nothing of the game: no APK, no Unity data, no original library or assembly, no font.
BAD="$(cd "$PKG" && find . -type f \( -name '*.apk' -o -name '*.ttf' -o -name '*.assets' -o -name 'mainData' \
  -o -name 'libravenguest*' -o -name 'Assembly-*.dll' -o -name 'UnityEngine.dll' -o -name '*.jks' \) | sort)"
[ -z "$BAD" ] || { echo "refusing to pack game or private files:" >&2; echo "$BAD" >&2; exit 1; }
GAME_LIB="$(cd "$ROOT" && unzip -p "$APK" assets/libs/armeabi-v7a/libunity.so | sha256sum | cut -d' ' -f1)"
for f in "$PKG"/prebuilt/arm64-v8a/*.so; do
  [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$GAME_LIB" ] || { echo "game library found: $f" >&2; exit 1; }
done

(cd "$DIST" && zip -q -r -X -9 "$NAME.zip" "$NAME")
rm -rf "$PKG"
sha256sum "$DIST/$NAME.zip" | tee "$DIST/$NAME.zip.sha256"
unzip -l "$DIST/$NAME.zip"
