#!/bin/bash
# Builds VoiceShift.app (with the virtual-mic driver embedded) into ./build.
# Requires only the Xcode Command Line Tools.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="$ROOT/build"
APP="$OUT/VoiceShift.app"
DRV="$OUT/VoiceShiftVirtualMic.driver"

rm -rf "$APP" "$DRV"
mkdir -p "$OUT"

echo "==> Building virtual mic driver (universal)"
mkdir -p "$DRV/Contents/MacOS"
cp Driver/Info.plist "$DRV/Contents/Info.plist"
clang -bundle -O2 -Wall -arch x86_64 -arch arm64 -mmacosx-version-min=11.0 \
    -framework CoreAudio -framework CoreFoundation \
    -o "$DRV/Contents/MacOS/VoiceShiftVirtualMic" Driver/VoiceShiftVirtualMic.c
codesign --force --sign - "$DRV"

echo "==> Building app"
ARCHS=(--arch "$(uname -m)")
if [[ "${UNIVERSAL:-0}" == "1" ]]; then ARCHS=(--arch arm64 --arch x86_64); fi
swift build -c release --product VoiceShift "${ARCHS[@]}"
BIN="$(swift build -c release --product VoiceShift "${ARCHS[@]}" --show-bin-path)/VoiceShift"

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/VoiceShift"
cp Resources/Info.plist "$APP/Contents/Info.plist"
cp -R "$DRV" "$APP/Contents/Resources/"
cp scripts/install_driver.sh scripts/uninstall_driver.sh "$APP/Contents/Resources/"
codesign --force --deep --sign - "$APP"

echo
echo "Built: $APP"
echo "Run:   open \"$APP\""
