// SPDX-License-Identifier: GPL-3.0-or-later
// ParamGroupBox — a collapsible inspector section (header + aligned grid of ParamRows).
#pragma once

#include "ui/inspector/InspectorGlyphs.h"
#include "ui/inspector/ParamAccess.h"

#include <QVector>
#include <QWidget>

#include <functional>

class QAction;
class QCheckBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QToolButton;

namespace kk {

class ParamEditHost;
class ParamRow;

/// One section of the inspector: "Transform", "Cropping", an effect instance, a transition...
///
/// The header shows a collapse chevron, the title and optional extra controls (an enable
/// checkbox and tool buttons for effects). The body is a QGridLayout shared by all rows so the
/// labels, fields and keyframe buttons line up in columns (see paramgrid::).
///
/// Built once per structure; refresh() just forwards to the rows (cheap, no allocation).
class ParamGroupBox : public QWidget
{
    Q_OBJECT
public:
    /// Creates an expanded, empty group titled @p title.
    explicit ParamGroupBox(const QString& title, QWidget* parent = nullptr);

    /// Adds an editor row for @p desc at the end of the grid. Returns the row.
    ParamRow* addParam(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host);
    /// Adds an arbitrary widget spanning the editor columns (with an optional label).
    void addWidgetRow(const QString& label, QWidget* widget);
    /// Rows in display order.
    [[nodiscard]] const QVector<ParamRow*>& rows() const { return m_rows; }

    /// Adds an enable checkbox before the title (effects). Returns it (owned by the header).
    QCheckBox* addEnableToggle();
    /// Adds a compact tool button at the right of the header.
    QToolButton* addHeaderButton(InspectorGlyph glyph, const QString& toolTip);
    /// Adds an arbitrary widget at the right of the header (e.g. the section keyframe diamond).
    void addHeaderWidget(QWidget* widget);
    /// Changes the header title.
    void setTitle(const QString& title);
    [[nodiscard]] QString title() const;
    /// Adds @p action to the header's right-click menu (e.g. "How to use <name>…").
    void addHeaderMenuAction(QAction* action);
    /// The header's right-click menu entries.
    [[nodiscard]] QList<QAction*> headerMenuActions() const;

    /// True if the body is hidden.
    [[nodiscard]] bool isCollapsed() const { return m_collapsed; }
    /// Shows or hides the body.
    void setCollapsed(bool collapsed);

    /// Called by refresh() before the rows, for header state (e.g. the enable checkbox).
    void setHeaderRefresh(std::function<void(const ClipData&)> refresh)
    {
        m_headerRefresh = std::move(refresh);
    }

    /// Refreshes the header and every row from @p clip at @p localFrame.
    void refresh(const ClipData& clip, int64_t localFrame, bool playheadInside);

signals:
    /// The user collapsed or expanded the group.
    void collapsedChanged(bool collapsed);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* m_header = nullptr;
    QHBoxLayout* m_headerLayout = nullptr;
    QToolButton* m_chevron = nullptr;
    QLabel* m_title = nullptr;
    QWidget* m_body = nullptr;
    QGridLayout* m_grid = nullptr;
    QVector<ParamRow*> m_rows;
    std::function<void(const ClipData&)> m_headerRefresh;
    bool m_collapsed = false;
    int m_nextRow = 0;
};

} // namespace kk
