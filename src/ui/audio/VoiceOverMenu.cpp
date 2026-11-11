// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/audio/VoiceOverMenu.h"

#include "ui/audio/VoiceOverController.h"

#include <QActionGroup>
#include <QCoreApplication>
#include <QMenu>

namespace kk {

namespace {
QString tr(const char* text)
{
    return QCoreApplication::translate("kk::VoiceOverMenu", text);
}
} // namespace

void fillVoiceOverInputMenu(QMenu& menu, VoiceOverController* controller, quint64 trackId)
{
    if (!controller) {
        menu.addAction(tr("Voice-over is not available"))->setEnabled(false);
        return;
    }
    const QList<AudioInputDevice> devices = controller->inputs();
    if (devices.isEmpty()) {
        menu.addAction(tr("No microphone found"))->setEnabled(false);
        return;
    }
    auto* group = new QActionGroup(&menu);
    const bool own = controller->hasOwnInput(trackId);
    const QByteArray chosen = controller->inputFor(trackId);
    QAction* def = menu.addAction(tr("Default (%1)").arg(devices.first().description));
    def->setCheckable(true);
    def->setChecked(!own);
    group->addAction(def);
    QObject::connect(def, &QAction::triggered, controller,
        [controller, trackId] { controller->setInputFor(trackId, {}); });
    menu.addSeparator();
    for (const AudioInputDevice& d : devices) {
        QAction* a = menu.addAction(d.description);
        a->setCheckable(true);
        a->setChecked(own && d.id == chosen);
        group->addAction(a);
        const QByteArray id = d.id;
        QObject::connect(a, &QAction::triggered, controller,
            [controller, trackId, id] { controller->setInputFor(trackId, id); });
    }
}

QString voiceOverArmTooltip(const VoiceOverController* controller, quint64 trackId)
{
    if (!controller)
        return tr("Record arm: voice-over is not available here.");
    const QString problem = controller->problem();
    if (!problem.isEmpty())
        return tr("Voice-over cannot be used: %1").arg(problem);
    const QString input = controller->inputName(trackId);
    if (controller->isArmed(trackId))
        return tr("Armed for voice-over (input: %1). Click to disarm; right-click to choose "
                  "another input.")
            .arg(input);
    return tr("Arm this track for voice-over (input: %1). Then press Record to count in and "
              "record while the timeline plays. Right-click to choose the input.")
        .arg(input);
}

} // namespace kk
