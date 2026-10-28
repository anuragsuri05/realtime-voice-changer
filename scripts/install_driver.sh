#!/bin/bash
# Installs the VoiceShift Virtual Mic driver and restarts CoreAudio.
# Usage: sudo ./install_driver.sh [path/to/VoiceShiftVirtualMic.driver]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-}"
if [[ -z "$SRC" ]]; then
    for c in "$HERE/VoiceShiftVirtualMic.driver" "$HERE/../build/VoiceShiftVirtualMic.driver"; do
        [[ -d "$c" ]] && SRC="$c" && break
    done
fi
[[ -d "$SRC" ]] || { echo "Driver bundle not found. Run scripts/build.sh first." >&2; exit 1; }
if [[ $EUID -ne 0 ]]; then echo "Run with sudo." >&2; exit 1; fi

DEST=/Library/Audio/Plug-Ins/HAL
mkdir -p "$DEST"
NAME="$(basename "$SRC")"
rm -rf "$DEST/$NAME"
cp -R "$SRC" "$DEST/"
chown -R root:wheel "$DEST/$NAME"
chmod -R 755 "$DEST/$NAME"

# Restart the audio server so it loads the plug-in (audio drops for ~1 s).
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod
echo "Installed $NAME."
