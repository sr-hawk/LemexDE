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

/// Manages the GGUF models installed in the app container and which one is
/// active. The engine is model-agnostic, so this is just a registry of files
/// plus a persisted "active" selection — the source of truth for the model
/// picker UI and for whatever loads a model (debug runner today, the agent
/// later).
public final class LLMModelStore {

    public static let shared = LLMModelStore()

    private let activeKey = "LLMActiveModelFilename"
    private let defaults = UserDefaults.standard

    private init() {}

    /// Directory where models live: <Application Support>/Models. Created lazily.
    public var modelsDirectory: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        let dir = base.appendingPathComponent("Models", isDirectory: true)
        if !FileManager.default.fileExists(atPath: dir.path) {
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        return dir
    }

    /// All installed GGUF models, sorted by name.
    public func availableModels() -> [URL] {
        let urls = (try? FileManager.default.contentsOfDirectory(at: modelsDirectory,
                                                                 includingPropertiesForKeys: [.fileSizeKey],
                                                                 options: [.skipsHiddenFiles])) ?? []
        return urls
            .filter { $0.pathExtension.lowercased() == "gguf" }
            .sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
    }

    /// Filename of the active model, or nil.
    public var activeModelFilename: String? {
        defaults.string(forKey: activeKey)
    }

    /// URL of the active model if it is set and still present on disk.
    public var activeModelURL: URL? {
        guard let name = activeModelFilename else { return nil }
        let url = modelsDirectory.appendingPathComponent(name)
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    /// Set (or clear, with nil) the active model.
    public func setActiveModel(_ url: URL?) {
        if let url {
            defaults.set(url.lastPathComponent, forKey: activeKey)
        } else {
            defaults.removeObject(forKey: activeKey)
        }
    }

    /// Copy an external GGUF into the models directory. Returns the new URL.
    @discardableResult
    public func importModel(from source: URL) throws -> URL {
        let destination = modelsDirectory.appendingPathComponent(source.lastPathComponent)
        if FileManager.default.fileExists(atPath: destination.path) {
            try FileManager.default.removeItem(at: destination)
        }
        try FileManager.default.copyItem(at: source, to: destination)
        return destination
    }

    /// Delete an installed model; clears the active selection if it was active.
    public func deleteModel(_ url: URL) throws {
        try FileManager.default.removeItem(at: url)
        if activeModelFilename == url.lastPathComponent {
            setActiveModel(nil)
        }
    }

    /// On-disk size in bytes, or 0.
    public func fileSize(of url: URL) -> UInt64 {
        let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
        return (attrs?[.size] as? NSNumber)?.uint64Value ?? 0
    }
}
