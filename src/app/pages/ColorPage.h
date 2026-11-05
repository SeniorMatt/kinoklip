// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPage — the Color page in Resolve's layout (docs/color-page.md, "Full page"): gallery,
// viewer and node editor on top, the clips strip and a mini timeline in the middle, the
// palettes (Primaries, Curves, Qualifier, Windows, LUTs) and the scopes / key below. Thin:
// every command goes through ColorSession (src/ui/color), the maths is in core and render.
#pragma once

#include "ui/common/Page.h"

#include <QList>
#include <QPointer>
#include <QStringList>

class QAction;
class QButtonGroup;
class QLabel;
class QSplitter;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace kk {

class ActionRegistry;
struct AppContext;
class ColorClipStrip;
class ColorCurvesPanel;
class ColorFrameSource;
class ColorGallery;
class ColorMiniTimeline;
class ColorNodeEditor;
class ColorSession;
class ColorViewerOverlay;
class KeyPanel;
class LutBrowser;
class PanelFrame;
class PrimariesPanel;
class QualifierPanel;
class ScopeFeeder;
class ScopesPanel;
class ViewerSurface;
class WindowsPanel;

/// Layout:
/// ```
/// | Gallery | viewer (header: clip, Clip / Timeline, Highlight, Wipe, Bypass) | Nodes |
/// |         | the picture with its on-screen controls, transport + timecode   |       |
/// |------------------------------------------------------------------------------------|
/// | clips strip (thumbnails, the graded clip outlined)                                  |
/// | mini timeline                                                                       |
/// |------------------------------------------------------------------------------------|
/// | Primaries | Curves | Qualifier | Windows | LUTs  (palette)  | Scopes | Key         |
/// ```
/// The interface bar shows Gallery, Clips and Timeline on the left, Nodes and Scopes on the
/// right (each panel can be hidden). The page grades the clip under the playhead
/// (ColorSession). The color.* commands act while the page is shown and focus is on it: the
/// page claims their keys (ShortcutOverride; not while typing in a text field), so they
/// never clash with the Edit page's, whose timeline commands are suspended here
/// (TimelineCommandSuspension).
///
/// Thread-safety: GUI thread only.
class ColorPage : public Page
{
    Q_OBJECT
public:
    /// The bottom-left palettes, in tab order.
    enum class Palette {
        Primaries,
        Curves,
        Qualifier,
        Windows,
        Luts
    };

    explicit ColorPage(AppContext& ctx, QWidget* parent = nullptr);
    ~ColorPage() override;

    [[nodiscard]] QString pageId() const override { return QStringLiteral("color"); }
    [[nodiscard]] bool hasPanel(PanelId id) const override;
    void setPanelVisible(PanelId id, bool visible) override;
    [[nodiscard]] bool isPanelVisible(PanelId id) const override;
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;
    void resetLayout() override;

    [[nodiscard]] ColorSession* session() const { return m_session; }
    [[nodiscard]] ColorFrameSource* frames() const { return m_frames; }
    [[nodiscard]] ColorNodeEditor* nodeEditor() const { return m_nodes; }
    [[nodiscard]] PrimariesPanel* primaries() const { return m_primaries; }
    [[nodiscard]] ColorCurvesPanel* curves() const { return m_curves; }
    [[nodiscard]] QualifierPanel* qualifier() const { return m_qualifier; }
    [[nodiscard]] WindowsPanel* windows() const { return m_windows; }
    [[nodiscard]] LutBrowser* luts() const { return m_luts; }
    [[nodiscard]] KeyPanel* keyPanel() const { return m_key; }
    [[nodiscard]] ScopesPanel* scopes() const { return m_scopes; }
    [[nodiscard]] ScopeFeeder* scopeFeeder() const { return m_feeder; }
    [[nodiscard]] ColorGallery* gallery() const { return m_gallery; }
    [[nodiscard]] ColorClipStrip* clipStrip() const { return m_strip; }
    [[nodiscard]] ColorMiniTimeline* miniTimeline() const { return m_mini; }
    [[nodiscard]] ViewerSurface* viewer() const { return m_surface; }
    [[nodiscard]] ColorViewerOverlay* overlay() const { return m_overlay; }

    /// Shows palette @p palette (the tab buttons do the same).
    void showPalette(Palette palette);
    [[nodiscard]] Palette currentPalette() const;
    /// Highlight on (the viewer shows the current node's key).
    [[nodiscard]] bool isHighlighting() const { return m_highlightOn; }
    void setHighlight(bool on);
    /// Wipe on (the viewer splits with the gallery's wipe still).
    void setWipe(bool on);
    /// Picks qualifier colours inside @p frameRect (fractions of the program frame) with the
    /// qualifier palette's eyedropper mode (Pick when none is active); the overlay's
    /// pickRequested() lands here.
    void pickQualifier(const QRectF& frameRect);
    /// Re-renders the node thumbnails and the highlight now (normally debounced).
    void refreshPictures();

    /// Ids of the color.* commands (the Color menu).
    [[nodiscard]] static QStringList commandIds(const ActionRegistry& registry);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Resolve-like splitter proportions for the page's size (first show, Reset UI Layout).
    void applyDefaultSizes();
    QWidget* buildViewer();
    QWidget* buildPalettes();
    void connectActions();
    void updateOutputClient();
    void updateHeader();
    void updateTimecode();
    void updateWipe();
    /// The color.* action bound to the key of @p event, or nullptr.
    [[nodiscard]] QAction* actionForKey(const QKeyEvent* event) const;
    [[nodiscard]] QWidget* panelWidget(PanelId id) const;

    AppContext& m_ctx;
    ColorSession* m_session = nullptr;
    ColorFrameSource* m_frames = nullptr;
    ViewerSurface* m_surface = nullptr;
    ColorViewerOverlay* m_overlay = nullptr;
    PanelFrame* m_viewerFrame = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_timecode = nullptr;
    QToolButton* m_clipButton = nullptr;
    QToolButton* m_targetButton = nullptr; ///< "Timeline": checked = the timeline grade
    QToolButton* m_bypassButton = nullptr;
    QToolButton* m_highlightButton = nullptr;
    QToolButton* m_wipeButton = nullptr;
    ScopesPanel* m_scopes = nullptr;
    ScopeFeeder* m_feeder = nullptr;
    ColorNodeEditor* m_nodes = nullptr;
    PrimariesPanel* m_primaries = nullptr;
    ColorCurvesPanel* m_curves = nullptr;
    QualifierPanel* m_qualifier = nullptr;
    WindowsPanel* m_windows = nullptr;
    LutBrowser* m_luts = nullptr;
    KeyPanel* m_key = nullptr;
    ColorGallery* m_gallery = nullptr;
    ColorClipStrip* m_strip = nullptr;
    ColorMiniTimeline* m_mini = nullptr;
    PanelFrame* m_galleryFrame = nullptr;
    PanelFrame* m_nodesFrame = nullptr;
    PanelFrame* m_scopesFrame = nullptr;
    QWidget* m_middle = nullptr;
    QStackedWidget* m_paletteStack = nullptr;
    QButtonGroup* m_paletteButtons = nullptr;
    QStackedWidget* m_rightStack = nullptr;
    QButtonGroup* m_rightButtons = nullptr;
    QSplitter* m_rows = nullptr;
    QSplitter* m_top = nullptr;
    QSplitter* m_bottom = nullptr;
    QTimer* m_pictureTimer = nullptr;
    bool m_highlightOn = false;
    bool m_wipeOn = false;
    bool m_filterInstalled = false;
    bool m_defaultSizes = true;
};

} // namespace kk
