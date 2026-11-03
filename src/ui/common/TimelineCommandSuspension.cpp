// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineCommandSuspension implementation.
#include "ui/common/TimelineCommandSuspension.h"

#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QEvent>
#include <QVariant>
#include <QWidget>

namespace kk {

namespace {

/// Dynamic property of a QAction: how many helpers hold it disabled (absent = none).
constexpr char kCountProperty[] = "kkTimelineSuspensions";

} // namespace

TimelineCommandSuspension::TimelineCommandSuspension(
    QWidget* page, ActionRegistry* registry, const QStringList& kept)
    : QObject(page)
    , m_registry(registry)
    , m_kept(kept)
{
    if (page)
        page->installEventFilter(this);
}

TimelineCommandSuspension::~TimelineCommandSuspension()
{
    resume();
}

const QStringList& TimelineCommandSuspension::alwaysKept()
{
    static const QStringList kKept {QStringLiteral("edit.undo"), QStringLiteral("edit.redo"),
        QStringLiteral("edit.history_window"), QStringLiteral("edit.keyboard_customization"),
        QStringLiteral("edit.preferences"), QStringLiteral("timeline.audio_scrubbing")};
    return kKept;
}

QStringList TimelineCommandSuspension::commandIds(
    const ActionRegistry& registry, const QStringList& kept)
{
    // Menus whose commands edit the timeline (submenus included; submenu markers such as
    // ">cut" are no ids and the Cut / Compositing page commands are switched by their pages).
    static const char* const kMenus[] = {"edit", "trim", "timeline", "timeline.tracks", "clip",
        "clip.sync", "clip.multicam", "clip.masks", "mark"};
    QStringList ids;
    for (const char* menu : kMenus) {
        for (const QString& id : registry.menuEntries(QLatin1String(menu))) {
            if (registry.contains(id) && !alwaysKept().contains(id) && !kept.contains(id)
                && !ids.contains(id))
                ids.append(id);
        }
    }
    return ids;
}

QStringList TimelineCommandSuspension::commandIds() const
{
    return m_registry ? commandIds(*m_registry, m_kept) : QStringList();
}

void TimelineCommandSuspension::suspend()
{
    if (m_suspendedState || !m_registry)
        return;
    m_suspendedState = true;
    for (const QString& id : commandIds()) {
        QAction* a = m_registry->action(id);
        const int count = a->property(kCountProperty).toInt();
        if (count > 0) {
            // Another page's helper has it off (a page switch may show the next page before
            // hiding the previous one): share it, so the first resume does not enable it.
            a->setProperty(kCountProperty, count + 1);
            m_suspended.append(a);
        } else if (a->isEnabled()) {
            a->setEnabled(false);
            a->setProperty(kCountProperty, 1);
            m_suspended.append(a);
        }
    }
}

void TimelineCommandSuspension::resume()
{
    for (const QPointer<QAction>& a : std::as_const(m_suspended)) {
        if (!a)
            continue;
        const int count = a->property(kCountProperty).toInt() - 1;
        a->setProperty(kCountProperty, count > 0 ? QVariant(count) : QVariant());
        if (count <= 0)
            a->setEnabled(true);
    }
    m_suspended.clear();
    m_suspendedState = false;
}

bool TimelineCommandSuspension::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parent() && !event->spontaneous()) {
        if (event->type() == QEvent::Show)
            suspend();
        else if (event->type() == QEvent::Hide)
            resume(); // a spontaneous hide is minimising: keep the state
    }
    return QObject::eventFilter(watched, event);
}

} // namespace kk
