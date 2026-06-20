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

/// In-app `LLMBackend` backed by llama.cpp (Metal). Inference runs on a private
/// serial queue — a llama context must be driven from one thread at a time, and
/// generation must stay off the main thread.
public final class LlamaCppBackend: LLMBackend, @unchecked Sendable {

    private let engine = LLMEngine()
    private let queue = DispatchQueue(label: "org.emexlab.nyxian.llm.inference", qos: .userInitiated)

    public init() {}

    public var isReady: Bool { engine.isLoaded }
    public var modelDescription: String { engine.modelDescription }

    /// Load a GGUF model. Heavy and synchronous — call from a background context.
    /// `gpuLayers` defaults high to offload everything to Metal.
    public func loadModel(path: String, contextLength: UInt32 = 4096, gpuLayers: Int32 = 999) throws {
        // Refuse models too large for the device before touching llama.cpp.
        try LLMMemoryGuard.check(modelPath: path, contextLength: contextLength)
        var thrown: Error?
        queue.sync {
            do {
                try engine.loadModel(atPath: path, nCtx: contextLength, nGpuLayers: gpuLayers)
            } catch {
                thrown = error
            }
        }
        if let thrown { throw thrown }
    }

    public func unload() {
        queue.sync { engine.unload() }
    }

    public func complete(messages: [ChatMessage],
                         tools: [ToolSpec]?,
                         sampling: SamplingParameters,
                         onToken: @Sendable @escaping (String) -> Void) async throws -> CompletionResult {
        guard engine.isLoaded else { throw LLMError.modelNotLoaded }

        let prompt = buildPrompt(from: messages)

        let config = LLMSamplingConfig()
        config.temperature = sampling.temperature
        config.topP = sampling.topP
        config.maxTokens = Int32(clamping: sampling.maxTokens)
        config.seed = sampling.seed ?? 0xFFFF_FFFF   // LLAMA_DEFAULT_SEED → random
        config.grammar = sampling.grammar
        // NOTE: turning `tools` into a GBNF grammar and parsing tool-call output
        // is Session C's job. Session A streams plain text; tools are accepted
        // here so callers compile, but not yet enforced.

        let cancel = CancelFlag()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<CompletionResult, Error>) in
                queue.async {
                    do {
                        let result = try self.engine.generate(withPrompt: prompt,
                                                              sampling: config,
                                                              onToken: { token in onToken(token) },
                                                              isCancelled: { cancel.isCancelled })
                        continuation.resume(returning: Self.map(result))
                    } catch {
                        continuation.resume(throwing: error)
                    }
                }
            }
        } onCancel: {
            cancel.cancel()
        }
    }

    // MARK: - Helpers

    private func buildPrompt(from messages: [ChatMessage]) -> String {
        let roles = messages.map { $0.role.rawValue }
        let contents = messages.map { $0.content }
        if let templated = engine.applyChatTemplate(withRoles: roles, contents: contents, addAssistant: true) {
            return templated
        }
        // Fallback for models without an embedded chat template.
        var parts = messages.map { "\($0.role.rawValue): \($0.content)" }
        parts.append("assistant:")
        return parts.joined(separator: "\n")
    }

    private static func map(_ result: LLMGenerationResult) -> CompletionResult {
        let reason: FinishReason
        switch result.finishReason {
        case .length:    reason = .length
        case .cancelled: reason = .cancelled
        default:         reason = .stop
        }
        return CompletionResult(text: result.text,
                                toolCalls: [],
                                finishReason: reason,
                                promptTokens: Int(result.promptTokens),
                                completionTokens: Int(result.completionTokens))
    }
}

/// Thread-safe cancellation flag bridged into the C++ generation loop.
private final class CancelFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var flag = false
    var isCancelled: Bool {
        lock.lock(); defer { lock.unlock() }
        return flag
    }
    func cancel() {
        lock.lock(); flag = true; lock.unlock()
    }
}
