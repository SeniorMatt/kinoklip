// SPDX-License-Identifier: GPL-3.0-or-later
// Shared fixture for the panel tests: a project + selection + action registry, and helpers to
// place clips and to save widget screenshots when KK_SCREENSHOT_DIR is set.
#pragma once

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/StandardActions.h"

#include <QCoreApplication>
#include <QDir>
#include <QPixmap>
#include <QWidget>

namespace kk::test {

/// Everything main() creates that a panel needs.
struct PanelFixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;

    PanelFixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
    }

    /// Places a clip on a track (one undo step) and returns its id.
    quint64 addClip(TrackRef track, int64_t start, int64_t duration,
        ClipKind kind = ClipKind::Media, const QString& name = QStringLiteral("Clip"))
    {
        ClipData clip;
        clip.kind = kind;
        clip.name = name;
        clip.mediaId = QStringLiteral("m1");
        clip.start = start;
        clip.duration = duration;
        project.beginEdit(QStringLiteral("Add Clip"));
        const quint64 id = editops::overwriteClip(project.timeline()->mutableData(), track, clip);
        project.endEdit();
        return id;
    }

    /// The current state of clip @p id (asserting callers check for nullptr).
    [[nodiscard]] const ClipData* clip(quint64 id) const
    {
        return editops::clipById(project.timeline()->data(), id);
    }
};

/// Saves @p widget as <KK_SCREENSHOT_DIR>/<name>.png if the variable is set (no-op otherwise).
inline void saveScreenshot(QWidget& widget, const QString& name)
{
    const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    // Lay out and paint like on screen (scroll areas only settle after a real show/resize).
    widget.show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    widget.grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
}

} // namespace kk::test
