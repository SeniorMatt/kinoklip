// SPDX-License-Identifier: GPL-3.0-or-later
// ColorBrowsers — the Color page's gallery of stills, the clips strip (thumbnails of the
// timeline's clips) and the mini timeline under it.
#pragma once

#include <QListWidget>
#include <QTreeWidget>
#include <QPointer>
#include <QWidget>

class QLabel;
class QToolButton;

namespace kk {

class ColorFrameSource;
class GradeGallery;
struct GalleryStill;
class ColorSession;
class Project;

/// The Gallery panel (Resolve's): the album list on the left (Stills with its albums, PowerGrade
/// with its albums, Memories) and the stills of the selected album as thumbnails, or the memories
/// A to Z as tiles. Double-click a still to wipe the viewer with it (again: wipe off),
/// middle-click applies its grade to the current clip; the still's menu applies it (or appends
/// its node graph) to the clips selected in the strip, shows its node graph, wipes the viewer
/// with it, imports and exports stills, moves or copies them between albums (also by dragging
/// them onto an album), renames or deletes it. The PowerGrade albums live in the user data
/// folder and are the same in every project; the other albums and the memories are the
/// project's (GradeGallery). (Grab Still is the page's color.grab_still command, in the panel's
/// title bar.)
class ColorGallery : public QWidget
{
    Q_OBJECT
public:
    /// Which list the thumbnails show.
    enum class Source { Project, PowerGrade, Memories };
    /// PowerGrade stills carry this bit in the ids this widget hands out (currentStill(),
    /// wipeStill(), buildMenu()); stillFor() resolves either kind.
    static constexpr quint64 kPowerGradeBit = quint64(1) << 62;

    ColorGallery(ColorSession* session, Project* project, QWidget* parent = nullptr);

    [[nodiscard]] QListWidget* list() const { return m_list; }
    [[nodiscard]] QTreeWidget* albums() const { return m_albums; }
    /// The selected still (0: none).
    [[nodiscard]] quint64 currentStill() const;
    void setCurrentStill(quint64 id);
    /// The still the viewer wipes against (0: none).
    [[nodiscard]] quint64 wipeStill() const { return m_wipeStill; }
    void setWipeStill(quint64 id);
    /// The still behind a widget id (project or PowerGrade), or nullptr.
    [[nodiscard]] const GalleryStill* stillFor(quint64 id) const;
    /// The menu for still @p id (tests call it directly).
    [[nodiscard]] QMenu* buildMenu(quint64 id, QWidget* parent);

    // ---- Albums ----
    /// The list the thumbnails show and its album (Memories: album 0).
    [[nodiscard]] Source source() const { return m_source; }
    [[nodiscard]] quint64 album() const { return m_albumId; }
    void selectAlbum(Source source, quint64 album = 1);
    /// Adds an album called @p name to @p source (Project or PowerGrade) and shows it.
    quint64 newAlbum(Source source, const QString& name);
    /// The menu of the album list entry (Memories has none): new, rename, delete, import.
    [[nodiscard]] QMenu* buildAlbumMenu(Source source, quint64 album, QWidget* parent);
    /// Moves still @p id to @p album of @p source: within its own gallery it moves, to the other
    /// one it copies (dragging a still onto a PowerGrade album). Returns the id of the still in
    /// the target (0: nothing happened).
    quint64 dropStill(quint64 id, Source source, quint64 album);
    /// Copies still @p id into @p album of @p source (either gallery); returns the new id.
    quint64 copyStillTo(quint64 id, Source source, quint64 album);
    /// Moves still @p id to another album of its own gallery.
    bool moveStillTo(quint64 id, quint64 album);

    // ---- Stills ----
    /// Grab Still into the album being shown (the first one of the project's when Memories are
    /// shown); selects the new still and returns its widget id (0 when nothing is open).
    quint64 grabStill();
    /// Apply Grade / Append Node Graph of still @p id to @p clips (empty: the current clip).
    int applyStill(quint64 id, const QVector<quint64>& clips = {}, bool append = false);
    /// Import Still: reads @p path (picture and grade file, or grade file) into the album
    /// being shown; returns the new widget id (0: failed, @p error says why).
    quint64 importStill(const QString& path, QString* error = nullptr);
    /// Export Still: writes still @p id to @p path (picture by suffix, plus the grade file).
    bool exportStill(quint64 id, const QString& path, QString* error = nullptr);
    /// Import Still with a file dialog, into the album being shown.
    void importDialog();
    /// Export Still with a save dialog.
    void exportDialog(quint64 id);
    /// Deletes still @p id.
    bool deleteStill(quint64 id);
    /// Shows the node graph of still @p id in a small window (tests may set @p modal false).
    void displayNodeGraph(quint64 id);

    // ---- Memories ----
    /// The gallery that holds @p id's still (project or PowerGrade).
    [[nodiscard]] GradeGallery* galleryOf(quint64 id) const;
    /// Menu of memory tile @p index: save, apply, clear.
    [[nodiscard]] QMenu* buildMemoryMenu(int index, QWidget* parent);

Q_SIGNALS:
    void wipeStillChanged(quint64 id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    void refreshAlbums();
    void importInto(const QStringList& paths);
    [[nodiscard]] QVector<quint64> targetsOf(quint64 id) const;
    [[nodiscard]] static quint64 plainId(quint64 id) { return id & ~kPowerGradeBit; }
    [[nodiscard]] GradeGallery* gallery(Source source) const;

    QPointer<ColorSession> m_session;
    Project* m_project = nullptr;
    QListWidget* m_list = nullptr;
    QTreeWidget* m_albums = nullptr;
    Source m_source = Source::Project;
    quint64 m_albumId = 1;
    quint64 m_wipeStill = 0;
    bool m_refreshing = false;
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
    /// Shot Match: gives every clip of @p sources (the target itself is skipped) the primary
    /// balance and contrast of clip @p target, measured on the graded thumbnails
    /// (colormatch::shotMatch(), strength from ColorSession::shotMatchStrength()). The result
    /// is a Shot Match node at the end of each clip's grade (colormatch::applyShotMatch()); all
    /// clips are one undo step. Returns the number of clips changed; 0 while a thumbnail is
    /// missing.
    int shotMatch(quint64 target, const QVector<quint64>& sources);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void rebuild();
    void updateCurrent();
    void syncSelection();
    /// Generate 3D LUT (CUBE) of clip @p clipId with @p size points: asks for the file.
    void generateLut(quint64 clipId, int size);

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
