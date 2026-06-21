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

import UIKit
import UniformTypeIdentifiers

/// Lets the user choose which installed GGUF model the engine uses, and import
/// new ones from the Files app. The engine is model-agnostic, so anything
/// llama.cpp can load (Qwen-coder, OLMoE, …) works here.
class LLMModelPickerViewController: UIThemedTableViewController, UIDocumentPickerDelegate {

    private let store = LLMModelStore.shared
    private var models: [URL] = []

    // Sections
    private let activeSection = 0
    private let installedSection = 1
    private let importSection = 2

    override func viewDidLoad() {
        super.viewDidLoad()
        self.title = "Model"
        reload()
    }

    private func reload() {
        models = store.availableModels()
        tableView.reloadData()
    }

    // MARK: - Data source

    override func numberOfSections(in tableView: UITableView) -> Int { 3 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        switch section {
        case activeSection:    return 1
        case installedSection: return max(models.count, 1)   // 1 placeholder row when empty
        case importSection:    return 1
        default:               return 0
        }
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        switch section {
        case activeSection:    return "Active Model"
        case installedSection: return "Installed Models"
        default:               return nil
        }
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        switch section {
        case importSection:
            return "Import a .gguf model from Files. Anything llama.cpp supports works — a small quantized coder model (e.g. Qwen-coder 3B, Q4_K_M) is a good default."
        default:
            return nil
        }
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .subtitle, reuseIdentifier: nil)

        switch indexPath.section {
        case activeSection:
            if let active = store.activeModelURL {
                cell.textLabel?.text = active.lastPathComponent
                cell.detailTextLabel?.text = "\(byteString(store.fileSize(of: active))) · \(fitDescription(for: active))"
                cell.imageView?.image = UIImage(systemName: "brain.head.profile")
            } else {
                cell.textLabel?.text = "None selected"
                cell.detailTextLabel?.text = "Pick a model below"
                cell.imageView?.image = UIImage(systemName: "brain.head.profile")
            }
            cell.selectionStyle = .none

        case installedSection:
            if models.isEmpty {
                cell.textLabel?.text = "No models installed"
                cell.detailTextLabel?.text = "Use Import below"
                cell.textLabel?.textColor = .secondaryLabel
                cell.selectionStyle = .none
            } else {
                let url = models[indexPath.row]
                cell.textLabel?.text = url.lastPathComponent
                cell.detailTextLabel?.text = "\(byteString(store.fileSize(of: url))) · \(fitDescription(for: url))"
                cell.accessoryType = (url.lastPathComponent == store.activeModelFilename) ? .checkmark : .none
            }

        case importSection:
            cell.textLabel?.text = "Import Model…"
            cell.textLabel?.textColor = view.tintColor
            cell.imageView?.image = UIImage(systemName: "square.and.arrow.down")

        default:
            break
        }
        return cell
    }

    // MARK: - Selection

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        switch indexPath.section {
        case installedSection:
            guard !models.isEmpty else { return }
            store.setActiveModel(models[indexPath.row])
            reload()
        case importSection:
            presentImporter()
        default:
            break
        }
    }

    // MARK: - Delete

    override func tableView(_ tableView: UITableView, canEditRowAt indexPath: IndexPath) -> Bool {
        indexPath.section == installedSection && !models.isEmpty
    }

    override func tableView(_ tableView: UITableView, commit editingStyle: UITableViewCell.EditingStyle, forRowAt indexPath: IndexPath) {
        guard editingStyle == .delete, indexPath.section == installedSection, !models.isEmpty else { return }
        do {
            try store.deleteModel(models[indexPath.row])
            reload()
        } catch {
            presentError("Could not delete model", error)
        }
    }

    // MARK: - Import

    private func presentImporter() {
        // Prefer a .gguf-only filter; fall back to any file only if the system
        // can't construct the gguf type.
        let ggufType = UTType(filenameExtension: "gguf")
        let types: [UTType] = ggufType.map { [$0] } ?? [.data]
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: types, asCopy: true)
        picker.delegate = self
        picker.allowsMultipleSelection = false
        present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let source = urls.first else { return }
        let needsScope = source.startAccessingSecurityScopedResource()
        defer { if needsScope { source.stopAccessingSecurityScopedResource() } }
        do {
            let imported = try store.importModel(from: source)
            // First import becomes the active model for convenience.
            if store.activeModelFilename == nil {
                store.setActiveModel(imported)
            }
            reload()
        } catch {
            presentError("Could not import model", error)
        }
    }

    // MARK: - Helpers

    private func fitDescription(for url: URL) -> String {
        let footprint = LLMMemoryGuard.estimatedFootprintBytes(modelPath: url.path, contextLength: 4096)
        let available = LLMMemoryGuard.availableBytes()
        let budget = UInt64(Double(available) * 0.8)
        if available == 0 { return "memory unknown" }
        return footprint > budget ? "may exceed memory" : "fits"
    }

    private func byteString(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    }

    private func presentError(_ title: String, _ error: Error) {
        let alert = UIAlertController(title: title, message: error.localizedDescription, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        present(alert, animated: true)
    }
}
