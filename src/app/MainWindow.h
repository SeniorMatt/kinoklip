// SPDX-License-Identifier: GPL-3.0-or-later
// MainWindow — menu bar, interface toolbar, page stack and page bar.
#pragma once

#include "ui/common/Page.h"

#include <QMainWindow>
#include <QVector>

class QAction;
class QActionGroup;
class QStackedWidget;

namespace kk {

struct AppContext;
class AutosaveController;
class DeliverPage;
class AudioPage;
class ColorPage;
class CompositingPage;
class ScopesWindow;
class CutPage;
class EditPage;
class HelpController;
class InterfaceBar;
class MediaPage;
class PageBar;
class ProjectController;
class ViewerTransport;

/// Options for special runs (screenshots, tests).
struct MainWindowOptions
{
    bool restoreLayout = true; ///< restore geometry/splitters/panels from QSettings
    bool saveLayout = true; ///< save them again on close
    bool autosave = true; ///< run the AutosaveController
};

/// The application window.
///
/// Layout: menu bar (built from the ActionRegistry) / InterfaceBar (panel toggles + project
/// name) / QStackedWidget of Pages / PageBar. Pages are registered with addPage(), which is the
/// extension point for new pages (see src/app/readme.md).
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    /// Builds the window, the standard pages and menus; connects the app-level actions.
    explicit MainWindow(
        AppContext& ctx, const MainWindowOptions& options = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Adds @p page to the stack and binds it to the checkable page action @p actionId
    /// (its button appears in the page bar). Takes ownership.
    void addPage(Page* page, const QString& actionId);
    /// Switches to the page with Page::pageId() == @p pageId. Returns false if unknown.
    bool setCurrentPage(const QString& pageId);
    /// Id of the visible page.
    [[nodiscard]] QString currentPageId() const;
    /// Page with id @p pageId, or nullptr.
    [[nodiscard]] Page* page(const QString& pageId) const;

    [[nodiscard]] MediaPage* mediaPage() const { return m_mediaPage; }
    [[nodiscard]] CutPage* cutPage() const { return m_cutPage; }
    [[nodiscard]] AudioPage* audioPage() const { return m_audioPage; }
    [[nodiscard]] EditPage* editPage() const { return m_editPage; }
    [[nodiscard]] CompositingPage* compositingPage() const { return m_compositingPage; }
    [[nodiscard]] ColorPage* colorPage() const { return m_colorPage; }
    /// The Video Scopes window (View > Video Scopes), created on first use; may be null.
    [[nodiscard]] ScopesWindow* scopesWindow() const { return m_scopesWindow; }
    [[nodiscard]] DeliverPage* deliverPage() const { return m_deliverPage; }
    [[nodiscard]] ProjectController* projectController() const { return m_projectController; }
    /// The in-app help (F1, Help > Guide).
    [[nodiscard]] HelpController* helpController() const { return m_help; }
    /// The application objects this window was built with.
    [[nodiscard]] AppContext& context() const { return m_ctx; }

    /// Shows the window: restored geometry if available, otherwise maximised.
    void showRestored();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct PageEntry
    {
        Page* page;
        QAction* action;
    };

    void buildMenus();
    /// Drops the Alt+letter mnemonic of a menu bar title whose Alt+letter is a keyboard
    /// shortcut (Alt+V Paste Attributes, Alt+F Find in Media Pool, Alt+M Delete Marker), so the
    /// shortcut wins instead of both being ignored as ambiguous (spec 23.9).
    void updateMenuMnemonics();
    void buildInterfaceBar();
    void connectAppActions();
    void connectPanelToggles();
    void syncPanelToggles();
    void updateTitle();
    void saveSettings() const;
    void restoreSettings();

    AppContext& m_ctx;
    MainWindowOptions m_options;
    InterfaceBar* m_interfaceBar = nullptr;
    QStackedWidget* m_stack = nullptr;
    PageBar* m_pageBar = nullptr;
    QActionGroup* m_pageActions = nullptr;
    QVector<PageEntry> m_pages;
    MediaPage* m_mediaPage = nullptr;
    CutPage* m_cutPage = nullptr;
    AudioPage* m_audioPage = nullptr;
    EditPage* m_editPage = nullptr;
    CompositingPage* m_compositingPage = nullptr;
    ColorPage* m_colorPage = nullptr;
    ScopesWindow* m_scopesWindow = nullptr;
    DeliverPage* m_deliverPage = nullptr;
    ProjectController* m_projectController = nullptr;
    AutosaveController* m_autosave = nullptr;
    HelpController* m_help = nullptr;
    ViewerTransport* m_transport = nullptr; ///< routes transport actions to the active viewer
    bool m_geometryRestored = false;
};

} // namespace kk
