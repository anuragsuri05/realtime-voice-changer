#!/bin/bash
# Removes the VoiceShift Virtual Mic driver. Usage: sudo ./uninstall_driver.sh
set -euo pipefail
if [[ $EUID -ne 0 ]]; then echo "Run with sudo." >&2; exit 1; fi
rm -rf /Library/Audio/Plug-Ins/HAL/VoiceShiftVirtualMic.driver
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod
echo "Removed VoiceShift Virtual Mic."
