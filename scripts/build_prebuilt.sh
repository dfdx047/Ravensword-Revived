#!/usr/bin/env bash
# Builds the parts of the mod that are compiled code, into prebuilt/ (what the release zip ships):
#   prebuilt/arm64-v8a/   the ARM32-on-ARM64 bridge (libunity.so) and two empty stand-in libraries
#   prebuilt/RavenMod.dll the in-game settings, controls and translation code
#   prebuilt/PatchAssemblies  the IL patcher, self-contained for linux-x64
#
#   scripts/build_prebuilt.sh <original 1.26 APK> [--out DIR] [--dev]
#
# Needs the Android NDK and CMake/Ninja; installs a local .NET SDK into tools/ on first use.
# The APK is only read: the bridge's GL table comes from the game's imports, and RavenMod.dll is
# compiled against the game's own 2.0-profile assemblies. Neither is copied into the output.
# --dev: the bridge can be replaced in a debuggable install without reinstalling (dev_push.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK=""
OUT="$ROOT/prebuilt"
DEV=0
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --dev) DEV=1; shift ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) APK="$1"; shift ;;
  esac
done
[ -n "$APK" ] || { sed -n 2,12p "$0" >&2; exit 2; }

SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK="${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}"
HOST="$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)"
READELF="$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-readelf"
SYSROOT_INC="$NDK/toolchains/llvm/prebuilt/$HOST/sysroot/usr/include"
CMAKE_ANDROID=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a
  -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release)
WORK="$ROOT/build/prebuilt-work"
DATA="assets/bin/Data"

export DOTNET_ROOT="$ROOT/tools/dotnet" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
export PATH="$DOTNET_ROOT:$PATH"
if [ ! -x "$DOTNET_ROOT/dotnet" ]; then
  mkdir -p "$ROOT/tools"
  curl -sSL -o "$ROOT/tools/dotnet-install.sh" https://dot.net/v1/dotnet-install.sh
  bash "$ROOT/tools/dotnet-install.sh" --channel 10.0 --install-dir "$DOTNET_ROOT" --no-path
fi

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT/arm64-v8a"
unzip -q -o "$APK" "$DATA/Managed/*" "assets/libs/armeabi-v7a/libunity.so" -d "$WORK"

echo "== dynarmic"
DYN="$ROOT/third_party/dynarmic"
[ -f "$DYN/CMakeLists.txt" ] || git -C "$ROOT" submodule update --init third_party/dynarmic
git -C "$DYN" submodule update --init --depth 1 externals/mcl externals/oaknut externals/fmt externals/robin-map
BOOST="$ROOT/third_party/boost/boost_1_88_0"
if [ ! -d "$BOOST" ]; then
  mkdir -p "$ROOT/third_party/boost"
  curl -sSL https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.bz2 | tar -xj -C "$ROOT/third_party/boost" --wildcards 'boost_1_88_0/boost/*'
fi
cmake -S "$DYN" -B "$ROOT/build/dynarmic" "${CMAKE_ANDROID[@]}" -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_TESTS=OFF \
  -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF \
  -DBoost_INCLUDE_DIR="$BOOST" -DCMAKE_CXX_FLAGS="-I$BOOST -fPIC" -DBUILD_TESTING=OFF >/dev/null 2>&1
cmake --build "$ROOT/build/dynarmic" -j"$(nproc)" | tail -1

echo "== bridge"
"$READELF" --dyn-syms "$WORK/assets/libs/armeabi-v7a/libunity.so" | awk '$7=="UND"{print $8}' | sed 's/@.*//' | sort -u | grep -v '^$' > "$WORK/unity.imports"
mkdir -p "$ROOT/bridge/gen"
python3 -I "$ROOT/scripts/gen_gl_table.py" "$WORK/unity.imports" "$SYSROOT_INC" "$ROOT/bridge/gen/gl_table.inc"
BRIDGE_BUILD="$ROOT/build/bridge$([ "$DEV" = 1 ] && echo -dev || true)"
cmake -S "$ROOT/bridge" -B "$BRIDGE_BUILD" "${CMAKE_ANDROID[@]}" -DRAVEN_DEV=$([ "$DEV" = 1 ] && echo ON || echo OFF) >/dev/null
cmake --build "$BRIDGE_BUILD" -j"$(nproc)" | tail -1
cp "$BRIDGE_BUILD/libunity.so" "$BRIDGE_BUILD/libmono.so" "$BRIDGE_BUILD/lib-ouya-ndk.so" "$OUT/arm64-v8a/"

echo "== RavenMod.dll"
MG="$WORK/$DATA/Managed"
CSC="$(ls -d "$DOTNET_ROOT"/sdk/*/Roslyn/bincore/csc.dll | sort -V | tail -1)"
python3 -I "$ROOT/scripts/gen_mod_data.py" cs "$ROOT/lang/padmap.txt" "$ROOT/mod/ModData.cs"
dotnet "$CSC" -nologo -noconfig -nostdlib -target:library -langversion:7.3 -optimize -nowarn:0436 -out:"$OUT/RavenMod.dll" \
  -r:"$MG/mscorlib.dll" -r:"$MG/System.dll" -r:"$MG/System.Core.dll" -r:"$MG/UnityEngine.dll" -r:"$MG/Assembly-CSharp-firstpass.dll" \
  "$ROOT/mod/RavenMod.cs" "$ROOT/mod/ModData.cs"

echo "== PatchAssemblies"
dotnet publish "$ROOT/scripts/patch_assemblies/PatchAssemblies.csproj" -c Release -r linux-x64 --self-contained \
  -p:PublishSingleFile=true -p:PublishTrimmed=true -p:TrimMode=partial -p:DebugType=none -v quiet --nologo \
  -o "$WORK/publish" -p:BaseIntermediateOutputPath="$ROOT/build/patcher-obj/" -p:BaseOutputPath="$ROOT/build/patcher-bin/" | tail -2
cp "$WORK/publish/PatchAssemblies" "$OUT/PatchAssemblies"

rm -rf "$WORK"
echo "== prebuilt ready in $OUT"
ls -la "$OUT" "$OUT/arm64-v8a"
