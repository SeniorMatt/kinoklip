// SPDX-License-Identifier: GPL-3.0-or-later
// CutEditor — runs the Cut page's smart edits (core/CutEdits.h) on the active timeline, one undo
// step each, with the material of the Cut viewer.
#pragma once

#include "core/CutEdits.h"
#include "ui/timeline/TimelineController.h"

#include <QObject>
#include <QVector>

#include <functional>

namespace kk {

struct AppContext;

/// The six smart edit buttons of the Cut page.
enum class CutEdit {
    SmartInsert,
    Append,
    RippleOverwrite,
    CloseUp,
    PlaceOnTop,
    SourceOverwrite
};

/// Executes CutEdit commands.
///
/// The material comes from a provider (the page: the Cut viewer's source clip or source tape
/// range, else the media pool selection); each range becomes one group of clips built by
/// TimelineController::makeMediaClips() for the controller's destination tracks, placed back to
/// back. Every command is one undo step (Project::beginEdit()/endEdit()); afterwards the new
/// clips are selected and the playhead goes to the end of the placed material, as in Resolve.
///
/// Keyboard: connectActions() binds the cut.* edit actions; source edit keys of the Edit page
/// (F9, Shift+F10, F12, Shift+F12) reach run() through TimelineController::setSourceEditHandler()
/// while the Cut page is shown (the page installs it).
///
/// Thread-safety: GUI thread only.
class CutEditor : public QObject
{
    Q_OBJECT
public:
    /// Source ranges for the next edit, in order (empty = nothing to edit).
    using MaterialProvider = std::function<QVector<SourceSelection>()>;

    /// Creates the editor for @p ctx, building clips with @p controller (not owned).
    CutEditor(AppContext& ctx, TimelineController* controller, QObject* parent = nullptr);

    /// Where the material comes from (the page wires the Cut viewer).
    void setMaterialProvider(MaterialProvider provider) { m_material = std::move(provider); }
    /// Connects the cut.* edit actions; @p enabled tells whether an action may run now (the
    /// page: while it is shown).
    void connectActions(std::function<bool()> enabled);

    /// Runs @p edit with the provider's material. Returns whether the timeline changed; when
    /// it did not, editRefused() tells why.
    bool run(CutEdit edit);
    /// Runs @p edit with explicit @p material (tests, the provider's result).
    bool run(CutEdit edit, const QVector<SourceSelection>& material);
    /// The source edit command @p mode as the Cut page understands it: Insert = Smart Insert,
    /// Append at End = Append, Ripple Overwrite and Place on Top = their Cut versions; the
    /// others (Overwrite, Replace, Fit to Fill) use the controller's edit with the first range.
    /// Returns whether the command was taken (also when it changed nothing).
    bool handleSourceEdit(SourceEditMode mode);

    /// The clip groups for @p material on the controller's destination tracks, back to back
    /// from offset 0 (exposed for tests).
    [[nodiscard]] cutedits::SourceGroups groupsFor(const QVector<SourceSelection>& material) const;

    /// Undo label of @p edit ("Smart Insert", ...).
    [[nodiscard]] static QString label(CutEdit edit);

signals:
    /// An edit did nothing; @p reason is a short sentence for the user.
    void editRefused(const QString& reason);
    /// An edit placed clips (the page may switch the viewer to the timeline).
    void edited(kk::CutEdit edit);

private:
    AppContext& m_ctx;
    TimelineController* m_controller = nullptr;
    MaterialProvider m_material;
};

} // namespace kk
