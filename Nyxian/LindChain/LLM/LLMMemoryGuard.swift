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

/// Guards against loading a model too large for the device. Uses
/// `os_proc_available_memory()`, which reports the bytes the process may still
/// allocate before jetsam — and honours the increased-memory entitlement the
/// app carries, so the budget reflects the real ceiling, not the default cap.
public enum LLMMemoryGuard {

    /// Bytes the process can still allocate before jetsam.
    public static func availableBytes() -> UInt64 {
        let value = os_proc_available_memory()
        return value > 0 ? UInt64(value) : 0
    }

    /// Coarse footprint estimate for a GGUF model: the weights (~file size) get
    /// resident, plus an allowance for the KV cache / compute buffers that grows
    /// with context length. Intentionally conservative — the goal is to reject
    /// clearly-too-large models, not to predict usage exactly.
    public static func estimatedFootprintBytes(modelPath: String, contextLength: UInt32) -> UInt64 {
        let attrs = try? FileManager.default.attributesOfItem(atPath: modelPath)
        let weights = (attrs?[.size] as? NSNumber)?.uint64Value ?? 0
        let kvAllowance = UInt64(contextLength) * 96 * 1024   // ~96 KB / context token
        return weights + (weights / 8) + kvAllowance          // weights + ~12% overhead + KV
    }

    /// Throws `LLMError.insufficientMemory` if the estimated footprint would
    /// exceed `safeFraction` of the currently-available memory.
    public static func check(modelPath: String,
                             contextLength: UInt32,
                             safeFraction: Double = 0.8) throws {
        let available = availableBytes()
        let required = estimatedFootprintBytes(modelPath: modelPath, contextLength: contextLength)
        let budget = UInt64(Double(available) * safeFraction)
        if available == 0 || required > budget {
            throw LLMError.insufficientMemory(requiredBytes: required, availableBytes: available)
        }
    }
}
