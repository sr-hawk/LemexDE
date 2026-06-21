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

// MARK: - list_files

/// Lists project files (optionally under a subpath), skipping build/cache dirs.
public struct ListFilesTool: AgentTool {
    public let workspace: ProjectWorkspace
    private let limit = 1000

    public init(workspace: ProjectWorkspace) { self.workspace = workspace }

    public var spec: ToolSpec {
        ToolSpec(
            name: "list_files",
            description: "List files in the project, optionally restricted to a subdirectory.",
            parameters: [
                "type": "object",
                "properties": [
                    "subpath": ["type": "string",
                                "description": "Optional project-relative directory to list. Defaults to the project root."]
                ],
                "required": []
            ]
        )
    }

    public func invoke(_ arguments: JSONValue) async throws -> ToolResult {
        let subpath = arguments.optionalString("subpath")
        if let subpath, workspace.resolve(subpath) == nil {
            throw AgentToolError.pathEscapesProject(subpath)
        }
        let files = workspace.files(under: subpath, limit: limit + 1)
        let relatives = files.prefix(limit).map { workspace.relativePath($0) }.sorted()
        if relatives.isEmpty { return .ok("(no files)") }
        var text = relatives.joined(separator: "\n")
        if files.count > limit { text += "\n… (truncated at \(limit) files)" }
        return .ok(text)
    }
}

// MARK: - read_file

/// Reads a project file, optionally a 1-based line range; caps large reads.
public struct ReadFileTool: AgentTool {
    public let workspace: ProjectWorkspace
    private let maxBytesWithoutRange = 200_000
    private let maxLinesWithoutRange = 600

    public init(workspace: ProjectWorkspace) { self.workspace = workspace }

    public var spec: ToolSpec {
        ToolSpec(
            name: "read_file",
            description: "Read a project file. Optionally restrict to a 1-based inclusive line range.",
            parameters: [
                "type": "object",
                "properties": [
                    "path": ["type": "string", "description": "Project-relative file path."],
                    "start_line": ["type": "integer", "description": "Optional 1-based first line."],
                    "end_line": ["type": "integer", "description": "Optional 1-based last line (inclusive)."]
                ],
                "required": ["path"]
            ]
        )
    }

    public func invoke(_ arguments: JSONValue) async throws -> ToolResult {
        let path = try arguments.requireString("path")
        guard let url = workspace.resolve(path) else { throw AgentToolError.pathEscapesProject(path) }
        guard FileManager.default.fileExists(atPath: url.path) else { throw AgentToolError.notFound(path) }
        guard let data = try? Data(contentsOf: url), let content = String(data: data, encoding: .utf8) else {
            return .failure("Could not read \(path) as UTF-8 text (binary file?).")
        }

        let lines = content.components(separatedBy: "\n")
        let start = arguments.optionalInt("start_line")
        let end = arguments.optionalInt("end_line")

        if start != nil || end != nil {
            let s = max(1, start ?? 1)
            let e = min(lines.count, end ?? lines.count)
            guard s <= e else { return .failure("Invalid range: start_line (\(s)) > end_line (\(e)).") }
            let body = numbered(lines[(s - 1)..<e], firstLine: s)
            return .ok("\(path) [lines \(s)–\(e) of \(lines.count)]\n\(body)")
        }

        if data.count > maxBytesWithoutRange || lines.count > maxLinesWithoutRange {
            let head = Array(lines.prefix(maxLinesWithoutRange))
            let body = numbered(head[...], firstLine: 1)
            return .ok("\(path) [first \(head.count) of \(lines.count) lines — request a line range for more]\n\(body)")
        }

        return .ok("\(path) [\(lines.count) lines]\n\(numbered(lines[...], firstLine: 1))")
    }

    private func numbered(_ slice: ArraySlice<String>, firstLine: Int) -> String {
        slice.enumerated().map { "\(firstLine + $0.offset)\t\($0.element)" }.joined(separator: "\n")
    }
}

// MARK: - search

/// Substring or regex search across project file contents; returns file:line hits.
public struct SearchTool: AgentTool {
    public let workspace: ProjectWorkspace
    private let maxHits = 200
    private let maxFiles = 5000

    public init(workspace: ProjectWorkspace) { self.workspace = workspace }

    public var spec: ToolSpec {
        ToolSpec(
            name: "search",
            description: "Search project file contents for a substring or regular expression. Returns file:line: hits.",
            parameters: [
                "type": "object",
                "properties": [
                    "query": ["type": "string", "description": "Text or regular expression to search for."],
                    "regex": ["type": "boolean", "description": "Treat query as a regular expression. Default false."],
                    "glob": ["type": "string", "description": "Optional comma-separated extension filter, e.g. \"swift\" or \"c,h\"."]
                ],
                "required": ["query"]
            ]
        )
    }

    public func invoke(_ arguments: JSONValue) async throws -> ToolResult {
        let query = try arguments.requireString("query")
        let useRegex = arguments.optionalBool("regex") ?? false

        let extensions: Set<String>? = arguments.optionalString("glob").map {
            Set($0.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces).lowercased() })
        }

        var regex: NSRegularExpression?
        if useRegex {
            guard let r = try? NSRegularExpression(pattern: query) else {
                return .failure("Invalid regular expression: \(query)")
            }
            regex = r
        }

        let files = workspace.files(under: nil, limit: maxFiles).filter { url in
            guard let extensions else { return true }
            return extensions.contains(url.pathExtension.lowercased())
        }

        var hits: [String] = []
        outer: for url in files {
            guard let data = try? Data(contentsOf: url), let content = String(data: data, encoding: .utf8) else { continue }
            let relative = workspace.relativePath(url)
            for (index, line) in content.components(separatedBy: "\n").enumerated() {
                let matched: Bool
                if let regex {
                    matched = regex.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)) != nil
                } else {
                    matched = line.contains(query)
                }
                if matched {
                    hits.append("\(relative):\(index + 1): \(line.trimmingCharacters(in: .whitespaces))")
                    if hits.count >= maxHits { break outer }
                }
            }
        }

        if hits.isEmpty { return .ok("No matches for \(query).") }
        var text = hits.joined(separator: "\n")
        if hits.count >= maxHits { text += "\n… (truncated at \(maxHits) hits)" }
        return .ok(text)
    }
}
