// SPDX-License-Identifier: GPL-3.0-or-later
// FontReload — picks up fonts installed while Kinoklip runs. Qt reads the system fonts once at
// start and has no public rescan, so the font folders are watched and every new font file is
// added with QFontDatabase::addApplicationFont (as Kdenlive/MLT, Inkscape and LibreOffice do
// for fonts that appear later; see attribution.md). QFontComboBox lists follow by themselves
// (they listen to QGuiApplication::fontDatabaseChanged); titles are drawn again because
// TitleStyle carries fontGeneration().
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

namespace kk::render::fonts {

/// Counts the font reloads that added fonts. TitleStyle stores it, so a title drawn with a
/// fallback font before its font was installed is drawn (and cached) again afterwards.
[[nodiscard]] int fontGeneration();

/// The folders fontconfig searches: ~/.local/share/fonts, ~/.fonts, the XDG data dirs' fonts,
/// /usr/share/fonts, /usr/local/share/fonts. The environment variable KK_FONT_DIRS (folders
/// separated by ':') replaces the list (tests, unusual setups).
[[nodiscard]] QStringList defaultFontDirs();

class FontWatcher final : public QObject
{
    Q_OBJECT
public:
    /// Remembers the font files in @p dirs as already known and starts watching them.
    /// Changes are acted on @p debounceMs after the last one (a copy in progress settles).
    explicit FontWatcher(const QStringList& dirs, QObject* parent = nullptr, int debounceMs = 1500);

    /// Scans now: every font file not seen before is added to Qt's font database. Returns the
    /// families that became available (empty when nothing new). Also the Reload Fonts command.
    QStringList reload();

signals:
    /// Fonts were added; @p families are the new family names. Emitted after the database and
    /// fontGeneration() changed.
    void fontsAdded(const QStringList& families);

private:
    void watchFolders();
    [[nodiscard]] QSet<QString> scan() const;

    QStringList m_dirs;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_retry;
    QSet<QString> m_known;
    int m_retries = 0;
};

} // namespace kk::render::fonts
