// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/audio/VimMixerHost.h"

#include "ui/audio/AudioScales.h"
#include "ui/audio/DynamicsWindow.h"
#include "ui/audio/EqWindow.h"
#include "ui/audio/FaderWidget.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/common/VimCursorFrame.h"

#include <QApplication>
#include <QInputDialog>
#include <QScrollArea>
#include <QToolButton>

#include <algorithm>

namespace kk {

VimMixerHost::VimMixerHost(ActionRegistry* actions)
    : VimPanelHost(actions)
{ }

VimMixerHost::~VimMixerHost()
{
    delete m_frame;
}

MixerStrip* VimMixerHost::current() const
{
    if (!m_panel || m_panel->strips().isEmpty())
        return nullptr;
    return m_panel->strips()[std::clamp(m_index, 0, int(m_panel->strips().size()) - 1)];
}

void VimMixerHost::focusChanged(QWidget* focus)
{
    for (QWidget* w = focus; w; w = w->parentWidget()) {
        if (auto* panel = qobject_cast<MixerPanel*>(w)) {
            m_panel = panel;
            show();
            return;
        }
    }
}

void VimMixerHost::setIndex(int index)
{
    if (!m_panel || m_panel->strips().isEmpty())
        return;
    m_index = std::clamp(index, 0, int(m_panel->strips().size()) - 1);
    show();
}

void VimMixerHost::setCursorVisible(bool visible)
{
    m_visible = visible;
    if (!visible && m_frame)
        m_frame->hide();
    else if (visible)
        show();
}

void VimMixerHost::show()
{
    MixerStrip* strip = current();
    if (!strip || !m_visible) {
        if (m_frame)
            m_frame->hide();
        return;
    }
    if (!m_frame)
        m_frame = new VimCursorFrame(strip);
    if (m_frame->parentWidget() != strip)
        m_frame->setParent(strip);
    m_frame->setGeometry(strip->rect());
    m_frame->show();
    m_frame->raise();
    for (QWidget* w = strip->parentWidget(); w; w = w->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(w)) {
            scroll->ensureWidgetVisible(strip, 8, 0);
            break;
        }
    }
}

bool VimMixerHost::hostCommand(const QString& name, int count, bool hasCount)
{
    if (!name.startsWith(QLatin1String("mix_")))
        return false;
    if (!m_panel)
        return true;
    MixerStrip* strip = current();
    if (!strip) {
        showMessage(QObject::tr("No strips"));
        return true;
    }
    const QString cmd = name.mid(4);
    const int n = std::max(1, count);
    bool ok = true;
    if (cmd == QLatin1String("left")) {
        setIndex(m_index - n);
    } else if (cmd == QLatin1String("right")) {
        setIndex(m_index + n);
    } else if (cmd == QLatin1String("first")) {
        setIndex(0);
    } else if (cmd == QLatin1String("last")) {
        setIndex(hasCount ? count - 1 : 1 << 20);
    } else if (cmd == QLatin1String("up") || cmd == QLatin1String("down")) {
        const double db = std::clamp(
            strip->fader()->value() + (cmd == QLatin1String("up") ? n : -n) * kFaderStepDb,
            kFaderMinDb, kFaderMaxDb);
        emit strip->fader()->valueRequested(db);
    } else if (cmd == QLatin1String("zero")) {
        emit strip->fader()->valueRequested(0.0);
    } else if (cmd == QLatin1String("pan_left") || cmd == QLatin1String("pan_right")) {
        PanKnob* pan = strip->panKnob();
        ok = pan != nullptr;
        if (pan)
            emit pan->valueRequested(std::clamp(
                pan->value() + (cmd == QLatin1String("pan_right") ? n : -n) * kPanStep, -1.0, 1.0));
    } else if (cmd == QLatin1String("mute")) {
        strip->muteButton()->click();
    } else if (cmd == QLatin1String("solo")) {
        ok = strip->soloButton() && strip->soloButton()->isEnabled();
        if (ok)
            strip->soloButton()->click();
    } else if (cmd == QLatin1String("arm")) {
        ok = strip->armButton() && strip->armButton()->isEnabled();
        if (ok)
            strip->armButton()->click();
    } else if (cmd == QLatin1String("type")) {
        bool accepted = false;
        const double db = QInputDialog::getDouble(QApplication::activeWindow(),
            QObject::tr("Fader"), QObject::tr("Level (dB):"), strip->fader()->value(), kFaderMinDb,
            kFaderMaxDb, 1, &accepted);
        if (accepted)
            emit strip->fader()->valueRequested(db);
    } else if (cmd == QLatin1String("eq")) {
        strip->openEq();
    } else if (cmd == QLatin1String("dynamics")) {
        strip->openDynamics();
    } else {
        return false;
    }
    if (!ok)
        showMessage(QObject::tr("This strip has no such control"));
    return true;
}

} // namespace kk
