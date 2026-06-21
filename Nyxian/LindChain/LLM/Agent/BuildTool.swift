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

import Foundation
import MobileDevelopmentKit

/// Compiles the project and returns structured diagnostics — the agent's primary
/// feedback signal.
///
/// It drives the same `Builder.buildProject` the build/run button uses, but with
/// `.InstallPackagedApp`, which runs the full compile (`executeRunner`) and signs
/// + packages without launching the product (launching is `run()`'s job) and
/// without requiring a code-signing certificate. Diagnostics are read back from
/// the `debug.json` the builder persists, where compiler diagnostics already
/// carry file/line/column/severity.
public struct BuildTool: AgentTool, @unchecked Sendable {

    public let project: NXProject

    public init(project: NXProject) { self.project = project }

    public var spec: ToolSpec {
        ToolSpec(
            name: "build",
            description: "Compile the project. Returns whether it built and structured diagnostics (file:line:col severity message).",
            parameters: ["type": "object", "properties": [:], "required": []]
        )
    }

    public func invoke(_ arguments: JSONValue) async throws -> ToolResult {
        // Run the real build pipeline; completion(Bool) is the builder's own
        // success (which also covers packaging), but we derive compile success
        // from the diagnostics so a packaging hiccup doesn't read as a code error.
        _ = await withCheckedContinuation { (continuation: CheckedContinuation<Bool, Never>) in
            Builder.buildProject(withProject: project,
                                 buildType: .InstallPackagedApp,
                                 outPipe: nil,
                                 inPipe: nil) { ok in
                continuation.resume(returning: ok)
            }
        }

        let dbPath = project.cacheURL.appendingPathComponent("debug.json").path
        let database = DebugDatabase.getDatabase(ofPath: dbPath)
        let workspace = ProjectWorkspace(root: project.url)

        var diagnostics: [String] = []
        var errorCount = 0
        var warningCount = 0

        for object in database.debugObjects.values {
            for item in object.debugItems {
                let severity = Self.severityLabel(item.severity)
                switch item.severity.rawValue {
                case 3, 4: errorCount += 1     // Error, Fatal
                case 2:    warningCount += 1   // Warning
                default:   break
                }
                let location = item.sourceLocation
                if object.flavour == .File, location.isValid.boolValue {
                    let relative = workspace.relativePath(URL(fileURLWithPath: object.title))
                    diagnostics.append("\(relative):\(location.line):\(location.column): \(severity): \(item.message)")
                } else {
                    diagnostics.append("\(object.title): \(severity): \(item.message)")
                }
            }
        }

        let compiled = errorCount == 0
        var lines: [String] = [
            compiled ? "Build succeeded." : "Build failed.",
            "\(errorCount) error(s), \(warningCount) warning(s)."
        ]
        if !diagnostics.isEmpty {
            lines.append("")
            lines.append(contentsOf: diagnostics.sorted())
        }
        return ToolResult(success: compiled, content: lines.joined(separator: "\n"))
    }

    private static func severityLabel(_ level: CCDiagnosticLevel) -> String {
        switch level.rawValue {
        case 0: return "note"
        case 1: return "remark"
        case 2: return "warning"
        case 3: return "error"
        case 4: return "fatal"
        default: return "unknown"
        }
    }
}
