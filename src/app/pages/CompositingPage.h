// SPDX-License-Identifier: GPL-3.0-or-later
// CompositingPage — the node compositor: two viewers, time ruler and transport, the node
// editor with its tool bar, the Inspector and the Nodes library (docs/compositing-page.md).
#pragma once

#include "ui/common/Page.h"

#include <QStringList>

class QSplitter;
class QToolButton;

namespace kk {

class ActionRegistry;
struct AppContext;
class CompInspector;
class CompSession;
class CompTransport;
class CompViewer;
class NodeEditor;
class NodeLibraryPanel;
class SelectNodePopup;

/// Compositing page layout (a node compositor's page, own look):
/// ```
/// | Nodes   | Left viewer          | Right viewer         | Inspector |
/// | library |----------------------------------------------|           |
/// | (toggle)| time ruler + transport over the clip's frames |           |
/// |         | node tool bar                                 |           |
/// |         | node editor                                   |           |
/// ```
/// Interface bar toggles: Effects (the Nodes library), Nodes (the node editor) and Inspector.
/// Splitter sizes and panel visibility are saved like the other pages.
///
/// The page shows the composition of one clip (CompSession): on show, the selected clip on a
/// video track, else the clip under the playhead on the top video track; "Open in Compositing
/// Page" opens a specific clip. Without a clip every panel shows "Select a clip on the Edit
/// page". The comp.* commands act while the page is shown.
///
/// The Edit page's timeline is hidden here, so its editing commands (Edit, Trim, Timeline,
/// Clip and Mark menus: Delete, Copy / Paste, Blade, the number keys of the multicam angles,
/// ...) are disabled while the page is shown and enabled again when it hides, the way the Cut
/// and Audio pages switch their own commands; a key the node editor does not claim then does
/// nothing instead of changing a timeline the user cannot see. Transport and navigation,
/// undo / redo, the page keys, Open in / Reset Composition and the dialogs stay available.
///
/// Thread-safety: GUI thread only.
class CompositingPage : public Page
{
    Q_OBJECT
public:
    explicit CompositingPage(AppContext& ctx, QWidget* parent = nullptr);
    ~CompositingPage() override;

    [[nodiscard]] QString pageId() const override { return QStringLiteral("compositing"); }
    [[nodiscard]] bool hasPanel(PanelId id) const override;
    void setPanelVisible(PanelId id, bool visible) override;
    [[nodiscard]] bool isPanelVisible(PanelId id) const override;
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;

    /// Opens clip @p clipId (0: the clip the page would pick itself, see the class doc).
    void openClip(quint64 clipId = 0);

    [[nodiscard]] CompSession* session() const { return m_session; }
    [[nodiscard]] NodeEditor* nodeEditor() const { return m_editor; }
    [[nodiscard]] CompViewer* viewer(int index) const { return index == 0 ? m_left : m_right; }
    [[nodiscard]] CompInspector* inspector() const { return m_inspector; }
    [[nodiscard]] CompTransport* transport() const { return m_transport; }
    [[nodiscard]] NodeLibraryPanel* library() const { return m_library; }
    [[nodiscard]] SelectNodePopup* selectNodePopup() const { return m_selectNode; }
    /// Tool bar button that adds node type @p type, or nullptr.
    [[nodiscard]] QToolButton* toolButton(const QString& type) const;

    /// Ids of the timeline commands the page disables while it is shown (see the class doc and
    /// TimelineCommandSuspension).
    [[nodiscard]] static QStringList suspendedCommandIds(const ActionRegistry& registry);
    /// Timeline menu commands the page keeps enabled (TimelineCommandSuspension's kept ids).
    [[nodiscard]] static QStringList keptCommandIds();

protected:
    void showEvent(QShowEvent* event) override;

private:
    QWidget* buildToolBar();
    void connectActions();
    /// Opens the "Select Node" popup over the node editor.
    void showSelectNode();

    AppContext& m_ctx;
    CompSession* m_session = nullptr;
    CompViewer* m_left = nullptr;
    CompViewer* m_right = nullptr;
    CompTransport* m_transport = nullptr;
    NodeEditor* m_editor = nullptr;
    QWidget* m_nodeArea = nullptr; ///< tool bar + node editor (the "Nodes" panel)
    CompInspector* m_inspector = nullptr;
    NodeLibraryPanel* m_library = nullptr;
    SelectNodePopup* m_selectNode = nullptr;
    QWidget* m_toolBar = nullptr;
    QSplitter* m_columns = nullptr; ///< library | centre | inspector
    QSplitter* m_centre = nullptr; ///< viewers + transport | node area
    QSplitter* m_viewers = nullptr; ///< left | right viewer
};

} // namespace kk
