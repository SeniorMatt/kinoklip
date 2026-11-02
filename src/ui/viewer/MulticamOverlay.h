// SPDX-License-Identifier: GPL-3.0-or-later
// MulticamOverlay — the multicam viewer's on-picture layer: angle numbers and names, the
// active video / audio angle frames, page arrows; turns clicks into angle choices.
#pragma once

#include <QStringList>
#include <QWidget>

namespace kk {

class ViewerSurface;

/// Transparent child of a viewer surface drawn over the angle grid (multicam::gridTimeline()
/// renders the pictures; this only adds the chrome and the input).
///
/// - Each cell: thin frame, "n  name" label at the bottom left. The active angle has a
///   coloured frame: red = video and audio, blue = video only, green = audio only (two
///   frames when the video and audio angles differ).
/// - Click on a cell: angleClicked(angle). Hover: a light highlight.
/// - More angles than cells: page arrows at the top right (pageRequested(+-1)) and "page n / m".
///
/// Thread-safety: GUI thread only.
class MulticamOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit MulticamOverlay(ViewerSurface* surface);

    /// What the grid shows: @p names of every angle, @p columns per row, page @p page.
    void setGrid(const QStringList& names, int columns, int page);
    /// Active angles (0-based, -1 = none) highlighted.
    void setActive(int videoAngle, int audioAngle);
    /// Message shown in the middle instead of the grid chrome (empty = none).
    void setMessage(const QString& text);
    /// Follows the surface's picture rectangle (zoom, resize).
    void refresh();

    [[nodiscard]] int columns() const { return m_columns; }
    [[nodiscard]] int page() const { return m_page; }
    [[nodiscard]] int pageCount() const;
    /// Angle under widget position @p pos, or -1.
    [[nodiscard]] int angleAt(const QPoint& pos) const;
    /// Widget rectangle of angle @p angle's cell on the current page (empty if not on it).
    [[nodiscard]] QRect cellRect(int angle) const;

Q_SIGNALS:
    /// The user clicked the cell of @p angle (0-based).
    void angleClicked(int angle);
    /// The user clicked a page arrow (@p delta = -1 / +1).
    void pageRequested(int delta);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] QRect pageArrowRect(int delta) const;

    ViewerSurface* m_surface;
    QStringList m_names;
    int m_columns = 2;
    int m_page = 0;
    int m_video = -1;
    int m_audio = -1;
    int m_hover = -1;
    QString m_message;
};

} // namespace kk
