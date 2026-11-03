// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot geometry sidecar (see ScreenshotGeometry.h).
#include "app/ScreenshotGeometry.h"

#include "core/EditOps.h"
#include "core/TimelineData.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTabBar>
#include <QToolButton>
#include <QWidget>

#include <cmath>

namespace kk {

namespace {

QJsonArray rectJson(const QRect& r)
{
    return {r.x(), r.y(), r.width(), r.height()};
}

/// True when @p w and all its parents up to @p root are shown.
bool shownIn(const QWidget* w, const QWidget* root)
{
    for (const QWidget* p = w; p && p != root; p = p->parentWidget()) {
        if (!p->isVisible())
            return false;
    }
    return true;
}

/// @p r (in @p w's coordinates) in the picture, clipped to the visible part of @p w.
QRect toPicture(const QWidget* w, const QWidget* root, QRect r, QPoint offset)
{
    r &= w->rect();
    if (r.isEmpty())
        return {};
    return QRect(w->mapTo(root, r.topLeft()), r.size()).translated(offset);
}

void addViewItems(QJsonArray& out, const QAbstractItemView* view, const QWidget* root,
    QPoint offset, const QModelIndex& parent, int depth)
{
    const QAbstractItemModel* model = view->model();
    if (!model || depth > 8)
        return;
    const QWidget* vp = view->viewport();
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        const QRect r = toPicture(vp, root, view->visualRect(index), offset);
        if (!r.isEmpty()) {
            QJsonObject o {{QStringLiteral("kind"), QStringLiteral("item")},
                {QStringLiteral("text"), index.data(Qt::DisplayRole).toString()},
                {QStringLiteral("view"), view->objectName()},
                {QStringLiteral("rect"), rectJson(r)}};
            const QVariant data = index.data(Qt::UserRole);
            if (data.typeId() == QMetaType::QString && !data.toString().isEmpty())
                o.insert(QStringLiteral("data"), data.toString());
            out.append(o);
        }
        if (model->hasChildren(index))
            addViewItems(out, view, root, offset, index, depth + 1);
    }
}

void addTimelineClips(QJsonArray& out, TimelineWidget* widget, const QWidget* root, QPoint offset,
    const TimelineData& data, int64_t playhead)
{
    const TimelineLayout& layout = widget->layout();
    const QWidget* vp = widget->viewport();
    auto addTracks = [&](const std::vector<TrackData>& tracks, TrackType type,
                         const QString& letter) {
        for (int t = 0; t < int(tracks.size()); ++t) {
            const int row = layout.rowOf(TrackRef {type, t});
            if (row < 0)
                continue;
            const QString label = letter + QString::number(t + 1);
            const QRect header = toPicture(vp, root,
                QRect(0, layout.rowRect(row).y(), timelinemetrics::kHeaderWidth,
                    layout.rowRect(row).height()),
                offset);
            if (!header.isEmpty())
                out.append(QJsonObject {{QStringLiteral("kind"), QStringLiteral("track")},
                    {QStringLiteral("track"), label}, {QStringLiteral("rect"), rectJson(header)}});
            int index = 0;
            for (const ClipData& clip : tracks[size_t(t)].clips) {
                const QRect r
                    = toPicture(vp, root, layout.clipRect(row, clip).toAlignedRect(), offset);
                if (!r.isEmpty())
                    out.append(QJsonObject {{QStringLiteral("kind"), QStringLiteral("clip")},
                        {QStringLiteral("track"), label}, {QStringLiteral("index"), index},
                        {QStringLiteral("text"), clip.name},
                        {QStringLiteral("rect"), rectJson(r)}});
                // Its transitions: the frames they cover, the clip's height.
                for (const bool head : {true, false}) {
                    const auto window = editops::transitionWindow(data, clip.id, head);
                    if (!window)
                        continue;
                    const QRectF c = layout.clipRect(row, clip);
                    const double x0 = layout.frameToX(double(window->first));
                    const double x1 = layout.frameToX(double(window->second));
                    const QRect t = toPicture(
                        vp, root, QRectF(x0, c.top(), x1 - x0, c.height()).toAlignedRect(), offset);
                    if (!t.isEmpty())
                        out.append(QJsonObject {
                            {QStringLiteral("kind"), QStringLiteral("transition")},
                            {QStringLiteral("track"), label}, {QStringLiteral("index"), index},
                            {QStringLiteral("head"), head}, {QStringLiteral("rect"), rectJson(t)}});
                }
                ++index;
            }
        }
    };
    addTracks(data.videoTracks, TrackType::Video, QStringLiteral("V"));
    addTracks(data.audioTracks, TrackType::Audio, QStringLiteral("A"));
    addTracks(data.subtitleTracks, TrackType::Subtitle, QStringLiteral("ST"));
    if (playhead >= 0) {
        const int x = int(std::lround(layout.frameToX(double(playhead))));
        const QRect r = toPicture(vp, root, QRect(x, 0, 1, vp->height()), offset);
        if (!r.isEmpty())
            out.append(QJsonObject {{QStringLiteral("kind"), QStringLiteral("playhead")},
                {QStringLiteral("rect"), rectJson(r)}});
    }
}

} // namespace

bool writeScreenshotGeometry(QWidget* root, const QString& jsonPath, QPoint offset,
    const TimelineData* timeline, int64_t playhead)
{
    QJsonArray out;
    QList<QWidget*> widgets = root->findChildren<QWidget*>();
    widgets.prepend(root);
    for (QWidget* w : widgets) {
        if (!shownIn(w, root))
            continue;
        QJsonObject o;
        if (!w->objectName().isEmpty())
            o.insert(QStringLiteral("name"), w->objectName());
        if (auto* tool = qobject_cast<QToolButton*>(w);
            tool && tool->defaultAction() && !tool->defaultAction()->objectName().isEmpty())
            o.insert(QStringLiteral("action"), tool->defaultAction()->objectName());
        if (auto* button = qobject_cast<QAbstractButton*>(w); button && !button->text().isEmpty())
            o.insert(QStringLiteral("text"), button->text());
        const QLatin1String className(w->metaObject()->className());
        if (!o.isEmpty() || className.endsWith(QLatin1String("Panel"))) {
            const QRect r = toPicture(w, root, w->rect(), offset);
            if (!r.isEmpty()) {
                o.insert(QStringLiteral("kind"), QStringLiteral("widget"));
                o.insert(QStringLiteral("class"), className);
                o.insert(QStringLiteral("rect"), rectJson(r));
                out.append(o);
            }
        }
        if (auto* tabs = qobject_cast<QTabBar*>(w)) {
            for (int i = 0; i < tabs->count(); ++i) {
                const QRect r = toPicture(tabs, root, tabs->tabRect(i), offset);
                if (!r.isEmpty())
                    out.append(QJsonObject {{QStringLiteral("kind"), QStringLiteral("tab")},
                        {QStringLiteral("text"), tabs->tabText(i)},
                        {QStringLiteral("rect"), rectJson(r)}});
            }
        }
        if (auto* view = qobject_cast<QAbstractItemView*>(w))
            addViewItems(out, view, root, offset, view->rootIndex(), 0);
    }
    if (timeline) {
        QList<TimelinePanel*> panels = root->findChildren<TimelinePanel*>();
        if (auto* self = qobject_cast<TimelinePanel*>(root))
            panels.prepend(self);
        for (TimelinePanel* panel : panels) {
            if (shownIn(panel, root) && panel->timelineWidget())
                addTimelineClips(out, panel->timelineWidget(), root, offset, *timeline, playhead);
        }
    }
    QFile file(jsonPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QJsonObject doc {{QStringLiteral("width"), root->width()},
        {QStringLiteral("height"), root->height()}, {QStringLiteral("entries"), out}};
    return file.write(QJsonDocument(doc).toJson(QJsonDocument::Compact)) > 0;
}

} // namespace kk
