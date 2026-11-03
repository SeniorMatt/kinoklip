// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPage — the minimal Color page (docs/color-page.md, "Page"): a viewer and the scopes on
// top, the node list, the Primaries wheels and the curves below, a small gallery strip. Thin:
// every command goes through ColorSession (src/ui/color), the maths is in core and render.
// The full Resolve-style page (node editor, palettes, thumbnail timeline) replaces it later.
#pragma once

#include "ui/common/Page.h"

#include <QList>
#include <QPointer>
#include <QStringList>

class QAction;
class QListWidget;
class QSplitter;
class QToolButton;

namespace kk {

class ActionRegistry;
struct AppContext;
class ColorSession;
class GradeCurveEditor;
class NodeList;
class PrimariesPanel;
class ScopeFeeder;
class ScopesPanel;
class ViewerSurface;

/// Layout:
/// ```
/// | viewer (the program picture, graded)            | scopes         |
/// |-------------------------------------------------------------------|
/// | nodes | Primaries (wheels, fields)  | curves    | gallery        |
/// ```
/// The page grades the clip under the playhead (ColorSession). The color.* commands
/// (Alt+S / Alt+P / Alt+L, Ctrl+D, Shift+D, Ctrl+Alt+G, ...) act while the page is shown and
/// focus is on it: the page claims their keys (ShortcutOverride) like the node editor does,
/// so they never clash with the Edit page's. The Edit page's timeline commands are suspended
/// while the page is shown (TimelineCommandSuspension).
///
/// Thread-safety: GUI thread only.
class ColorPage : public Page
{
    Q_OBJECT
public:
    explicit ColorPage(AppContext& ctx, QWidget* parent = nullptr);
    ~ColorPage() override;

    [[nodiscard]] QString pageId() const override { return QStringLiteral("color"); }
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;

    [[nodiscard]] ColorSession* session() const { return m_session; }
    [[nodiscard]] NodeList* nodeList() const { return m_nodes; }
    [[nodiscard]] PrimariesPanel* primaries() const { return m_primaries; }
    [[nodiscard]] GradeCurveEditor* curves() const { return m_curves; }
    [[nodiscard]] ScopesPanel* scopes() const { return m_scopes; }
    [[nodiscard]] ScopeFeeder* scopeFeeder() const { return m_feeder; }
    [[nodiscard]] QListWidget* gallery() const { return m_gallery; }
    [[nodiscard]] ViewerSurface* viewer() const { return m_surface; }

    /// Ids of the color.* commands (the Color menu).
    [[nodiscard]] static QStringList commandIds(const ActionRegistry& registry);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void connectActions();
    void refreshGallery();
    void updateOutputClient();
    void updateHeader();
    /// The color.* action bound to the key of @p event, or nullptr.
    [[nodiscard]] QAction* actionForKey(const QKeyEvent* event) const;

    AppContext& m_ctx;
    ColorSession* m_session = nullptr;
    ViewerSurface* m_surface = nullptr;
    QWidget* m_header = nullptr;
    QToolButton* m_targetButton = nullptr;
    QToolButton* m_bypassButton = nullptr;
    ScopesPanel* m_scopes = nullptr;
    ScopeFeeder* m_feeder = nullptr;
    NodeList* m_nodes = nullptr;
    PrimariesPanel* m_primaries = nullptr;
    GradeCurveEditor* m_curves = nullptr;
    QListWidget* m_gallery = nullptr;
    QSplitter* m_rows = nullptr;
    QSplitter* m_top = nullptr;
    QSplitter* m_bottom = nullptr;
    bool m_filterInstalled = false;
};

} // namespace kk
