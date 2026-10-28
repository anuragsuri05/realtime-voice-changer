import AppKit
import Foundation

/// Installs the bundled virtual-mic driver into /Library/Audio/Plug-Ins/HAL
/// via the standard macOS administrator password prompt.
enum DriverInstaller {
    struct InstallError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    static func install(completion: @escaping @MainActor (Result<Void, Error>) -> Void) {
        guard let res = Bundle.main.resourceURL else {
            Task { @MainActor in completion(.failure(InstallError(message: "App resources not found."))) }
            return
        }
        let driver = res.appendingPathComponent("VoiceShiftVirtualMic.driver").path
        let script = res.appendingPathComponent("install_driver.sh").path
        guard FileManager.default.fileExists(atPath: driver), FileManager.default.fileExists(atPath: script) else {
            Task { @MainActor in
                completion(.failure(InstallError(message:
                    "Driver not bundled. Build with scripts/build.sh, or run: sudo scripts/install_driver.sh")))
            }
            return
        }
        DispatchQueue.global(qos: .userInitiated).async {
            // The privileged shell may be denied access to ~/Downloads, ~/Desktop, ~/Documents
            // (privacy protection), so hand it copies in a temporary folder instead.
            let staged: (script: String, driver: String)
            do {
                staged = try stage(script: script, driver: driver)
            } catch {
                Task { @MainActor in
                    completion(.failure(InstallError(message: "Could not prepare installer: \(error.localizedDescription)")))
                }
                return
            }
            defer { try? FileManager.default.removeItem(atPath: (staged.script as NSString).deletingLastPathComponent) }
            let source = "do shell script \"/bin/bash \" & quoted form of \"\(escape(staged.script))\" & \" \" & " +
                "quoted form of \"\(escape(staged.driver))\" with administrator privileges"
            var err: NSDictionary?
            NSAppleScript(source: source)?.executeAndReturnError(&err)
            let result: Result<Void, Error>
            if let err {
                let msg = err[NSAppleScript.errorMessage] as? String ?? "Installation was cancelled."
                result = .failure(InstallError(message: "Virtual Mic install failed: \(msg)"))
            } else {
                result = .success(())
            }
            Task { @MainActor in completion(result) }
        }
    }

    private static func stage(script: String, driver: String) throws -> (script: String, driver: String) {
        let fm = FileManager.default
        let dir = fm.temporaryDirectory.appendingPathComponent("VoiceShiftInstall-\(UUID().uuidString)")
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let s = dir.appendingPathComponent("install_driver.sh")
        let d = dir.appendingPathComponent("VoiceShiftVirtualMic.driver")
        try fm.copyItem(atPath: script, toPath: s.path)
        try fm.copyItem(atPath: driver, toPath: d.path)
        return (s.path, d.path)
    }

    private static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
    }
}
