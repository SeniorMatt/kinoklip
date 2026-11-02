// SPDX-License-Identifier: GPL-3.0-or-later
// ExportPresets — built-in render presets (data: resources/export/presets.json) and user
// presets (saved as JSON in the app data folder).
#pragma once

#include "export/ExportSettings.h"

#include <QList>
#include <QString>

namespace kk {

/// One named preset. Presets never carry the output path or the render range.
struct ExportPreset
{
    QString id; ///< stable id; user presets are "user:<name>"
    QString name; ///< UI name
    QString category; ///< "Web", "Master", "Audio", "Images", "User"
    QString description; ///< one line for tooltips
    bool builtin = true;
    ExportSettings settings;
};

/// The preset list: built-ins first (file order), then user presets (alphabetical).
///
/// Built-ins come from the Qt resource `:/export/presets.json`; user presets from
/// `<AppDataLocation>/export_presets.json` (or the file given to the constructor, for tests).
/// Thread-safety: not thread-safe; GUI thread (or one owner).
class ExportPresets
{
public:
    /// Loads built-in and user presets. @p userFile empty = the default location.
    explicit ExportPresets(const QString& userFile = {});

    /// All presets.
    [[nodiscard]] const QList<ExportPreset>& all() const { return m_presets; }
    /// Preset by id, or nullptr.
    [[nodiscard]] const ExportPreset* find(const QString& id) const;
    /// Settings of preset @p id applied on top of @p base (keeps output path and range).
    /// @return false if the id is unknown.
    [[nodiscard]] bool apply(const QString& id, ExportSettings& base) const;

    /// Saves @p settings as user preset @p name (replaces one with the same name).
    /// @return the preset id, or empty with a message in @p error.
    QString saveUserPreset(
        const QString& name, const ExportSettings& settings, QString* error = nullptr);
    /// Deletes a user preset. Built-ins cannot be deleted. @return false with a message.
    bool removeUserPreset(const QString& id, QString* error = nullptr);

    /// The built-in presets alone (parsed once per process).
    [[nodiscard]] static const QList<ExportPreset>& builtinPresets();
    /// File holding the user presets.
    [[nodiscard]] QString userFile() const { return m_userFile; }

private:
    void loadUser();
    bool writeUser(QString* error) const;

    QString m_userFile;
    QList<ExportPreset> m_presets;
};

} // namespace kk
