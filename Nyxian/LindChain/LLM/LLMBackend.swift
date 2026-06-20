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

/// The engine that produces completions sits behind this protocol so the agent
/// loop never depends on a concrete model. The shipped default is the in-app
/// `LlamaCppBackend`; a remote OpenAI-compatible backend (and, later, a router
/// that keeps the tiny local model for tool-calling and delegates heavy
/// reasoning to a remote model) can conform without touching the agent.
public protocol LLMBackend: AnyObject, Sendable {
    /// True once a model is loaded and the backend can serve completions.
    var isReady: Bool { get }

    /// A short human-readable identifier for the active model (for UI / logs).
    var modelDescription: String { get }

    /// Run one completion over `messages`.
    ///
    /// - Streams assistant text through `onToken` as it is generated.
    /// - When `tools` is non-nil the backend is expected to emit structured tool
    ///   calls (parsed into `CompletionResult.toolCalls`) rather than free text,
    ///   using grammar-constrained decoding where the backend supports it.
    /// - Cancellation is cooperative via Swift's `Task`: the implementation polls
    ///   `Task.isCancelled` between tokens and returns a `.cancelled` result.
    ///
    /// Implementations must run inference off the main thread.
    func complete(messages: [ChatMessage],
                  tools: [ToolSpec]?,
                  sampling: SamplingParameters,
                  onToken: @Sendable @escaping (String) -> Void) async throws -> CompletionResult
}

// MARK: - Conversation

/// Role of a single chat message.
public enum ChatRole: String, Codable, Sendable {
    case system
    case user
    case assistant
    case tool
}

/// One message in the conversation history.
public struct ChatMessage: Codable, Sendable {
    public var role: ChatRole
    public var content: String

    /// For `role == .assistant`: tool calls the model requested this turn.
    public var toolCalls: [ToolCall]?

    /// For `role == .tool`: the call this message reports the result of.
    public var toolCallID: String?
    /// For `role == .tool`: the name of the tool that produced the result.
    public var toolName: String?

    public init(role: ChatRole,
                content: String,
                toolCalls: [ToolCall]? = nil,
                toolCallID: String? = nil,
                toolName: String? = nil) {
        self.role = role
        self.content = content
        self.toolCalls = toolCalls
        self.toolCallID = toolCallID
        self.toolName = toolName
    }

    public static func system(_ content: String) -> ChatMessage { .init(role: .system, content: content) }
    public static func user(_ content: String) -> ChatMessage { .init(role: .user, content: content) }
    public static func assistant(_ content: String) -> ChatMessage { .init(role: .assistant, content: content) }

    /// A tool-result message to feed an observation back to the model.
    public static func toolResult(callID: String, name: String, content: String) -> ChatMessage {
        .init(role: .tool, content: content, toolCallID: callID, toolName: name)
    }
}

// MARK: - Tools

/// Declaration of a tool the model may call. `parameters` is a JSON Schema
/// object describing the arguments; it doubles as the source for a GBNF grammar
/// when grammar-constrained decoding is used.
public struct ToolSpec: Codable, Sendable {
    public var name: String
    public var description: String
    public var parameters: JSONValue

    public init(name: String, description: String, parameters: JSONValue) {
        self.name = name
        self.description = description
        self.parameters = parameters
    }
}

/// A single tool call emitted by the model.
public struct ToolCall: Codable, Sendable, Identifiable {
    public var id: String
    public var name: String
    /// The call arguments as a JSON object.
    public var arguments: JSONValue

    public init(id: String, name: String, arguments: JSONValue) {
        self.id = id
        self.name = name
        self.arguments = arguments
    }
}

// MARK: - Sampling

/// Decoding parameters for a single completion.
public struct SamplingParameters: Sendable {
    public var temperature: Float
    public var topP: Float
    /// Hard cap on generated tokens for this completion.
    public var maxTokens: Int
    /// Optional fixed seed for reproducible sampling.
    public var seed: UInt32?
    /// Optional GBNF grammar constraining the output (e.g. forcing a tool-call
    /// JSON shape). When nil the backend samples freely.
    public var grammar: String?

    public init(temperature: Float = 0.7,
                topP: Float = 0.95,
                maxTokens: Int = 1024,
                seed: UInt32? = nil,
                grammar: String? = nil) {
        self.temperature = temperature
        self.topP = topP
        self.maxTokens = maxTokens
        self.seed = seed
        self.grammar = grammar
    }

    public static let `default` = SamplingParameters()
}

// MARK: - Result

/// Why a completion ended.
public enum FinishReason: String, Sendable {
    case stop          // hit an end-of-turn / stop condition
    case length        // reached maxTokens / context limit
    case toolCalls     // ended to emit tool calls
    case cancelled     // cancelled cooperatively
}

/// The outcome of a completion: free text and/or parsed tool calls, plus token
/// accounting for the memory/UX surfaces.
public struct CompletionResult: Sendable {
    public var text: String
    public var toolCalls: [ToolCall]
    public var finishReason: FinishReason
    public var promptTokens: Int
    public var completionTokens: Int

    public init(text: String,
                toolCalls: [ToolCall] = [],
                finishReason: FinishReason,
                promptTokens: Int = 0,
                completionTokens: Int = 0) {
        self.text = text
        self.toolCalls = toolCalls
        self.finishReason = finishReason
        self.promptTokens = promptTokens
        self.completionTokens = completionTokens
    }
}

// MARK: - Errors

public enum LLMError: Error, Sendable {
    case modelNotLoaded
    case insufficientMemory(requiredBytes: UInt64, availableBytes: UInt64)
    case modelLoadFailed(String)
    case contextCreationFailed
    case tokenizationFailed
    case decodeFailed(Int32)
    case invalidGrammar(String)
    case backendUnavailable(String)
}
