// SPDX-License-Identifier: GPL-3.0-or-later
// The keyboard cursor of the keyframe views (Vim mode, spec section 83): a row and a timeline
// frame, moves over the keys of the rows, selecting, moving and deleting keys from the keyboard.
// The commands are the same functions the mouse and the menus use (KeyframeCommands,
// KeyframeOps), so every edit is one undo step.
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeViewBase.h"

#include <algorithm>
#include <cstdlib>

namespace kk {

namespace ko = keyframeops;
namespace kc = keyframecommands;

void KeyframeViewBase::placeCursor(int row, int64_t frame)
{
    const int rows = int(cursorRows().size());
    m_cursorRow = rows > 0 ? std::clamp(row, 0, rows - 1) : -1;
    m_cursorFrame = frame;
    update();
    emit cursorMoved(m_cursorRow);
}

void KeyframeViewBase::clearCursor()
{
    if (m_cursorRow < 0)
        return;
    m_cursorRow = -1;
    update();
}

QVector<int64_t> KeyframeViewBase::rowFrames(int row) const
{
    QVector<int64_t> frames;
    const QVector<QVector<ParamId>> rows = cursorRows();
    if (row < 0 || row >= rows.size())
        return frames;
    for (const ParamId& id : rows[row]) {
        const ClipData* c = clipData(id.clipId);
        const AnimatedValue* v = c ? ko::findValue(*c, id.path) : nullptr;
        if (!v)
            continue;
        for (const Keyframe& k : v->keys)
            frames.append(c->start + k.frame);
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

QVector<KeyframeRef> KeyframeViewBase::cursorKeys() const
{
    const QVector<QVector<ParamId>> rows = cursorRows();
    if (m_cursorRow < 0 || m_cursorRow >= rows.size())
        return {};
    return keysAtFrame(m_cursorFrame, rows[m_cursorRow]);
}

QVector<KeyframeRef> KeyframeViewBase::keyTargets() const
{
    const QVector<KeyframeRef> selected = selectedKeys();
    return selected.isEmpty() ? cursorKeys() : selected;
}

namespace {

/// The key frame of @p frames nearest to @p frame (@p frame itself when there is none).
int64_t nearest(const QVector<int64_t>& frames, int64_t frame)
{
    int64_t best = frame;
    int64_t bestDistance = INT64_MAX;
    for (const int64_t f : frames) {
        const int64_t d = std::llabs(f - frame);
        if (d < bestDistance) {
            best = f;
            bestDistance = d;
        }
    }
    return best;
}

} // namespace

bool KeyframeViewBase::moveCursorToRow(int row)
{
    const int rows = int(cursorRows().size());
    if (rows == 0)
        return false;
    row = std::clamp(row, 0, rows - 1);
    const int64_t from = m_cursorRow >= 0 ? m_cursorFrame : m_ctx.project->timeline()->playhead();
    placeCursor(row, nearest(rowFrames(row), from));
    return true;
}

bool KeyframeViewBase::moveCursorRows(int delta)
{
    return moveCursorToRow((m_cursorRow < 0 ? 0 : m_cursorRow) + delta);
}

bool KeyframeViewBase::moveCursorKeys(int delta)
{
    if (m_cursorRow < 0 && !moveCursorToRow(0))
        return false;
    const QVector<int64_t> frames = rowFrames(m_cursorRow);
    if (frames.isEmpty() || delta == 0)
        return false;
    // The key at or after (before) the cursor frame, then delta - 1 more.
    int index;
    if (delta > 0) {
        index = int(std::upper_bound(frames.begin(), frames.end(), m_cursorFrame) - frames.begin())
            + delta - 1;
    } else {
        index = int(std::lower_bound(frames.begin(), frames.end(), m_cursorFrame) - frames.begin())
            + delta;
    }
    index = std::clamp(index, 0, int(frames.size()) - 1);
    if (frames[index] == m_cursorFrame)
        return false;
    placeCursor(m_cursorRow, frames[index]);
    return true;
}

bool KeyframeViewBase::moveCursorToEdgeKey(bool last)
{
    if (m_cursorRow < 0 && !moveCursorToRow(0))
        return false;
    const QVector<int64_t> frames = rowFrames(m_cursorRow);
    if (frames.isEmpty())
        return false;
    placeCursor(m_cursorRow, last ? frames.last() : frames.first());
    return true;
}

bool KeyframeViewBase::selectCursor(bool toggle)
{
    const QVector<KeyframeRef> keys = cursorKeys();
    if (keys.isEmpty())
        return false;
    QVector<KeyframeRef> now = selectedKeys();
    bool allIn = true;
    for (const KeyframeRef& k : keys)
        allIn = allIn && now.contains(k);
    if (toggle && allIn) {
        for (const KeyframeRef& k : keys)
            now.removeAll(k);
    } else {
        for (const KeyframeRef& k : keys) {
            if (!now.contains(k))
                now.append(k);
        }
    }
    setSelection(now);
    update();
    return true;
}

bool KeyframeViewBase::selectCursorRow()
{
    const QVector<QVector<ParamId>> rows = cursorRows();
    if (m_cursorRow < 0 || m_cursorRow >= rows.size())
        return false;
    QVector<KeyframeRef> refs;
    for (const int64_t frame : rowFrames(m_cursorRow))
        refs += keysAtFrame(frame, rows[m_cursorRow]);
    if (refs.isEmpty())
        return false;
    setSelection(refs);
    update();
    return true;
}

void KeyframeViewBase::unselectKeys()
{
    m_ctx.selection->setKeyframes({});
    update();
}

bool KeyframeViewBase::nudgeTime(int64_t frames)
{
    const QVector<KeyframeRef> refs = keyTargets();
    if (refs.isEmpty() || frames == 0)
        return false;
    const bool hadSelection = !selectedKeys().isEmpty();
    const int64_t delta = ko::clampMoveDelta(m_ctx.project->timeline()->data(), refs, frames);
    if (delta == 0)
        return false;
    m_ctx.project->beginEdit(tr("Move Keyframes"));
    const QVector<KeyframeRef> moved
        = ko::moveKeys(m_ctx.project->timeline()->mutableData(), refs, delta);
    m_ctx.project->endEdit();
    if (hadSelection)
        m_ctx.selection->setKeyframes(moved);
    m_cursorFrame += delta;
    update();
    return true;
}

bool KeyframeViewBase::addKeyAtPlayhead()
{
    const QVector<QVector<ParamId>> rows = cursorRows();
    if (m_cursorRow < 0 || m_cursorRow >= rows.size())
        return false;
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    bool any = false;
    m_ctx.project->beginEdit(tr("Add Keyframe"));
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    for (const ParamId& id : rows[m_cursorRow]) {
        ClipData* c = editops::clipById(data, id.clipId);
        AnimatedValue* v = c ? ko::findValue(*c, id.path) : nullptr;
        const int64_t local = c ? playhead - c->start : 0;
        if (!v || local < 0 || local >= c->duration || v->hasKeyAt(local))
            continue;
        v->setKey(local, v->valueAt(local));
        any = true;
    }
    m_ctx.project->endEdit();
    if (any) {
        m_cursorFrame = playhead;
        update();
    }
    return any;
}

bool KeyframeViewBase::deleteTargets()
{
    if (selectedKeys().isEmpty())
        setSelection(cursorKeys());
    const int row = m_cursorRow;
    const int64_t frame = m_cursorFrame;
    if (!kc::deleteSelected(m_ctx))
        return false;
    if (row >= 0)
        placeCursor(row, nearest(rowFrames(row), frame));
    return true;
}

bool KeyframeViewBase::copyTargets()
{
    if (selectedKeys().isEmpty())
        setSelection(cursorKeys());
    return kc::copySelected(m_ctx);
}

bool KeyframeViewBase::pasteAtPlayhead()
{
    return kc::paste(m_ctx);
}

bool KeyframeViewBase::interpTargets(Interp interp)
{
    if (selectedKeys().isEmpty())
        setSelection(cursorKeys());
    return kc::setSelectedInterp(m_ctx, interp);
}

bool KeyframeViewBase::jumpToCursor()
{
    if (m_cursorRow < 0)
        return false;
    jumpToFrame(m_cursorFrame);
    return true;
}

} // namespace kk
