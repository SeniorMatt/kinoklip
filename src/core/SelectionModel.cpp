// SPDX-License-Identifier: GPL-3.0-or-later
// SelectionModel implementation.
#include "core/SelectionModel.h"

namespace kk {

SelectionModel::SelectionModel(QObject* parent)
    : QObject(parent)
{ }

void SelectionModel::setClips(const QVector<quint64>& clipIds)
{
    QVector<quint64> unique;
    for (quint64 id : clipIds) {
        if (id != 0 && !unique.contains(id))
            unique.append(id);
    }
    if (unique == m_clips && m_keyframes.isEmpty() && !m_transition)
        return;
    m_clips = unique;
    m_keyframes.clear();
    m_transition.reset();
    emit changed();
}

void SelectionModel::addClips(const QVector<quint64>& clipIds)
{
    bool modified = false;
    if (m_transition && !clipIds.isEmpty()) {
        m_transition.reset();
        modified = true;
    }
    for (quint64 id : clipIds) {
        if (id != 0 && !m_clips.contains(id)) {
            m_clips.append(id);
            modified = true;
        }
    }
    if (modified)
        emit changed();
}

void SelectionModel::removeClips(const QVector<quint64>& clipIds)
{
    const qsizetype removed = m_clips.removeIf([&](quint64 id) { return clipIds.contains(id); });
    if (removed > 0)
        emit changed();
}

void SelectionModel::toggleClip(quint64 clipId)
{
    if (isSelected(clipId))
        removeClips({clipId});
    else
        addClips({clipId});
}

void SelectionModel::prune(const QSet<quint64>& existingIds)
{
    const qsizetype removed
        = m_clips.removeIf([&](quint64 id) { return !existingIds.contains(id); });
    const qsizetype removedKeys = m_keyframes.removeIf(
        [&](const KeyframeRef& k) { return !existingIds.contains(k.clipId); });
    const bool transitionGone = m_transition && !existingIds.contains(m_transition->clipId);
    if (transitionGone)
        m_transition.reset();
    if (removed > 0 || removedKeys > 0 || transitionGone)
        emit changed();
}

void SelectionModel::setKeyframes(const QVector<KeyframeRef>& keys)
{
    if (keys == m_keyframes)
        return;
    m_keyframes = keys;
    emit changed();
}

void SelectionModel::selectTransition(const TransitionRef& ref)
{
    if (ref.clipId == 0) {
        clear();
        return;
    }
    if (m_transition == ref && m_clips.isEmpty() && m_keyframes.isEmpty())
        return;
    m_clips.clear();
    m_keyframes.clear();
    m_transition = ref;
    emit changed();
}

void SelectionModel::clear()
{
    if (isEmpty())
        return;
    m_clips.clear();
    m_keyframes.clear();
    m_transition.reset();
    emit changed();
}

} // namespace kk
