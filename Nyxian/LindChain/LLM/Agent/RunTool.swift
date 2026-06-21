/*
 SPDX-License-Identifier: AGPL-3.0-or-later

 Copyright (C) 2026 emexlab

 This file is part of Nyxian.

 Nyxian is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Nyxian is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with Nyxian. If not, see <https://www.gnu.org/licenses/>.
*/

// run() drives the in-app process subsystem (PEProcessManager) and the
// LiveProcess extension, which are only available in the jailed (non-jailbreak)
// build. It is also the one tool gated on the iOS 27 LiveProcess fix.
#if !JAILBREAK_ENV

import Foundation

/// Launches the built product, captures merged stdout+stderr, and enforces a
/// timeout, returning the output. Uses the same FDMapObject pipe-capture pattern
/// the build/run button uses (Builder.install), but drives PEProcessManager
/// directly so it can wait for exit and kill on timeout.
public struct RunTool: AgentTool, @unchecked Sendable {

    public let project: NXProject

    public init(project: NXProject) { self.project = project }

    public var spec: ToolSpec {
        ToolSpec(
            name: "run",
            description: "Run the built product, capturing its output, killing it after a timeout.",
            parameters: [
                "type": "object",
                "properties": [
                    "timeout_seconds": ["type": "integer",
                                        "description": "Max seconds to run before the process is killed. Default 30."]
                ],
                "required": []
            ]
        )
    }

    public func invoke(_ arguments: JSONValue) async throws -> ToolResult {
        let timeout = max(1, arguments.optionalInt("timeout_seconds") ?? 30)

        // Merge stdout+stderr into one pipe; keep both ends of both pipes mapped
        // (locs 100/101) so the child isn't torn down early — same trick Builder
        // uses for the IDE console.
        let outPipe = Pipe()
        let inPipe = Pipe()
        // FDMapObject.emptyMap() / PEProcessManager.shared() import as optionals
        // (the ObjC headers carry no nullability annotations); they're never
        // actually nil, but Swift requires the unwrap.
        guard let map = FDMapObject.emptyMap() else {
            return .failure("Could not allocate a file-descriptor map.")
        }
        map.appendFileDescriptor(inPipe.fileHandleForReading.fileDescriptor, withMappingToLoc: STDIN_FILENO)
        map.appendFileDescriptor(outPipe.fileHandleForWriting.fileDescriptor, withMappingToLoc: STDOUT_FILENO)
        map.appendFileDescriptor(outPipe.fileHandleForWriting.fileDescriptor, withMappingToLoc: STDERR_FILENO)
        map.appendFileDescriptor(inPipe.fileHandleForWriting.fileDescriptor, withMappingToLoc: 100)
        map.appendFileDescriptor(outPipe.fileHandleForReading.fileDescriptor, withMappingToLoc: 101)

        let collector = OutputCollector()
        outPipe.fileHandleForReading.readabilityHandler = { handle in
            let data = handle.availableData
            if !data.isEmpty { collector.append(data) }
        }
        func stopReading() { outPipe.fileHandleForReading.readabilityHandler = nil }

        // Launch the product.
        guard let manager = PEProcessManager.shared() else {
            stopReading()
            return .failure("The process manager is unavailable.")
        }
        let scheme = project.projectConfig.schemeKind
        let pid: pid_t
        if scheme == .utility {
            guard let path = LDEApplicationWorkspace.shared().fastpathUtility(project.machoURL.path) else {
                stopReading()
                return .failure("Could not prepare the utility binary to run. Build the project first.")
            }
            pid = manager.spawnProcess(withItems: ["PEExecutablePath": path, "PEArguments": [path], "PEMapObject": map],
                                       withKernelSurfaceProcess: nil)
        } else if scheme == .app {
            guard let bundleId = project.projectConfig.bundleid else {
                stopReading()
                return .failure("Project has no bundle identifier.")
            }
            pid = manager.spawnProcess(withBundleIdentifier: bundleId,
                                       withItems: ["PEMapObject": map],
                                       withKernelSurfaceProcess: nil,
                                       doRestartIfRunning: true)
        } else {
            stopReading()
            return .failure("The run tool supports app and utility schemes only.")
        }

        guard pid >= 0, let process = manager.processForProcessIdentifier(pid) else {
            stopReading()
            return .failure("Failed to launch the built product (pid \(pid)). On iOS 27 this requires the LiveProcess fix.")
        }

        // Wait for exit, or kill on timeout — whichever comes first.
        let exited = await withCheckedContinuation { (continuation: CheckedContinuation<Bool, Never>) in
            let resumer = Resumer(continuation)
            process.exitingCallback = { _ = resumer.resume(true) }
            Task {
                try? await Task.sleep(nanoseconds: UInt64(timeout) * 1_000_000_000)
                if resumer.resume(false) {
                    _ = process.terminate()
                }
            }
        }

        stopReading()
        let output = collector.string()
        let header = exited ? "Process exited." : "Process killed after \(timeout)s timeout."
        // NOTE: the precise exit code lives in private FBProcess internals
        // (exitContext.underlyingContext.legacyCode); reaching it safely from
        // Swift is fragile, so success here means "exited before the timeout".
        return ToolResult(success: exited, content: "\(header)\n\n\(output.isEmpty ? "(no output)" : output)")
    }
}

/// Thread-safe single-shot continuation resumer: whichever of exit/timeout fires
/// first wins; `resume` returns true only for that winner.
private final class Resumer: @unchecked Sendable {
    private let lock = NSLock()
    private var done = false
    private let continuation: CheckedContinuation<Bool, Never>

    init(_ continuation: CheckedContinuation<Bool, Never>) {
        self.continuation = continuation
    }

    @discardableResult
    func resume(_ value: Bool) -> Bool {
        lock.lock(); defer { lock.unlock() }
        if done { return false }
        done = true
        continuation.resume(returning: value)
        return true
    }
}

/// Thread-safe accumulator for the streamed output.
private final class OutputCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var data = Data()

    func append(_ chunk: Data) {
        lock.lock(); data.append(chunk); lock.unlock()
    }

    func string() -> String {
        lock.lock(); defer { lock.unlock() }
        return String(data: data, encoding: .utf8) ?? ""
    }
}

#endif /* !JAILBREAK_ENV */
