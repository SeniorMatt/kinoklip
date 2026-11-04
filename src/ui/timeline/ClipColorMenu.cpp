// SPDX-License-Identifier: GPL-3.0-or-later
// ClipColorMenu implementation.
#include "ui/timeline/ClipColorMenu.h"

#include "core/ClipColors.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/MediaUsage.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"

#include <QCoreApplication>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSettings>

namespace kk {

namespace {

constexpr int kSwatchSize = 12; ///< px

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::ClipColorMenu", text);
}

QIcon swatch(const QColor& color)
{
    QPixmap pm(kSwatchSize, kSwatchSize);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setPen(color.darker(160));
    p.setBrush(color);
    p.drawRect(0, 0, kSwatchSize - 1, kSwatchSize - 1);
    return QIcon(pm);
}

/// Sets the label colour of @p clipIds (invalid colour = clear) in one undo step.
void setLabel(Project* project, const QVector<quint64>& clipIds, const QColor& color)
{
    project->beginEdit(color.isValid() ? tr("Set Clip Color") : tr("Clear Clip Color"));
    TimelineData& data = project->timeline()->mutableData();
    for (const quint64 id : clipIds) {
        const std::optional<editops::ClipLocation> where = editops::findClip(data, id);
        const TrackData* track = where ? data.track(where->track) : nullptr;
        if (!track || track->locked)
            continue;
        if (ClipData* clip = editops::clipById(data, id))
            clip->label = color;
    }
    project->endEdit();
}

} // namespace

void addClipColorMenus(
    QMenu& menu, AppContext& ctx, const QVector<quint64>& clipIds, QObject* context)
{
    Project* project = ctx.project;
    const TimelineData& data = project->timeline()->data();
    QString shared;
    bool first = true;
    QStringList items; // media pool items of the clips (for flags)
    for (const quint64 id : clipIds) {
        const ClipData* clip = editops::clipById(data, id);
        if (!clip)
            continue;
        const QString colorId = clipColorIdOf(clip->label);
        if (first)
            shared = colorId;
        else if (shared != colorId)
            shared = QStringLiteral("-");
        first = false;
        const QString item = usage::itemIdOf(*clip, project->mediaPool()->state());
        if (!item.isEmpty() && !items.contains(item))
            items.append(item);
    }

    QMenu* colors = menu.addMenu(tr("Clip Color"));
    colors->setObjectName(QStringLiteral("TimelineClipColorMenu"));
    for (const PaletteColor& c : clipColorPalette()) {
        const QColor value(c.rgb);
        QAction* a = colors->addAction(swatch(value), clipColorName(QLatin1String(c.id)), context,
            [project, clipIds, value] { setLabel(project, clipIds, value); });
        a->setCheckable(true);
        a->setChecked(shared == QLatin1String(c.id));
    }
    colors->addSeparator();
    colors->addAction(
        tr("Clear Color"), context, [project, clipIds] { setLabel(project, clipIds, QColor()); });

    if (items.isEmpty())
        return;
    MediaPool* pool = project->mediaPool();
    quint32 all = ~0u;
    for (const QString& id : std::as_const(items))
        all &= pool->item(id) ? pool->item(id)->flags : 0u;
    QMenu* flags = menu.addMenu(icon(QStringLiteral("flag")), tr("Flags"));
    flags->setObjectName(QStringLiteral("TimelineFlagMenu"));
    for (int i = 0; i < int(flagPalette().size()); ++i) {
        const bool on = (all & (1u << i)) != 0;
        QAction* a
            = flags->addAction(swatch(flagColor(i)), flagName(i), context, [project, items, i, on] {
                  project->beginEdit(on ? tr("Remove Flag") : tr("Add Flag"));
                  for (const QString& id : items) {
                      if (const MediaItem* m = project->mediaPool()->item(id))
                          project->mediaPool()->setFlags(
                              id, on ? m->flags & ~(1u << i) : m->flags | (1u << i));
                  }
                  project->endEdit();
              });
        a->setCheckable(true);
        a->setChecked(on);
    }
    flags->addSeparator();
    flags->addAction(tr("Clear All Flags"), context, [project, items] {
        project->beginEdit(tr("Clear All Flags"));
        for (const QString& id : items)
            project->mediaPool()->setFlags(id, 0);
        project->endEdit();
    });
}

int toolBarFlag()
{
    const int index = flagIndex(QSettings().value(QLatin1String(prefs::kToolBarFlag)).toString());
    return index >= 0 ? index : 0;
}

bool toggleFlag(AppContext& ctx, const QVector<quint64>& clipIds, int flag)
{
    Project* project = ctx.project;
    if (!project || flag < 0 || flag >= int(flagPalette().size()))
        return false;
    const TimelineData& data = project->timeline()->data();
    QStringList items;
    for (const quint64 id : clipIds) {
        const ClipData* clip = editops::clipById(data, id);
        const QString item
            = clip ? usage::itemIdOf(*clip, project->mediaPool()->state()) : QString();
        if (!item.isEmpty() && !items.contains(item))
            items.append(item);
    }
    if (items.isEmpty())
        return false;
    const quint32 bit = 1u << flag;
    bool allOn = true;
    for (const QString& id : std::as_const(items)) {
        const MediaItem* m = project->mediaPool()->item(id);
        allOn = allOn && m && (m->flags & bit);
    }
    project->beginEdit(allOn ? tr("Remove Flag") : tr("Add Flag"));
    for (const QString& id : std::as_const(items)) {
        if (const MediaItem* m = project->mediaPool()->item(id))
            project->mediaPool()->setFlags(id, allOn ? m->flags & ~bit : m->flags | bit);
    }
    project->endEdit();
    return true;
}

} // namespace kk
