#!/bin/bash
# End-to-end test against the real CoreAudio server.
#
#   scripts/e2e_test.sh            build harness + test-source driver, run all stages
#   scripts/e2e_test.sh --app      also drive the real VoiceShift.app (asks for mic permission once)
#   scripts/e2e_test.sh --cleanup  remove the test-source driver (needs sudo)
#
# Requires the VoiceShift Virtual Mic to be installed. The "VoiceShift Test Source"
# loopback device stands in for a microphone so the whole path is digital and
# repeatable:  test voice → Test Source → VoiceShift → Virtual Mic → "call app" recorder.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$PWD/build/e2e"
TESTDRV="$OUT/VoiceShiftTestSource.driver"
HAL=/Library/Audio/Plug-Ins/HAL
mkdir -p "$OUT"

if [[ "${1:-}" == "--cleanup" ]]; then
    sudo rm -rf "$HAL/VoiceShiftTestSource.driver"
    sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall coreaudiod
    echo "Removed VoiceShift Test Source."
    exit 0
fi

echo "==> Building test-source driver"
rm -rf "$TESTDRV"
mkdir -p "$TESTDRV/Contents/MacOS"
sed -e 's/com.voiceshift.driver.VirtualMic/com.voiceshift.driver.TestSource/' \
    -e 's/<string>VoiceShiftVirtualMic<\/string>/<string>VoiceShiftTestSource<\/string>/g' \
    -e 's/6C1E3F0A-5B7D-4E2A-9C1D-7A8B9C0D1E2F/0F4D2B6E-1A3C-4E5F-8A9B-0C1D2E3F4A5B/g' \
    -e 's/VoiceShift_Create/VoiceShiftTest_Create/' Driver/Info.plist > "$TESTDRV/Contents/Info.plist"
clang -bundle -O2 -arch x86_64 -arch arm64 -mmacosx-version-min=11.0 \
    -DkDevice_Name='"VoiceShift Test Source"' -DkDevice_UID='"VoiceShiftTestSource_UID"' \
    -DkDevice_ModelUID='"VoiceShiftTestSource_Model"' -DkFactoryName=VoiceShiftTest_Create \
    -framework CoreAudio -framework CoreFoundation \
    -o "$TESTDRV/Contents/MacOS/VoiceShiftTestSource" Driver/VoiceShiftVirtualMic.c
codesign --force --sign - "$TESTDRV"

echo "==> Building harness + reference voices"
clang -O2 -I Sources/VoiceCore/include Sources/VoiceCore/*.c Tests/e2e/e2e.c \
    -framework Accelerate -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -o "$OUT/e2e"
swift build -c release --product vscheck >/dev/null
.build/release/vscheck "$OUT" >/dev/null || true

if [[ ! -d "$HAL/VoiceShiftVirtualMic.driver" ]]; then
    echo "VoiceShift Virtual Mic is not installed (sudo scripts/install_driver.sh)." >&2
    exit 1
fi
if [[ ! -d "$HAL/VoiceShiftTestSource.driver" ]]; then
    echo "==> Installing test-source driver (sudo)"
    sudo bash scripts/install_driver.sh "$TESTDRV"
    sleep 3
fi

echo; echo "=== 1. Virtual Mic loopback inside coreaudiod ==="
"$OUT/e2e" loopback
echo; echo "=== 2. VoiceCore engine: Test Source → engine → Virtual Mic → call app ==="
E2E_SOURCE_UID=VoiceShiftTestSource_UID "$OUT/e2e" engine "$OUT/3_glide_input.wav" "$OUT"

echo; echo "=== 3. Noise suppression end to end (fan noise + speech with pauses) ==="
python3 - "$OUT" <<'PY'
import wave, struct, math, random, sys
out = sys.argv[1]
w = wave.open(out + "/5_noisy_input.wav"); n = w.getnframes()
x = [v / 32768 for v in struct.unpack('<%dh' % n, w.readframes(n))]
random.seed(1); lp = 0; y = []
for i, v in enumerate(x):
    lp = 0.85 * lp + 0.15 * random.gauss(0, 1)
    y.append(v + 0.012 * lp + 0.002 * math.sin(2 * math.pi * 120 * i / 48000))
o = wave.open(out + "/noisy_room.wav", "wb"); o.setnchannels(1); o.setsampwidth(2); o.setframerate(48000)
o.writeframes(struct.pack('<%dh' % n, *[max(-32767, min(32767, int(v * 32767))) for v in y])); o.close()
PY
for ns in 0 1; do
    E2E_SOURCE_UID=VoiceShiftTestSource_UID E2E_NS=$ns "$OUT/e2e" engine "$OUT/noisy_room.wav" "$OUT" >/dev/null
    cp "$OUT/e2e_call_app_heard.wav" "$OUT/room_ns$ns.wav"
done
python3 - "$OUT" <<'PY'
import wave, struct, math, sys
W = 2400
def stats(p):
    w = wave.open(p); n = w.getnframes(); x = [v / 32768 for v in struct.unpack('<%dh' % n, w.readframes(n))]
    f = sorted(v for v in (10 * math.log10(sum(u * u for u in x[i:i + W]) / W + 1e-12) for i in range(0, n - W, W)) if v > -100)
    return f[len(f) // 10], f[len(f) * 9 // 10]
a = stats(sys.argv[1] + "/room_ns0.wav"); b = stats(sys.argv[1] + "/room_ns1.wav")
cut, loss = a[0] - b[0], a[1] - b[1]
ok = cut > 12 and abs(loss) < 2
print("  [%s] noise cut %.1f dB, speech level change %+.1f dB" % ("PASS" if ok else "FAIL", cut, -loss))
sys.exit(0 if ok else 1)
PY

if [[ "${1:-}" == "--app" ]]; then
    echo; echo "=== 4. Real VoiceShift.app: Test Source → app → Virtual Mic → call app ==="
    DIAG="$OUT/app_state.json"
    pkill -x VoiceShift 2>/dev/null || true; sleep 1; rm -f "$DIAG"
    open build/VoiceShift.app --args --on --input VoiceShiftTestSource_UID --diagnostics "$DIAG"
    echo "  (click Allow if macOS asks for microphone access)"
    for _ in $(seq 1 60); do grep -q '"engineRunning" : true' "$DIAG" 2>/dev/null && break; sleep 2; done
    grep -E '"(input|output|micPermission|error)"' "$DIAG" | sed 's/^/  /'
    E2E_SOURCE_UID=VoiceShiftTestSource_UID E2E_PLAY_WAV="$OUT/3_glide_input.wav" \
        "$OUT/e2e" record 4.5 "$OUT/app_heard.wav"
    "$OUT/e2e" analyze "$OUT/3_glide_input.wav" "$OUT/app_heard.wav"
    pkill -x VoiceShift || true
fi

echo; echo "Done. Remove the test device afterwards with: scripts/e2e_test.sh --cleanup"
