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

/// Temporary Session A smoke test for the in-app inference engine — remove once
/// the agent UI (Session D) provides a real entry point.
///
/// To run it: drop a small quantized GGUF named `llm-debug.gguf` into the app's
/// Documents directory (e.g. via the Files app) and relaunch. The engine loads
/// it, streams one completion, and logs the result to the console
/// (`idevicesyslog` / Xcode). If the file is absent it does nothing.
public enum LLMDebugRunner {

    private static let modelFileName = "llm-debug.gguf"
    private static let prompt = "Write a short haiku about compiling code on an iPhone."

    /// Called from app launch. Cheap and silent when no debug model is present.
    public static func runIfRequested() {
        guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            return
        }
        let modelURL = docs.appendingPathComponent(modelFileName)
        guard FileManager.default.fileExists(atPath: modelURL.path) else {
            NSLog("[LLMDebug] no \(modelFileName) in Documents; skipping engine smoke test")
            return
        }
        Task.detached(priority: .utility) {
            await run(modelPath: modelURL.path)
        }
    }

    static func run(modelPath: String) async {
        let backend = LlamaCppBackend()
        do {
            let availableMB = LLMMemoryGuard.availableBytes() / (1024 * 1024)
            NSLog("[LLMDebug] loading \(modelPath) — available: \(availableMB) MB")
            try backend.loadModel(path: modelPath, contextLength: 4096)
            NSLog("[LLMDebug] loaded \(backend.modelDescription); generating…")

            let result = try await backend.complete(
                messages: [.system("You are a concise assistant."), .user(prompt)],
                tools: nil,
                sampling: SamplingParameters(temperature: 0.7, maxTokens: 128)
            ) { token in
                // Stream visibility: each token as it arrives.
                NSLog("[LLMDebug] token: %@", token)
            }

            NSLog("[LLMDebug] done [%@] prompt=%d gen=%d",
                  String(describing: result.finishReason), result.promptTokens, result.completionTokens)
            NSLog("[LLMDebug] output:\n%@", result.text)
            backend.unload()
        } catch {
            NSLog("[LLMDebug] FAILED: %@", String(describing: error))
        }
    }
}
