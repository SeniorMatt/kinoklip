// SPDX-License-Identifier: GPL-3.0-or-later
// ColorBrowsers — the Color page's gallery of stills, the clips strip (thumbnails of the
// timeline's clips) and the mini timeline under it.
#pragma once

#include <QListWidget>
#include <QPointer>
#include <QWidget>

class QLabel;
class QToolButton;

namespace kk {

class ColorFrameSource;
class ColorSession;
class Project;

/// The Gallery panel: the project's stills as thumbnails. Double-click or middle-click a still
/// applies its grade to the current clip; its menu applies it to the clips selected in the
/// strip, wipes the viewer with it, renames or deletes it. (Grab Still is the page's
/// color.grab_still command, in the panel's title bar.)
class ColorGallery : public QWidget
{
    Q_OBJECT
public:
    ColorGallery(ColorSession* session, Project* project, QWidget* parent = nullptr);

    [[nodiscard]] QListWidget* list() const { return m_list; }
    /// The selected still (0: none).
    [[nodiscard]] quint64 currentStill() const;
    void setCurrentStill(quint64 id);
    /// The still the viewer wipes against (0: none).
    [[nodiscard]] quint64 wipeStill() const { return m_wipeStill; }
    void setWipeStill(quint64 id);
    /// The menu for still @p id (tests call it directly).
    [[nodiscard]] QMenu* buildMenu(quint64 id, QWidget* parent);

Q_SIGNALS:
    void wipeStillChanged(quint64 id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();

    QPointer<ColorSession> m_session;
    Project* m_project = nullptr;
    QListWidget* m_list = nullptr;
    quint64 m_wipeStill = 0;
};

/// The clips strip: every clip of the timeline in order (ColorSession::stripClips()) as a
/// numbered thumbnail with its track and timecode; the clip being graded is outlined, graded
/// clips carry a mark. Click grades a clip (the playhead moves into it); Ctrl / Shift click
/// select several (paste targets); middle-click copies that clip's grade to the selected clips
/// (or the current one), as Resolve users do.
class ColorClipStrip : public QListWidget
{
    Q_OBJECT
public:
    ColorClipStrip(ColorSession* session, Project* project, ColorFrameSource* frames,
        QWidget* parent = nullptr);

    /// Row of clip @p clipId (-1: none).
    [[nodiscard]] int rowOf(quint64 clipId) const;
    /// Clip of row @p row.
    [[nodiscard]] quint64 clipAt(int row) const;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void rebuild();
    void updateCurrent();
    void syncSelection();

    QPointer<ColorSession> m_session;
    Project* m_project = nullptr;
    QPointer<ColorFrameSource> m_frames;
    QVector<quint64> m_clips;
    bool m_updating = false;
};

/// The mini timeline under the clips strip: the video tracks with their clips (the graded one
/// highlighted) and the playhead, fitted to the width. Click or drag to move the playhead;
/// the page then grades the clip there.
class ColorMiniTimeline : public QWidget
{
    Q_OBJECT
public:
    ColorMiniTimeline(ColorSession* session, Project* project, QWidget* parent = nullptr);

    /// Timeline frame at widget x @p x.
    [[nodiscard]] int64_t frameAt(double x) const;
    [[nodiscard]] double xOf(int64_t frame) const;

    [[nodiscard]] QSize sizeHint() const override { return {600, 44}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {100, 26}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] int64_t length() const;

    QPointer<ColorSession> m_session;
    Project* m_project = nullptr;
    bool m_dragging = false;
};

} // namespace kk
