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

/// The result of running a tool. `content` is the model-readable observation
/// fed back into the conversation; `success` lets the agent loop and UI render
/// failures distinctly.
public struct ToolResult: Sendable {
    public var success: Bool
    public var content: String

    public init(success: Bool, content: String) {
        self.success = success
        self.content = content
    }

    public static func ok(_ content: String) -> ToolResult { .init(success: true, content: content) }
    public static func failure(_ content: String) -> ToolResult { .init(success: false, content: content) }
}

/// Errors a tool can throw before producing a result.
public enum AgentToolError: Error, Sendable {
    case missingArgument(String)
    case invalidArgument(String)
    case pathEscapesProject(String)
    case notFound(String)
}

/// One concrete capability the agent can invoke. `spec` is what's advertised to
/// the model (name + JSON-Schema parameters); `invoke` runs it.
public protocol AgentTool: Sendable {
    var spec: ToolSpec { get }
    func invoke(_ arguments: JSONValue) async throws -> ToolResult
}

/// A project root plus path-safety helpers. Tools take paths relative to the
/// project; this refuses anything that escapes the root (`..`, absolute paths
/// outside it), so the agent can't read or write arbitrary files on the device.
public struct ProjectWorkspace: Sendable {
    public let root: URL

    /// Directories never walked or searched.
    public static let ignoredDirectories: Set<String> = ["Cache", "build", ".build", ".git", "DerivedData"]

    public init(root: URL) {
        self.root = root.standardizedFileURL
    }

    /// Resolve a project-relative (or absolute-within-root) path. Returns nil if
    /// it would escape the project root.
    public func resolve(_ path: String) -> URL? {
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        let candidate: URL
        if trimmed.hasPrefix("/") {
            candidate = URL(fileURLWithPath: trimmed).standardizedFileURL
        } else {
            candidate = root.appendingPathComponent(trimmed).standardizedFileURL
        }
        let rootPath = root.path
        guard candidate.path == rootPath || candidate.path.hasPrefix(rootPath + "/") else {
            return nil
        }
        return candidate
    }

    /// Path of `url` relative to the project root (for model-facing output).
    public func relativePath(_ url: URL) -> String {
        let rootPath = root.path
        let p = url.standardizedFileURL.path
        if p == rootPath { return "." }
        if p.hasPrefix(rootPath + "/") { return String(p.dropFirst(rootPath.count + 1)) }
        return p
    }

    /// Enumerate files (not directories) under `subpath`, skipping ignored dirs,
    /// up to `limit`. Returns absolute URLs.
    public func files(under subpath: String?, limit: Int) -> [URL] {
        let base = (subpath.flatMap { resolve($0) }) ?? root
        guard let enumerator = FileManager.default.enumerator(
            at: base,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else { return [] }

        var results: [URL] = []
        for case let url as URL in enumerator {
            if Self.ignoredDirectories.contains(url.lastPathComponent),
               (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
                enumerator.skipDescendants()
                continue
            }
            let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory ?? false
            if !isDir {
                results.append(url)
                if results.count >= limit { break }
            }
        }
        return results
    }
}

// MARK: - Argument helpers

public extension JSONValue {
    func requireString(_ key: String) throws -> String {
        guard let v = self[key]?.stringValue else { throw AgentToolError.missingArgument(key) }
        return v
    }
    func optionalString(_ key: String) -> String? { self[key]?.stringValue }
    func optionalInt(_ key: String) -> Int? { self[key]?.intValue }
    func optionalBool(_ key: String) -> Bool? { self[key]?.boolValue }
}
