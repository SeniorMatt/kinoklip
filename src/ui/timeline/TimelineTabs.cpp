// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineTabs: tab bar / drop-down of the project's timelines, bound to Project.
#include "ui/timeline/TimelineTabs.h"

#include "core/Project.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QTabBar>

namespace kk {

namespace {

constexpr int kMaxTabTextWidth = 220; ///< px; longer names are elided

QIcon timelineIcon(const TimelineData& data)
{
    return icon(data.compound ? QStringLiteral("compound_clip") : QStringLiteral("timeline"));
}

} // namespace

TimelineTabs::TimelineTabs(AppContext& ctx, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("TimelineTabs"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_bar = new QTabBar(this);
    m_bar->setObjectName(QStringLiteral("TimelineTabBar"));
    m_bar->setDocumentMode(true);
    m_bar->setMovable(true);
    m_bar->setExpanding(false);
    m_bar->setElideMode(Qt::ElideRight);
    m_bar->setUsesScrollButtons(true);
    m_bar->setFocusPolicy(Qt::NoFocus); // keys stay with the timeline
    m_bar->setContextMenuPolicy(Qt::CustomContextMenu);
    m_bar->setStyleSheet(QStringLiteral("QTabBar::tab { max-width: %1px; }").arg(kMaxTabTextWidth));
    layout->addWidget(m_bar, 1); // the tabs take the row (scroll buttons only when they must)

    m_combo = new QComboBox(this);
    m_combo->setObjectName(QStringLiteral("TimelineDropDown"));
    m_combo->setFocusPolicy(Qt::NoFocus);
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_combo->setToolTip(tr("Timeline shown in the timeline viewer and editor"));
    layout->addWidget(m_combo);
    // Fills the row in drop-down mode (hidden widgets do not take space).
    m_spacer = new QWidget(this);
    m_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(m_spacer, 1);

    connect(m_bar, &QTabBar::currentChanged, this, [this](int index) {
        if (m_syncing || index < 0)
            return;
        if (!m_ctx.project->setActiveTimeline(uidAt(index)))
            sync(); // refused (an edit is open): show the active one again
    });
    connect(m_bar, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (!m_syncing)
            m_ctx.project->moveTab(from, to);
    });
    connect(m_bar, &QTabBar::tabCloseRequested, this,
        [this](int index) { m_ctx.project->closeTab(uidAt(index)); });
    connect(m_bar, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int index = m_bar->tabAt(pos);
        if (index >= 0)
            showTabMenu(index, m_bar->mapToGlobal(pos));
    });
    connect(m_combo, &QComboBox::activated, this, [this](int index) {
        const quint64 uid = m_combo->itemData(index).toULongLong();
        if (!m_ctx.project->setActiveTimeline(uid))
            sync();
    });

    Project* project = ctx.project;
    connect(project, &Project::tabsChanged, this, &TimelineTabs::sync);
    connect(project, &Project::timelinesChanged, this, &TimelineTabs::sync);
    connect(project, &Project::activeTimelineChanged, this, &TimelineTabs::sync);
    connect(project, &Project::projectReset, this, &TimelineTabs::sync);
    setTabsMode(true);
}

void TimelineTabs::setTabsMode(bool tabs)
{
    m_tabsMode = tabs;
    m_bar->setVisible(tabs);
    m_combo->setVisible(!tabs);
    m_spacer->setVisible(!tabs);
    sync();
}

quint64 TimelineTabs::uidAt(int index) const
{
    return index >= 0 && index < m_bar->count() ? m_bar->tabData(index).toULongLong() : 0;
}

void TimelineTabs::sync()
{
    Project* project = m_ctx.project;
    const quint64 active = project->activeTimelineUid();
    m_syncing = true;

    // ---- Tabs ----
    const QVector<quint64>& tabs = project->tabs();
    bool sameOrder = m_bar->count() == tabs.size();
    for (int i = 0; sameOrder && i < m_bar->count(); ++i)
        sameOrder = uidAt(i) == tabs[i];
    if (!sameOrder) {
        // Recreate. Not reached during a tab drag: the bar reorders itself first and
        // moveTab() makes the project agree, so the orders match.
        while (m_bar->count() > 0)
            m_bar->removeTab(m_bar->count() - 1);
        for (const quint64 uid : tabs) {
            const int index = m_bar->addTab(QString());
            m_bar->setTabData(index, QVariant::fromValue<qulonglong>(uid));
        }
    }
    for (int i = 0; i < m_bar->count(); ++i) {
        const auto data = project->timelineData(uidAt(i));
        if (!data)
            continue;
        m_bar->setTabText(i, data->name);
        m_bar->setTabIcon(i, timelineIcon(*data));
        m_bar->setTabToolTip(i,
            data->compound ? tr("Compound clip \"%1\"").arg(data->name)
                           : tr("Timeline \"%1\"").arg(data->name));
        if (uidAt(i) == active)
            m_bar->setCurrentIndex(i);
    }
    m_bar->setTabsClosable(m_bar->count() > 1);

    // ---- Drop-down: every timeline, compound clips while they are open ----
    m_combo->clear();
    for (const quint64 uid : project->timelineUids()) {
        const auto data = project->timelineData(uid);
        if (!data || (data->compound && !tabs.contains(uid) && uid != active))
            continue;
        m_combo->addItem(timelineIcon(*data), data->name, QVariant::fromValue<qulonglong>(uid));
        if (uid == active)
            m_combo->setCurrentIndex(m_combo->count() - 1);
    }
    m_syncing = false;
}

void TimelineTabs::showTabMenu(int index, const QPoint& globalPos)
{
    const quint64 uid = uidAt(index);
    Project* project = m_ctx.project;
    QMenu menu(this);
    QAction* close
        = menu.addAction(tr("Close Tab"), this, [project, uid] { project->closeTab(uid); });
    close->setEnabled(m_bar->count() > 1);
    QAction* others = menu.addAction(tr("Close Other Tabs"), this, [project, uid] {
        if (!project->setActiveTimeline(uid))
            return;
        const QVector<quint64> tabs = project->tabs();
        for (const quint64 other : tabs) {
            if (other != uid)
                project->closeTab(other);
        }
    });
    others->setEnabled(m_bar->count() > 1);
    menu.addSeparator();
    menu.addAction(tr("Rename Timeline…"), this, [this, uid] { emit renameRequested(uid); });
    menu.addAction(tr("Duplicate Timeline"), this, [project, uid] {
        if (const quint64 copy = project->duplicateTimeline(uid))
            project->openTab(copy);
    });
    menu.addAction(tr("Timeline Settings…"), this, [this, uid] { emit settingsRequested(uid); });
    menu.exec(globalPos);
}

} // namespace kk
