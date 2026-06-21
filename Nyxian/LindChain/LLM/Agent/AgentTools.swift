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

/// The set of tools the agent can call, plus the dispatcher the agent loop uses.
/// Tools advertise themselves via `specs`; `invoke` runs one by name and always
/// returns a `ToolResult` (errors become failed results so the loop can recover).
public final class AgentTools: Sendable {

    public let workspace: ProjectWorkspace
    private let registry: [String: AgentTool]

    public init(workspace: ProjectWorkspace, tools: [AgentTool]) {
        self.workspace = workspace
        self.registry = Dictionary(tools.map { ($0.spec.name, $0) }, uniquingKeysWith: { first, _ in first })
    }

    /// Tool specifications to advertise to the model.
    public var specs: [ToolSpec] {
        registry.values.map(\.spec).sorted { $0.name < $1.name }
    }

    public func tool(named name: String) -> AgentTool? { registry[name] }

    /// Dispatch a tool call by name. Unknown tools and thrown errors are returned
    /// as failed `ToolResult`s rather than propagated, so the agent can read the
    /// error and retry.
    public func invoke(toolName: String, arguments: JSONValue) async -> ToolResult {
        guard let tool = registry[toolName] else {
            return .failure("Unknown tool: \(toolName)")
        }
        do {
            return try await tool.invoke(arguments)
        } catch let error as AgentToolError {
            return .failure(Self.describe(error))
        } catch {
            return .failure("Tool \(toolName) failed: \(error.localizedDescription)")
        }
    }

    private static func describe(_ error: AgentToolError) -> String {
        switch error {
        case .missingArgument(let k):  return "Missing required argument: \(k)"
        case .invalidArgument(let k):  return "Invalid argument: \(k)"
        case .pathEscapesProject(let p): return "Path escapes the project root: \(p)"
        case .notFound(let p):         return "Not found: \(p)"
        }
    }
}

public extension AgentTools {
    /// The standard tool set wired to a project. Extended slice by slice as the
    /// write / build / run tools land.
    static func standard(project: NXProject) -> AgentTools {
        let workspace = ProjectWorkspace(root: project.url)
        return AgentTools(workspace: workspace, tools: [
            ListFilesTool(workspace: workspace),
            ReadFileTool(workspace: workspace),
            SearchTool(workspace: workspace),
        ])
    }
}
