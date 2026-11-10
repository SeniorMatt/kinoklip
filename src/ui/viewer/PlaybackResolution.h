// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackResolution — the application-wide playback resolution (Resolve's "Timeline Proxy
// Resolution"): one value shared by the Playback menu, every viewer's resolution combo and
// Preferences > Playback, persisted in prefs::kPreviewQuality.
#pragma once

#include "playback/PlaybackEngine.h"

#include <QObject>
#include <QString>

class QComboBox;

namespace kk {

class ActionRegistry;

/// Holds the current PlaybackEngine::QualityMode and keeps its views in sync.
///
/// - setMode() stores the choice (QSettings) and emits changed(); ViewerPanels apply it to
///   their engines and combos, the Playback > Playback Resolution actions follow it.
/// - The viewer combo (makeCombo()) and the menu actions (connectActions()) call setMode().
/// - load() reads the stored value (with the one-time Auto -> Full migration of
///   prefs::previewQuality()) without writing it back.
///
/// Thread-safety: GUI thread only.
class PlaybackResolution : public QObject
{
    Q_OBJECT
public:
    using Mode = PlaybackEngine::QualityMode;

    /// The process-wide instance (created on first use, loads the stored value).
    static PlaybackResolution& instance();

    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Sets and stores @p mode; emits changed() if it differs.
    void setMode(Mode mode);
    /// Re-reads the stored value (after Preferences were applied); emits changed() if needed.
    void load();

    /// Stable settings id ("full", "half", "quarter", "auto") and back (unknown -> Full).
    [[nodiscard]] static QString toId(Mode mode);
    [[nodiscard]] static Mode fromId(const QString& id);
    /// Modes in menu / combo order: Full, Half, Quarter, Auto.
    [[nodiscard]] static QList<Mode> modes();
    /// Short label for the viewer header combo ("Full Res"...).
    [[nodiscard]] static QString shortLabel(Mode mode);
    /// Explanation used as tooltip / status tip.
    [[nodiscard]] static QString toolTip();
    /// One line for the menu entry of @p mode (status bar, docs/menu-reference.md).
    [[nodiscard]] static QString statusTip(Mode mode);

    /// Fills @p combo with the modes (item data = int(Mode)) and keeps it in sync both ways.
    void bindCombo(QComboBox* combo);
    /// Connects the "playback.resolution_*" actions of @p actions (checkable, exclusive).
    void connectActions(ActionRegistry* actions);

Q_SIGNALS:
    void changed(PlaybackEngine::QualityMode mode);

private:
    PlaybackResolution();
    Mode m_mode = Mode::Full;
};

} // namespace kk
