// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackResolution — shared playback resolution setting (menu, viewer combos, preferences).
#include "ui/viewer/PlaybackResolution.h"

#include "core/Preferences.h"
#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QComboBox>
#include <QSignalBlocker>

namespace kk {

namespace {
/// Action id of each mode (StandardActions.cpp, category "playback.resolution").
QString actionId(PlaybackResolution::Mode mode)
{
    return QStringLiteral("playback.resolution_") + PlaybackResolution::toId(mode);
}
} // namespace

PlaybackResolution& PlaybackResolution::instance()
{
    static PlaybackResolution* resolution = new PlaybackResolution; // lives until exit
    return *resolution;
}

PlaybackResolution::PlaybackResolution()
    : m_mode(fromId(prefs::previewQuality()))
{ }

void PlaybackResolution::setMode(Mode mode)
{
    if (mode == m_mode) {
        return;
    }
    m_mode = mode;
    prefs::setPreviewQuality(toId(mode));
    Q_EMIT changed(mode);
}

void PlaybackResolution::load()
{
    const Mode stored = fromId(prefs::previewQuality());
    if (stored != m_mode) {
        m_mode = stored;
        Q_EMIT changed(stored);
    }
}

QString PlaybackResolution::toId(Mode mode)
{
    switch (mode) {
    case Mode::Auto:
        return QStringLiteral("auto");
    case Mode::Half:
        return QStringLiteral("half");
    case Mode::Quarter:
        return QStringLiteral("quarter");
    case Mode::Full:
        break;
    }
    return QStringLiteral("full");
}

PlaybackResolution::Mode PlaybackResolution::fromId(const QString& id)
{
    for (Mode mode : modes()) {
        if (toId(mode) == id) {
            return mode;
        }
    }
    return Mode::Full;
}

QList<PlaybackResolution::Mode> PlaybackResolution::modes()
{
    return {Mode::Full, Mode::Half, Mode::Quarter, Mode::Auto};
}

QString PlaybackResolution::shortLabel(Mode mode)
{
    switch (mode) {
    case Mode::Auto:
        return tr("Auto Res");
    case Mode::Half:
        return tr("Half Res");
    case Mode::Quarter:
        return tr("Quarter Res");
    case Mode::Full:
        break;
    }
    return tr("Full Res");
}

QString PlaybackResolution::toolTip()
{
    return tr("Playback resolution (also in Playback > Playback Resolution).\n"
              "Full: every frame at full resolution; frames are skipped if the computer "
              "cannot keep up.\n"
              "Half / Quarter: render at 1/2 or 1/4 of the timeline resolution for smoother "
              "playback.\n"
              "Auto: Full, switching to Half only while playback cannot keep up (paused: Full).");
}

void PlaybackResolution::bindCombo(QComboBox* combo)
{
    {
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (Mode mode : modes()) {
            combo->addItem(shortLabel(mode), int(mode));
        }
        combo->setCurrentIndex(combo->findData(int(m_mode)));
    }
    combo->setToolTip(toolTip());
    connect(combo, &QComboBox::currentIndexChanged, this, [this, combo](int index) {
        if (index >= 0) {
            setMode(Mode(combo->itemData(index).toInt()));
        }
    });
    connect(this, &PlaybackResolution::changed, combo, [combo](Mode mode) {
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(combo->findData(int(mode)));
    });
}

void PlaybackResolution::connectActions(ActionRegistry* actions)
{
    if (!actions) {
        return;
    }
    for (Mode mode : modes()) {
        const QString id = actionId(mode);
        if (!actions->contains(id)) {
            continue;
        }
        QAction* action = actions->action(id);
        action->setCheckable(true);
        action->setChecked(mode == m_mode);
        action->setStatusTip(toolTip().section(QLatin1Char('\n'), 0, 0));
        connect(action, &QAction::triggered, this, [this, mode] { setMode(mode); });
        connect(this, &PlaybackResolution::changed, action,
            [action, mode](Mode current) { action->setChecked(current == mode); });
    }
}

} // namespace kk
