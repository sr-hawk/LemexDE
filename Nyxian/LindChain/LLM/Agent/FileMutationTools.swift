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

public extension Notification.Name {
    /// Posted after the agent writes/patches a file. userInfo["path"] is the
    /// absolute file path. The editor observes this to reload an open buffer
    /// (wired with the agent UI). Posted on the main queue.
    static let nxAgentDidModifyFile = Notification.Name("NXAgentDidModifyFile")
}

/// Shared write + reload-notify helper for the mutation tools.
enum FileMutation {
    static func write(_ content: String, to url: URL) throws {
        let directory = url.deletingLastPathComponent()
        if !FileManager.default.fileExists(atPath: directory.path) {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        try content.data(using: .utf8)?.write(to: url, options: .atomic)
        let path = url.path
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: .nxAgentDidModifyFile, object: nil, userInfo: ["path": path])
        }
    }
}

// MARK: - write_file

/// Creates or overwrites a project file with the given content.
public struct WriteFileTool: AgentTool {
    public let workspace: ProjectWorkspace

    public init(workspace: ProjectWorkspace) { self.workspace = workspace }

    public var spec: ToolSpec {
        ToolSpec(
            name: "write_file",
            description: "Create or overwrite a project file with the given content.",
            parameters: [
                "type": "object",
                "properties": [
                    "path": ["type": "string", "description": "Project-relative file path."],
                    "content": ["type": "string", "description": "Full file content to write."]
                ],
                "required": ["path", "content"]
            ]
        )
    }

    public func invoke(_ arguments: JSONValue) async throws -> ToolResult {
        let path = try arguments.requireString("path")
        let content = try arguments.requireString("content")
        guard let url = workspace.resolve(path) else { throw AgentToolError.pathEscapesProject(path) }
        do {
            try FileMutation.write(content, to: url)
        } catch {
            return .failure("Could not write \(path): \(error.localizedDescription)")
        }
        let lineCount = content.components(separatedBy: "\n").count
        return .ok("Wrote \(path) (\(lineCount) lines).")
    }
}

// MARK: - apply_patch

/// Applies a unified diff to a single project file.
public struct ApplyPatchTool: AgentTool {
    public let workspace: ProjectWorkspace

    public init(workspace: ProjectWorkspace) { self.workspace = workspace }

    public var spec: ToolSpec {
        ToolSpec(
            name: "apply_patch",
            description: "Apply a unified diff (diff -u format) to a project file.",
            parameters: [
                "type": "object",
                "properties": [
                    "path": ["type": "string", "description": "Project-relative file path to patch."],
                    "unified_diff": ["type": "string",
                                     "description": "The patch in unified-diff format: @@ hunk headers with ' ', '-' and '+' lines."]
                ],
                "required": ["path", "unified_diff"]
            ]
        )
    }

    public func invoke(_ arguments: JSONValue) async throws -> ToolResult {
        let path = try arguments.requireString("path")
        let diff = try arguments.requireString("unified_diff")
        guard let url = workspace.resolve(path) else { throw AgentToolError.pathEscapesProject(path) }
        guard FileManager.default.fileExists(atPath: url.path) else { throw AgentToolError.notFound(path) }
        guard let data = try? Data(contentsOf: url), let original = String(data: data, encoding: .utf8) else {
            return .failure("Could not read \(path) as UTF-8 text.")
        }

        do {
            let patched = try UnifiedDiff.apply(diff, to: original)
            try FileMutation.write(patched, to: url)
            return .ok("Patched \(path).")
        } catch let error as UnifiedDiff.PatchError {
            return .failure("Patch did not apply to \(path): \(error.description)")
        } catch {
            return .failure("Could not patch \(path): \(error.localizedDescription)")
        }
    }
}

// MARK: - Unified diff applier

/// A small, dependency-free unified-diff applier. Handles standard `diff -u`
/// hunks (`@@ -a,b +c,d @@` with ' ', '-', '+' lines). Locates each hunk by its
/// old-start hint and verifies context/removed lines match before splicing, so a
/// bad patch fails loudly instead of corrupting the file.
enum UnifiedDiff {
    struct PatchError: Error, CustomStringConvertible {
        let description: String
    }

    private struct Hunk {
        var oldStart: Int          // 1-based
        var lines: [(tag: Character, text: String)]
    }

    static func apply(_ diff: String, to original: String) throws -> String {
        let hunks = try parseHunks(diff)
        if hunks.isEmpty { throw PatchError(description: "no hunks found") }

        var source = original.components(separatedBy: "\n")
        var result: [String] = []
        var cursor = 0   // 0-based index into source already consumed

        for hunk in hunks {
            // Find where this hunk's "before" block actually sits, starting from
            // the header hint and searching forward a little if it drifted.
            let before = hunk.lines.filter { $0.tag == " " || $0.tag == "-" }.map { $0.text }
            let hint = max(cursor, hunk.oldStart - 1)
            guard let matchIndex = locate(before, in: source, from: hint, fallbackFrom: cursor) else {
                throw PatchError(description: "context not found near line \(hunk.oldStart)")
            }
            // Copy untouched lines up to the match.
            if matchIndex > cursor {
                result.append(contentsOf: source[cursor..<matchIndex])
            }
            // Apply the hunk.
            var srcIndex = matchIndex
            for line in hunk.lines {
                switch line.tag {
                case " ":
                    result.append(source[srcIndex]); srcIndex += 1
                case "-":
                    srcIndex += 1   // drop
                case "+":
                    result.append(line.text)
                default:
                    break
                }
            }
            cursor = srcIndex
        }

        if cursor < source.count {
            result.append(contentsOf: source[cursor...])
        }
        return result.joined(separator: "\n")
    }

    private static func locate(_ before: [String], in source: [String], from hint: Int, fallbackFrom: Int) -> Int? {
        if before.isEmpty { return min(max(hint, fallbackFrom), source.count) }
        let maxStart = source.count - before.count
        if maxStart < 0 { return nil }
        // Try the header's hint first, then scan forward from the cursor.
        if hint >= 0, hint <= maxStart, Array(source[hint..<hint + before.count]) == before {
            return hint
        }
        var start = max(0, fallbackFrom)
        while start <= maxStart {
            if Array(source[start..<start + before.count]) == before { return start }
            start += 1
        }
        return nil
    }

    private static func parseHunks(_ diff: String) throws -> [Hunk] {
        var hunks: [Hunk] = []
        var current: Hunk?
        for raw in diff.components(separatedBy: "\n") {
            if raw.hasPrefix("@@") {
                if let current { hunks.append(current) }
                guard let oldStart = parseOldStart(raw) else {
                    throw PatchError(description: "malformed hunk header: \(raw)")
                }
                current = Hunk(oldStart: oldStart, lines: [])
            } else if current != nil {
                // Skip file headers that may be interleaved.
                if raw.hasPrefix("--- ") || raw.hasPrefix("+++ ") || raw.hasPrefix("diff ") || raw.hasPrefix("index ") {
                    continue
                }
                guard let first = raw.first else {
                    current?.lines.append((" ", ""))   // blank context line
                    continue
                }
                if first == " " || first == "+" || first == "-" {
                    current?.lines.append((first, String(raw.dropFirst())))
                } else if raw == "\\ No newline at end of file" {
                    continue
                }
            }
        }
        if let current { hunks.append(current) }
        return hunks
    }

    /// Parse the old-file start line from `@@ -a,b +c,d @@`.
    private static func parseOldStart(_ header: String) -> Int? {
        guard let dashRange = header.range(of: "-") else { return nil }
        let after = header[dashRange.upperBound...]
        let number = after.prefix { $0.isNumber }
        return Int(number)
    }
}
