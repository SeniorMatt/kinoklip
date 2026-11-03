// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerPanel, part: Timeline mode editing aids — the edit overlay for media dragged onto
// the viewer (spec 4.14), the two-up trim view (spec 8.15) and the typed timecode display
// (spec 2.10).
#include "core/EditBehaviour.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragAndDrop.h"
#include "ui/common/DragMime.h"
#include "ui/viewer/EditOverlay.h"
#include "ui/viewer/TrimTwoUpView.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QDragEnterEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMimeData>

namespace kk {

namespace {

/// The edits offered by the overlay, in tile order (Resolve's set, our layout).
struct EditTileDef
{
    const char* actionId;
    const char* label;
};
constexpr EditTileDef kEditTiles[] = {
    {"edit.insert", QT_TRANSLATE_NOOP("kk::ViewerPanel", "Insert")},
    {"edit.overwrite", QT_TRANSLATE_NOOP("kk::ViewerPanel", "Overwrite")},
    {"edit.replace", QT_TRANSLATE_NOOP("kk::ViewerPanel", "Replace")},
    {"edit.fit_to_fill", QT_TRANSLATE_NOOP("kk::ViewerPanel", "Fit to Fill")},
    {"edit.place_on_top", QT_TRANSLATE_NOOP("kk::ViewerPanel", "Place on Top")},
    {"edit.append_at_end", QT_TRANSLATE_NOOP("kk::ViewerPanel", "Append at End")},
    {"edit.ripple_overwrite", QT_TRANSLATE_NOOP("kk::ViewerPanel", "Ripple Overwrite")},
};

/// The sources a drag carries: a source viewer range, or media pool items (whole clips).
QList<SourceRange> draggedSources(const QMimeData* mime)
{
    QList<SourceRange> sources;
    if (mime->hasFormat(QLatin1String(mime::SourceRange))) {
        const QJsonObject o
            = QJsonDocument::fromJson(mime->data(QLatin1String(mime::SourceRange))).object();
        sources.append({o.value(QLatin1String("mediaId")).toString(),
            int64_t(o.value(QLatin1String("inUs")).toDouble()),
            int64_t(o.value(QLatin1String("outUs")).toDouble())});
    } else if (mime->hasFormat(QLatin1String(mime::MediaItems))) {
        const QString text = QString::fromUtf8(mime->data(QLatin1String(mime::MediaItems)));
        for (const QString& id : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            sources.append({id.trimmed(), 0, 0});
    }
    return sources;
}

} // namespace

void ViewerPanel::setupEditingAids()
{
    m_editOverlay = new EditOverlay(m_surface);
    m_twoUp = new TrimTwoUpView(m_surface, m_ctx.project ? m_ctx.project->mediaPool() : nullptr);
    // The timeline viewer can picture the two sides with their program look (spec 8.19).
    if (m_mode == Mode::Timeline && m_ctx.project)
        m_twoUp->setProgramSource(m_ctx.playback, m_ctx.project->timeline());
    updateEditTiles();
    if (m_ctx.actions)
        connect(
            m_ctx.actions, &ActionRegistry::shortcutsChanged, this, [this] { updateEditTiles(); });
}

void ViewerPanel::setEditOverlayEnabled(bool enabled)
{
    m_editOverlayEnabled = enabled && m_editOverlay;
    setAcceptDrops(m_editOverlayEnabled);
}

QMenu* ViewerPanel::buildContextMenu(QWidget* parent)
{
    // Resolve's viewer menu at a basic level: transport, marks, markers, the viewer modes.
    // Registry commands only (keys shown, enabled state kept by their owners).
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("ViewerContextMenu"));
    if (!m_ctx.actions)
        return menu;
    const bool timeline = m_mode == Mode::Timeline;
    const std::initializer_list<const char*> groups[] = {
        {"transport.play_pause", "transport.play_in_out", "transport.loop"},
        {"mark.in", "mark.out", "mark.clear_both", "mark.go_to_in", "mark.go_to_out"},
        {timeline ? "mark.add_marker" : nullptr, timeline ? "clip.match_frame" : nullptr},
        {"view.single_viewer", "view.toggle_source_timeline", "view.fullscreen_viewer"},
    };
    for (const auto& group : groups) {
        if (!menu->isEmpty())
            menu->addSeparator();
        for (const char* id : group)
            if (id && m_ctx.actions->contains(QLatin1String(id)))
                menu->addAction(m_ctx.actions->action(QLatin1String(id)));
    }
    // Viewer aids (spec 41): rulers, grid and the Guides / Safe Area submenus, built from
    // the registry's View categories like the menu bar.
    menu->addSeparator();
    for (const char* id : {"view.show_rulers", "view.grid_show"})
        if (m_ctx.actions->contains(QLatin1String(id)))
            menu->addAction(m_ctx.actions->action(QLatin1String(id)));
    const std::pair<const char*, QString> subs[]
        = {{"view.guides", tr("Guides")}, {"view.safe_area", tr("Safe Area")}};
    for (const auto& [category, title] : subs) {
        QMenu* sub = menu->addMenu(title);
        for (const QString& id : m_ctx.actions->menuEntries(QLatin1String(category))) {
            if (id == QLatin1String(ActionRegistry::kSeparator))
                sub->addSeparator();
            else if (m_ctx.actions->contains(id))
                sub->addAction(m_ctx.actions->action(id));
        }
        if (sub->isEmpty())
            menu->removeAction(sub->menuAction());
    }
    if (m_ctx.actions->contains(QStringLiteral("playback.resolution"))) {
        menu->addSeparator();
        QAction* res = m_ctx.actions->action(QStringLiteral("playback.resolution"));
        if (res->menu())
            menu->addMenu(res->menu());
    }
    return menu;
}

void ViewerPanel::showContextMenu(const QPoint& globalPos)
{
    QMenu* menu = buildContextMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(globalPos);
}

void ViewerPanel::setEditTileActions(const QStringList& actionIds)
{
    m_editTileActions = actionIds;
    if (m_editOverlay)
        updateEditTiles();
}

void ViewerPanel::updateEditTiles()
{
    QVector<std::pair<QString, QString>> defs; // action id, label
    if (m_editTileActions.isEmpty()) {
        for (const EditTileDef& def : kEditTiles)
            defs.append({QLatin1String(def.actionId), tr(def.label)});
    } else {
        for (const QString& id : m_editTileActions) {
            QString label = id;
            if (m_ctx.actions && m_ctx.actions->contains(id))
                label = m_ctx.actions->action(id)->text().remove(QLatin1Char('&'));
            defs.append({id, label});
        }
    }
    QVector<EditOverlay::Tile> tiles;
    for (const auto& [actionId, label] : defs) {
        EditOverlay::Tile tile;
        tile.actionId = actionId;
        tile.label = label;
        if (m_ctx.actions && m_ctx.actions->contains(tile.actionId)) {
            const QList<QKeySequence> keys = m_ctx.actions->shortcuts(tile.actionId);
            if (!keys.isEmpty())
                tile.key = keys.front().toString(QKeySequence::NativeText);
        }
        tiles.append(tile);
    }
    m_editOverlay->setTiles(tiles);
}

void ViewerPanel::setTimecodeEntryText(const QString& text)
{
    m_timecodeEntry = text;
    m_timecode->setProperty("entry", !text.isEmpty());
    if (!text.isEmpty()) {
        m_timecode->setText(text);
        m_timecode->setToolTip(tr("Typed timecode: Enter to go there, Esc to cancel"));
    } else {
        m_timecode->setToolTip(tr("Current timecode"));
        onPositionChanged(engine() ? engine()->position()
                : m_boundTimeline  ? m_boundTimeline->playhead()
                                   : 0);
    }
    // A frame around the typed text marks the entry mode.
    m_timecode->setFrameShape(text.isEmpty() ? QFrame::NoFrame : QFrame::Box);
}

void ViewerPanel::setTrimPreview(const TrimPreview& preview)
{
    if (!m_twoUp)
        return;
    // The picture mode is read when a trim starts (not per mouse move).
    if (preview.isActive() && !m_twoUp->preview().isActive())
        m_twoUp->setPictures(EditBehaviour::fromSettings().twoUpProgramLook
                ? TrimTwoUpView::Pictures::Program
                : TrimTwoUpView::Pictures::Source);
    m_twoUp->setPreview(preview, m_fps);
}

void ViewerPanel::dragEnterEvent(QDragEnterEvent* event)
{
    logDropEvent("timeline viewer", event);
    if (m_editOverlayEnabled && !draggedSources(event->mimeData()).isEmpty()) {
        m_editOverlay->refresh();
        m_editOverlay->show();
        m_editOverlay->raise();
        m_editOverlay->setHover(m_editOverlay->mapFrom(this, event->position().toPoint()));
        event->acceptProposedAction();
        return;
    }
    PanelFrame::dragEnterEvent(event);
}

void ViewerPanel::dragMoveEvent(QDragMoveEvent* event)
{
    if (m_editOverlay && m_editOverlay->isVisible()) {
        m_editOverlay->setHover(m_editOverlay->mapFrom(this, event->position().toPoint()));
        // Only a tile takes the drop; between tiles the cursor says "no".
        if (m_editOverlay->hovered() >= 0)
            event->acceptProposedAction();
        else
            event->ignore();
        return;
    }
    PanelFrame::dragMoveEvent(event);
}

void ViewerPanel::dragLeaveEvent(QDragLeaveEvent* event)
{
    if (m_editOverlay)
        m_editOverlay->hide();
    PanelFrame::dragLeaveEvent(event);
}

void ViewerPanel::dropEvent(QDropEvent* event)
{
    logDropEvent("timeline viewer", event);
    if (!m_editOverlay || !m_editOverlay->isVisible()) {
        PanelFrame::dropEvent(event);
        return;
    }
    const int tile
        = m_editOverlay->tileAt(m_editOverlay->mapFrom(this, event->position().toPoint()));
    m_editOverlay->hide();
    const QList<SourceRange> sources = draggedSources(event->mimeData());
    if (tile < 0 || sources.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    activate();
    Q_EMIT sourceEditDropped(m_editOverlay->tiles()[tile].actionId, sources);
}

} // namespace kk
