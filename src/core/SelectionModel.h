// SPDX-License-Identifier: GPL-3.0-or-later
// SelectionModel — which clips (and keyframes) the user has selected.
#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <cstdint>

namespace kk {

/// A selected keyframe: clip, parameter path and clip-local frame.
struct KeyframeRef
{
    quint64 clipId = 0;
    QString paramPath; ///< e.g. "transform/posX", "audio/volumeDb", "effect/<id>/radius"
    int64_t frame = 0; ///< clip-local frame

    friend bool operator==(const KeyframeRef&, const KeyframeRef&) = default;
};

/// Holds the selected clip ids (ordered by selection time) and selected keyframes.
/// Ids of clips that no longer exist are harmless; call prune() after big edits if needed.
/// Thread-safety: GUI thread only.
class SelectionModel : public QObject
{
    Q_OBJECT
public:
    /// Creates an empty selection.
    explicit SelectionModel(QObject* parent = nullptr);

    /// Selected clip ids in the order they were selected.
    [[nodiscard]] const QVector<quint64>& clips() const { return m_clips; }
    /// True if @p clipId is selected.
    [[nodiscard]] bool isSelected(quint64 clipId) const { return m_clips.contains(clipId); }
    /// True if no clip and no keyframe is selected.
    [[nodiscard]] bool isEmpty() const { return m_clips.isEmpty() && m_keyframes.isEmpty(); }
    /// Most recently selected clip (the one the Inspector shows), or 0.
    [[nodiscard]] quint64 currentClip() const { return m_clips.isEmpty() ? 0 : m_clips.back(); }

    /// Replaces the clip selection (also clears selected keyframes).
    void setClips(const QVector<quint64>& clipIds);
    /// Selects exactly one clip (0 = clear).
    void selectClip(quint64 clipId)
    {
        setClips(clipId ? QVector<quint64> {clipId} : QVector<quint64> {});
    }
    /// Adds clips to the selection (keeps existing ones).
    void addClips(const QVector<quint64>& clipIds);
    /// Removes clips from the selection.
    void removeClips(const QVector<quint64>& clipIds);
    /// Adds @p clipId if unselected, removes it otherwise.
    void toggleClip(quint64 clipId);
    /// Drops every selected clip id not contained in @p existingIds.
    void prune(const QSet<quint64>& existingIds);

    /// Selected keyframes.
    [[nodiscard]] const QVector<KeyframeRef>& keyframes() const { return m_keyframes; }
    /// Replaces the keyframe selection.
    void setKeyframes(const QVector<KeyframeRef>& keys);

    /// Clears clips and keyframes.
    void clear();

signals:
    /// The selection changed.
    void changed();

private:
    QVector<quint64> m_clips;
    QVector<KeyframeRef> m_keyframes;
};

} // namespace kk
