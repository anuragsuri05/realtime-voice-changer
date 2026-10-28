// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "VoiceShift",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "VoiceShift", targets: ["VoiceShift"]),
        .executable(name: "vscheck", targets: ["vscheck"]),
    ],
    targets: [
        // Real-time core: DSP chain + CoreAudio (AUHAL) I/O. Plain C so nothing on the
        // audio thread allocates, locks, or touches the Swift/ObjC runtime.
        .target(
            name: "VoiceCore",
            path: "Sources/VoiceCore",
            cSettings: [.unsafeFlags(["-O3", "-Wall", "-Wno-unused-function"])],
            linkerSettings: [
                .linkedFramework("CoreAudio"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("Accelerate"),
                .linkedFramework("CoreFoundation"),
            ]
        ),
        .executableTarget(
            name: "VoiceShift",
            dependencies: ["VoiceCore"],
            path: "Sources/VoiceShift",
            linkerSettings: [
                .linkedFramework("IOBluetooth"),
                .linkedFramework("AVFoundation"),
                .linkedFramework("Carbon"),
                .linkedFramework("CoreAudio"),
            ]
        ),
        // Offline DSP verification: synthesises a male-range voice, runs the chain,
        // checks pitch/formant/latency/noise numbers and writes WAVs to listen to.
        .executableTarget(
            name: "vscheck",
            dependencies: ["VoiceCore"],
            path: "Sources/vscheck",
            cSettings: [.unsafeFlags(["-O3"])]
        ),
    ]
)
