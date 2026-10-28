# VoiceShift — real-time female voice for macOS

VoiceShift converts your microphone into a natural-sounding female voice in real time
and routes it into calls, either straight to your phone as a Bluetooth headset or
through a virtual microphone that any call app can use.

- **One-button ON/OFF** in the window, the menu bar, or **⌥⌘V** from any app. OFF passes
  your natural voice through, so a call never goes silent while you toggle.
- **Voice controls**: presets, pitch, formant (vocal-tract size), intonation, breathiness,
  brightness, mic/output gain.
- **Noise suppression** (spectral, adjustable 6–30 dB) and a noise gate.
- **Low-latency engine**: ≈40 ms of DSP (≈30 ms in "Lowest latency" mode) plus device buffers,
  at about 1.5 % of one CPU core.
- **Bluetooth headset mode**: the Mac acts as a Hands-Free (HFP) headset for your phone.
- **Fallback virtual audio device**: *VoiceShift Virtual Mic*, selectable as the microphone in
  FaceTime, WhatsApp, Zoom, Meet, Discord, Teams, browsers and so on.

## Build and run

You only need the Xcode Command Line Tools (`xcode-select --install`).

```bash
./scripts/build.sh                 # → build/VoiceShift.app (driver embedded)
open build/VoiceShift.app
```

In the app, click **Install Virtual Mic**. It asks for your admin password and restarts
CoreAudio, so sound cuts out for about a second. To do the same from the terminal, run
`sudo scripts/install_driver.sh`.

The build is ad-hoc signed. If Gatekeeper complains, right-click the app → **Open** the first time.

## Getting the converted voice into phone calls

macOS limits what third-party apps can do with Bluetooth audio, so VoiceShift offers several
routes. In **Automatic** mode it picks the best available one and tells you which it used.

| Route | Works with | Reliability |
|---|---|---|
| **1. iPhone call relay (recommended for iPhone)**: iPhone → Settings → Phone → *Calls on Other Devices* → enable this Mac. Place or answer calls on the Mac (FaceTime app). In FaceTime's **Video** menu, choose **Microphone → VoiceShift Virtual Mic**. | Normal cellular calls from an iPhone | ✅ High |
| **2. Call apps on the Mac**: choose **VoiceShift Virtual Mic** as the microphone in WhatsApp, Zoom, Meet, Discord, Teams, Skype, a browser… | Any phone or contact, over the internet | ✅ High |
| **3. Bluetooth headset mode** (experimental): pair the phone with the Mac, select it in *Bluetooth headset mode*, then click **Connect as headset**. During a call the converted voice goes to the phone and the caller plays on your Mac. | Mostly Android; iPhones usually refuse a Mac as a hands-free headset | ⚠️ Depends on the phone, macOS version and Bluetooth chip |
| **4. Cable**: Mac headphone out → TRRS/USB-C audio adapter → the phone's mic input. Set "Send voice to → Specific device". | Any phone | ✅ High (needs an adapter) |

**How the Bluetooth fallback works:** VoiceShift checks each stage of the headset connection:
the HFP service connection, the SCO voice link, and the CoreAudio device macOS must create for
it. If any stage fails or times out, the app shows why and moves the voice to the
Virtual Mic automatically (routes 1 and 2).

## Controls

| Control | What it does | Typical male → female |
|---|---|---|
| Pitch | How high the voice is | +7 … +10 semitones |
| Formant | Apparent throat/mouth size, independent of pitch. This is what makes it sound like a woman rather than a sped-up man | +2.5 … +4 st |
| Intonation | Widens the pitch contour; female speech is usually more melodic | 110–130 % |
| Breathiness | Soft aspiration that follows your voice | 15–35 % |
| Brightness | High-frequency presence | 0 … +4 dB |
| Noise suppression / Strength | Removes fans, hum, room noise | 50–70 % |
| Gate | Hard silence between words | Off, or about −55 dBFS |
| Monitor | Hear yourself, ideally on headphones | — |
| Buffer / Lowest latency | Trade stability for latency | 128 / off |

Tips for the most natural result: speak a little softer and more melodically than usual, keep
your normal speaking speed, and start from the **Natural Female** preset.

## Latency

| Stage | Time |
|---|---|
| Noise suppressor (512-sample STFT @ 48 kHz) | 10.7 ms |
| Pitch/formant shifter (min. tracked pitch 75 Hz) | 29.2 ms |
| Ring buffer plus 128-frame device buffers | ≈ 7 ms |
| **VoiceShift total** | **≈ 47 ms** (≈ 36 ms with *Lowest latency*) |
| MacBook built-in mic array (Apple's beamforming DSP, reported by CoreAudio) | +50 ms |

The built-in MacBook microphone adds about 50 ms of its own processing. A wired or USB headset mic
avoids it. The app shows the real end-to-end figure under **Live → Latency**.

## Architecture

```
Sources/VoiceCore/      C, real-time safe (no allocation, locks or ObjC on the audio thread)
  chain.c               gain → 70 Hz HPF → noise suppression → gate → pitch/formant
                        → loudness match → brightness → breath → ON/OFF crossfade → limiter
  denoise.c             STFT Wiener suppressor, decision-directed SNR, min-tracking noise floor
  pitch.c               YIN pitch tracker + pitch-synchronous OLA (PSOLA) shifter; formant
                        shift by grain resampling, so pitch and formant are independent
  engine.c              AUHAL capture → DSP → lock-free rings → AUHAL outputs (main + monitor),
                        clock-drift correction, CPU/latency/underrun meters
Sources/VoiceShift/     SwiftUI app: routing logic, Bluetooth HFP (IOBluetoothHandsFreeDevice),
                        device management, driver installer, global hot key, menu bar
Driver/                 AudioServerPlugIn loopback device "VoiceShift Virtual Mic"
Sources/vscheck/        Offline DSP verification
```

When the Mac is the phone's headset, a second engine instance plays the caller's voice
(phone → Mac speakers/headphones) unprocessed.

## Testing

**Offline DSP checks** (no devices needed):

```bash
swift run -c release vscheck ./out    # writes WAVs to ./out so you can listen
```

This synthesises a male-range voice and checks output pitch accuracy, that formant shift is
independent of pitch, intonation tracking, the bypass delay, noise reduction (including after
digital silence at start-up), speech level and CPU use.

**End-to-end through the real CoreAudio server** (Virtual Mic must be installed):

```bash
scripts/e2e_test.sh          # installs a test-only "VoiceShift Test Source" device (sudo), then runs:
                             #   1. Virtual Mic loopback (bit-exact, no added delay, no dropouts)
                             #   2. test voice → engine → Virtual Mic → recorder acting as a call app
                             #   3. noise suppression on a fan-noise recording, on vs off
scripts/e2e_test.sh --app    # also drives the real VoiceShift.app the same way
scripts/e2e_test.sh --cleanup
```

The test-only device replaces the microphone with a digital loopback, so every run feeds in
the same signal and gives repeatable results.

**Launch options** (also useful as a login item): `--on` starts with the voice changer on;
`--input <device UID>` / `--output <device UID>` choose devices; `--diagnostics <file.json>`
writes the live state (permission, route, levels, pitch, latency, errors) twice a second.

## Limitations

- The conversion is DSP-based (PSOLA plus formant shifting), not a neural voice model. It is
  clean and low-latency, and it reads as a female voice, but a careful listener may hear that
  it is processed, especially on shouting, whispering or very deep voices. The chain is
  structured so a neural (for example CoreML) converter could replace the pitch stage later.
- Direct Bluetooth headset emulation relies on Apple's `IOBluetoothHandsFreeDevice`. Whether a
  given phone and macOS combination exposes the call audio cannot be guaranteed. That is why
  the virtual-mic fallback exists.
- Installing or removing the driver restarts CoreAudio. Audio in other apps cuts out for a moment.
  VoiceShift itself reconnects automatically. This was tested by removing a device mid-session.
- Locally built copies are ad-hoc signed, so macOS may ask for microphone permission again
  after you rebuild.

## Uninstall

```bash
sudo scripts/uninstall_driver.sh && rm -rf build/VoiceShift.app
```

## Responsible use

Use voice conversion for privacy, comfort, accessibility or creative purposes. Don't use it to
impersonate real people or to deceive someone in ways that cause harm. Some places require
consent before altering your voice on recorded or business calls.
