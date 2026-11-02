// SPDX-License-Identifier: GPL-3.0-or-later
// ParamEditHost — what a parameter editor row needs from its owner (the InspectorPanel):
// clip lookup, playhead-relative frames and the undoable edit protocol.
#pragma once

#include "ui/inspector/ParamAccess.h"

#include <cstdint>
#include <functional>

class QColor;

namespace kk {

/// Services the inspector offers its editor rows. Keeping rows behind this interface means
/// they never talk to Project/Timeline directly, so the undo grouping lives in one place
/// (InspectorPanel) and rows can be tested with a fake host.
class ParamEditHost
{
public:
    virtual ~ParamEditHost() = default;

    /// Current state of clip @p clipId, or nullptr if it no longer exists.
    [[nodiscard]] virtual const ClipData* clip(quint64 clipId) const = 0;
    /// Clip-local frame of the playhead for clip @p clipId, clamped into the clip.
    [[nodiscard]] virtual int64_t localFrame(quint64 clipId) const = 0;

    /// Opens an undo group labelled @p label (nesting-safe). Must be paired with endParamEdit().
    virtual void beginParamEdit(const QString& label) = 0;
    /// Closes the undo group opened by beginParamEdit().
    virtual void endParamEdit() = 0;
    /// Applies @p mutate to clip @p clipId inside an open edit and notifies views live.
    virtual void modifyClip(quint64 clipId, const std::function<void(ClipData&)>& mutate) = 0;
    /// One-shot change: beginParamEdit(@p label), modifyClip(), endParamEdit() = one undo step.
    virtual void editClip(
        const QString& label, quint64 clipId, const std::function<void(ClipData&)>& mutate)
        = 0;

    /// Moves the playhead to clip-local frame @p localFrame of clip @p clipId.
    virtual void seekLocalFrame(quint64 clipId, int64_t localFrame) = 0;
    /// Remembers the parameter the user last touched (target of the Add Keyframe shortcut).
    virtual void setActiveParam(const ParamRef& ref, const ParamDescriptor& desc) = 0;
    /// True if the clips an edit of @p ref applies to (the selected clips of the same kind,
    /// spec 24.1) hold different values of it; the row then shows "mixed" instead of a value.
    [[nodiscard]] virtual bool isMixed(const ParamRef& ref, const ParamDescriptor& desc) const
    {
        (void)ref;
        (void)desc;
        return false;
    }
    /// Asks the user for a colour (modal). Returns an invalid colour if cancelled.
    [[nodiscard]] virtual QColor pickColor(const QColor& initial, const QString& title) = 0;
};

} // namespace kk
