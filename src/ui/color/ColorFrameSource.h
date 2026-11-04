// SPDX-License-Identifier: GPL-3.0-or-later
// ColorFrameSource — the pictures the Color page computes beside the viewer: the ungraded
// frame at the playhead (qualifier picker, highlight, node thumbnails) and the graded
// thumbnails of the clips strip. Renders through PlaybackEngine::renderImage(), one request in
// flight; the grading of those pictures runs on the CPU (render::applyGrade()).
#pragma once

#include "core/Grade.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <cstdint>
#include <functional>

class QTimer;
template<typename T>
class QFutureWatcher;

namespace kk {

class ColorSession;
class PlaybackEngine;
class Project;
struct RenderedFrame;

/// Pictures for the Color page.
///
/// **Input picture.** input() is the program frame at the playhead rendered WITHOUT grades,
/// kInputWidth wide: what the clip grade starts from when the clip covers the frame (the
/// clip-content mapping of transformed clips is approximated by the full frame; see
/// docs/color-page.md, "Full page"). It is refreshed a moment after the playhead stops or the
/// timeline changes, never while playing.
///
/// **Thumbnails.** thumbnail() is a clip's graded picture at its middle frame (the program
/// composite there), kThumbSize; refreshed when the clip, its grade or the timeline grade
/// change.
///
/// Thread-safety: GUI thread only (the renders complete on the compositor's threads).
class ColorFrameSource : public QObject
{
    Q_OBJECT
public:
    static constexpr int kInputWidth = 960;
    static constexpr QSize kThumbSize {112, 63};

    /// How highlight() shows the key.
    enum class HighlightMode {
        Grey, ///< the graded picture where the key is, flat grey elsewhere
        BlackWhite ///< the key as a white-on-black matte
    };

    ColorFrameSource(
        PlaybackEngine* engine, Project* project, ColorSession* session, QObject* parent = nullptr);
    ~ColorFrameSource() override;

    /// Renders only while active (the page is shown).
    void setActive(bool active);
    [[nodiscard]] bool isActive() const { return m_active; }

    /// The ungraded playhead frame (null before the first render).
    [[nodiscard]] const QImage& input() const { return m_input; }
    /// Sets the input picture directly (tests, and engines without a compositor).
    void setInput(const QImage& image);
    /// Re-renders the input picture soon (debounced).
    void refreshInput();

    /// The picture node @p nodeId receives, from input() (@p size: scaled first; empty = the
    /// input's size). Unknown node: input().
    [[nodiscard]] QImage nodeInput(quint64 nodeId, QSize size = {}) const;
    /// The output of node @p nodeId (the grade cut at that node), scaled to @p size.
    [[nodiscard]] QImage nodeOutput(quint64 nodeId, QSize size) const;
    /// The highlight picture of node @p nodeId: its key (qualifier x windows x mix) over the
    /// input, in @p mode. Null without an input picture or node.
    [[nodiscard]] QImage highlight(quint64 nodeId, HighlightMode mode) const;
    /// The straight colour of node @p nodeId's input at picture fraction @p uv (0..1), the
    /// qualifier picker's sample (a 3 x 3 average). False without an input picture.
    bool sampleNodeInput(quint64 nodeId, QPointF uv, float rgb[3]) const;

    /// The graded thumbnail of @p clipId (null until rendered).
    [[nodiscard]] QImage thumbnail(quint64 clipId) const { return m_thumbs.value(clipId).image; }
    /// Re-renders the thumbnails whose clip or grade changed (debounced).
    void refreshThumbnails();

Q_SIGNALS:
    void inputChanged();
    void thumbnailChanged(quint64 clipId);

private:
    struct Thumb
    {
        QImage image;
        QByteArray signature;
    };
    struct Job
    {
        int64_t frame = 0;
        QSize size;
        bool graded = true;
        std::function<void(const QImage&)> done;
    };

    /// A copy of the current grade whose output is @p nodeId's input (or the node itself).
    [[nodiscard]] ClipGrade cutAt(quint64 nodeId, bool includeNode) const;
    [[nodiscard]] QByteArray signature(quint64 clipId) const;
    void renderInputNow();
    void renderThumbnailsNow();
    void enqueue(Job job);
    void runNext();
    void onRendered();

    QPointer<PlaybackEngine> m_engine;
    Project* m_project = nullptr;
    QPointer<ColorSession> m_session;
    bool m_active = false;
    QImage m_input;
    QHash<quint64, Thumb> m_thumbs;
    QTimer* m_inputTimer = nullptr;
    QTimer* m_thumbTimer = nullptr;
    QVector<Job> m_queue;
    Job m_running;
    bool m_busy = false;
    QFutureWatcher<std::shared_ptr<const RenderedFrame>>* m_watcher = nullptr;
};

} // namespace kk
