// SPDX-License-Identifier: GPL-3.0-or-later
// PanelFrame — the common chrome of every panel: a slim title bar (title + tool area) and a body.
#pragma once

#include <QFrame>
#include <QString>

class QAction;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QToolButton;

namespace kk {

/// Base widget for all panels so they look the same: a title bar with the panel name on the
/// left and an optional row of small tool buttons/widgets on the right, above a content widget.
///
/// Styling hooks for the theme's style sheet: object names "PanelFrame", "PanelTitleBar",
/// "PanelTitle".
class PanelFrame : public QFrame
{
    Q_OBJECT
public:
    /// Creates a panel titled @p title.
    explicit PanelFrame(const QString& title, QWidget* parent = nullptr);

    /// Panel title shown in the title bar.
    [[nodiscard]] QString title() const;
    /// Changes the panel title.
    void setTitle(const QString& title);

    /// Stable key of the panel for the in-app help ("inspector", "media_pool", ...; the
    /// `panel:<key>` contexts of docs/guide/index.json, see src/ui/help). Empty = none.
    void setPanelKey(const QString& key) { m_panelKey = key; }
    [[nodiscard]] QString panelKey() const { return m_panelKey; }

    /// Sets the body widget (re-parented to the panel; the previous one is deleted).
    void setContentWidget(QWidget* widget);
    /// The body widget, or nullptr.
    [[nodiscard]] QWidget* contentWidget() const { return m_content; }

    /// Adds a compact tool button for @p action at the right of the title bar.
    QToolButton* addTitleBarAction(QAction* action);
    /// Adds an arbitrary widget (e.g. a timecode label) at the right of the title bar.
    void addTitleBarWidget(QWidget* widget);
    /// Adds a widget at the left of the title bar, before the title (e.g. a viewer's zoom).
    void addTitleBarLeadingWidget(QWidget* widget);
    /// Centres the title in the bar (viewer headers show the clip / timeline name centred,
    /// with zoom on the left and the timecode on the right); default: left aligned.
    void setTitleCentered(bool centered);
    /// Layout of the title bar's tool area, for custom arrangements.
    [[nodiscard]] QHBoxLayout* titleBarToolsLayout() const { return m_toolsLayout; }
    /// Shows or hides the title bar (e.g. for the viewer in full-screen mode).
    void setTitleBarVisible(bool visible);
    /// The title bar widget, e.g. to install an event filter for header gestures
    /// (the Keyframes panel floats / docks on a header double-click or drag).
    [[nodiscard]] QWidget* titleBar() const { return m_titleBar; }

private:
    QWidget* m_titleBar = nullptr;
    QLabel* m_titleLabel = nullptr;
    QHBoxLayout* m_toolsLayout = nullptr;
    QHBoxLayout* m_leadingLayout = nullptr;
    QGridLayout* m_titleGrid = nullptr;
    QWidget* m_content = nullptr;
    QString m_panelKey;
};

/// A centred, dimmed explanatory label used for empty states and not-yet-implemented panels.
[[nodiscard]] QWidget* createPlaceholderContent(const QString& text, QWidget* parent = nullptr);

/// A PanelFrame whose body is a placeholder text (for panels that will be implemented later).
[[nodiscard]] PanelFrame* createPlaceholderPanel(
    const QString& title, const QString& text, QWidget* parent = nullptr);

} // namespace kk
