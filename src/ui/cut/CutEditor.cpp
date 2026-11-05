// SPDX-License-Identifier: GPL-3.0-or-later
// CutEditor implementation.
#include "ui/cut/CutEditor.h"

#include "core/MediaPool.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"

#include <QAction>

namespace kk {

namespace {

/// Registry action of each edit.
constexpr std::pair<CutEdit, const char*> kEditActions[] = {
    {CutEdit::SmartInsert, "cut.smart_insert"},
    {CutEdit::Append, "cut.append"},
    {CutEdit::RippleOverwrite, "cut.ripple_overwrite"},
    {CutEdit::CloseUp, "cut.close_up"},
    {CutEdit::PlaceOnTop, "cut.place_on_top"},
    {CutEdit::SourceOverwrite, "cut.source_overwrite"},
};

} // namespace

std::optional<CutEdit> CutEditor::editForAction(const QString& actionId)
{
    for (const auto& [edit, id] : kEditActions)
        if (actionId == QLatin1String(id))
            return edit;
    return std::nullopt;
}

CutEditor::CutEditor(AppContext& ctx, TimelineController* controller, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_controller(controller)
{ }

QString CutEditor::label(CutEdit edit)
{
    switch (edit) {
    case CutEdit::SmartInsert:
        return tr("Smart Insert");
    case CutEdit::Append:
        return tr("Append");
    case CutEdit::RippleOverwrite:
        return tr("Ripple Overwrite");
    case CutEdit::CloseUp:
        return tr("Close Up");
    case CutEdit::PlaceOnTop:
        return tr("Place on Top");
    case CutEdit::SourceOverwrite:
        return tr("Source Overwrite");
    }
    return {};
}

void CutEditor::connectActions(std::function<bool()> enabled)
{
    if (!m_ctx.actions)
        return;
    for (const auto& [edit, id] : kEditActions) {
        if (!m_ctx.actions->contains(QLatin1String(id)))
            continue;
        const CutEdit e = edit;
        connect(m_ctx.actions->action(QLatin1String(id)), &QAction::triggered, this,
            [this, e, enabled] {
                if (!enabled || enabled())
                    run(e);
            });
    }
}

cutedits::SourceGroups CutEditor::groupsFor(const QVector<SourceSelection>& material) const
{
    cutedits::SourceGroups groups;
    int64_t offset = 0;
    for (const SourceSelection& s : material) {
        QVector<editops::PlacedClip> clips = m_controller->makeMediaClips(s.mediaId, s.inUs,
            s.outUs, m_controller->targetVideoTrack(), m_controller->targetAudioTrack());
        if (clips.isEmpty())
            continue;
        const int64_t frames = clips.front().clip.duration;
        for (editops::PlacedClip& p : clips)
            p.clip.start = offset;
        offset += frames;
        groups.append(clips);
    }
    return groups;
}

bool CutEditor::run(CutEdit edit)
{
    return run(edit, m_material ? m_material() : QVector<SourceSelection> {});
}

bool CutEditor::run(CutEdit edit, const QVector<SourceSelection>& material)
{
    const cutedits::SourceGroups groups = groupsFor(material);
    if (groups.isEmpty()) {
        emit editRefused(material.isEmpty()
                ? tr("Nothing to edit: open a clip, mark a range on the source tape or select "
                     "clips in the media pool.")
                : tr("Nothing to edit: no destination track for this clip's video or audio."));
        return false;
    }
    Project* project = m_ctx.project;
    Timeline* timeline = project->timeline();
    const int64_t playhead = timeline->playhead();

    // Source Overwrite: where the source lines up by timecode (the first range decides).
    std::optional<int64_t> overwriteStart;
    if (edit == CutEdit::SourceOverwrite) {
        const MediaPool* pool = project->mediaPool();
        const Rational fps = timeline->data().fps;
        const cutedits::TimecodeLookup timecode
            = [pool, fps](const QString& id) -> std::optional<int64_t> {
            const MediaItem* item = pool->item(id);
            if (!item)
                return std::nullopt;
            const Rational rate = item->info.frameRate.isValid() ? item->info.frameRate : fps;
            if (item->attributes.overrideTimecode)
                return framesToUs(item->attributes.startTimecodeFrames, rate);
            return multicam::timecodeToUs(item->info.startTimecode, rate);
        };
        const SourceSelection& first = material.front();
        overwriteStart = cutedits::sourceOverwriteStart(
            timeline->data(), playhead, first.mediaId, first.inUs, first.outUs, timecode);
        if (!overwriteStart) {
            emit editRefused(tr("Source Overwrite needs a clip under the playhead and a source "
                                "whose timecode covers the same moment."));
            return false;
        }
    }

    cutedits::Result result;
    project->beginEdit(label(edit));
    TimelineData& data = timeline->mutableData();
    switch (edit) {
    case CutEdit::SmartInsert:
        result = cutedits::smartInsert(data, groups, playhead);
        break;
    case CutEdit::Append:
        result = cutedits::append(data, groups);
        break;
    case CutEdit::RippleOverwrite:
        result = cutedits::rippleOverwrite(data, groups, playhead);
        break;
    case CutEdit::CloseUp:
        result = cutedits::closeUp(data, groups, playhead);
        break;
    case CutEdit::PlaceOnTop:
        result = cutedits::placeOnTop(data, groups, playhead);
        break;
    case CutEdit::SourceOverwrite:
        result = cutedits::sourceOverwrite(data, groups, *overwriteStart);
        break;
    }
    if (!result.ok()) {
        project->cancelEdit();
        emit editRefused(edit == CutEdit::RippleOverwrite
                ? tr("Ripple Overwrite needs a clip under the playhead.")
                : tr("%1 could not place the clip (locked tracks?).").arg(label(edit)));
        return false;
    }
    project->endEdit();
    m_ctx.selection->setClips(m_controller->videoClipCurrent(result.ids));
    m_controller->setPlayheadAfterEdit(result.end);
    emit edited(edit);
    return true;
}

bool CutEditor::handleSourceEdit(SourceEditMode mode)
{
    switch (mode) {
    case SourceEditMode::Insert:
        run(CutEdit::SmartInsert);
        return true;
    case SourceEditMode::AppendAtEnd:
        run(CutEdit::Append);
        return true;
    case SourceEditMode::RippleOverwrite:
        run(CutEdit::RippleOverwrite);
        return true;
    case SourceEditMode::PlaceOnTop:
        run(CutEdit::PlaceOnTop);
        return true;
    case SourceEditMode::Overwrite:
    case SourceEditMode::Replace:
    case SourceEditMode::FitToFill:
        break;
    }
    // The Edit page's edit, with the Cut viewer's (first) range.
    const QVector<SourceSelection> material
        = m_material ? m_material() : QVector<SourceSelection> {};
    if (!material.isEmpty())
        m_controller->insertSource(
            material.front().mediaId, material.front().inUs, material.front().outUs, mode);
    return true;
}

} // namespace kk
