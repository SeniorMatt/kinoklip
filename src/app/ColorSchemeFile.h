// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSchemeFile — reads a KDE colour scheme (.colors) into a QPalette without KF6.
#pragma once

#include <QColor>
#include <QHash>
#include <QPalette>
#include <QString>

namespace kk::theme {

/// A parsed KDE colour scheme file (the INI format of /usr/share/color-schemes/*.colors).
///
/// Used by the fallback build (no KDE Frameworks, e.g. Qt 6.4 on Mint): it turns the very
/// files KColorSchemeManager reads into a QPalette that follows the same role mapping as
/// KColorScheme::createApplicationPalette(), so the fallback looks close to the real thing.
/// The KF6 build uses KColorSchemeManager instead and needs this only for tests.
class ColorSchemeFile
{
public:
    /// Parses @p path (a file or a Qt resource). Returns false if it is unreadable or has no
    /// [Colors:Window] group.
    bool load(const QString& path);

    /// Id = file base name ("BreezeDark"), as used by KColorSchemeManager.
    [[nodiscard]] QString id() const { return m_id; }
    /// Untranslated display name ([General] Name), falls back to the id.
    [[nodiscard]] QString name() const { return m_name.isEmpty() ? m_id : m_name; }
    /// Colour @p key ("BackgroundNormal") of @p set ("Window", "View", "Button", "Selection",
    /// "Tooltip", ...); invalid if missing.
    [[nodiscard]] QColor color(const QString& set, const QString& key) const;
    /// Application palette built from the scheme.
    [[nodiscard]] QPalette palette() const;

private:
    QString m_id;
    QString m_name;
    QHash<QString, QColor> m_colors; ///< "Window/BackgroundNormal" -> colour
};

} // namespace kk::theme
