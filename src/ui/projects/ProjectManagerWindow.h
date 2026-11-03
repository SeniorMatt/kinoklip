// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectManagerWindow — the Project Manager: browse, organise and open the library's projects.
#pragma once

#include <QDialog>
#include <QStringList>

#include <functional>

class QAbstractItemView;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QModelIndex;
class QPushButton;
class QSlider;
class QStackedWidget;
class QToolButton;
class QTreeView;

namespace kk {

class ProjectCardDelegate;
class ProjectLibrary;
class ProjectLibraryModel;
class ProjectSortProxy;

/// The Project Manager window (File > Project Manager, Shift+1; shown at start-up).
///
/// Left: the places (the project library, Recent Projects). Top: the folder path (each part
/// is a button and a drop target), search, sort (name / date, order), cards or list, card
/// size. Middle: the projects and folders of the shown folder as cards (ProjectCardDelegate)
/// or as a list with sortable columns, over one ProjectLibraryModel + ProjectSortProxy.
/// Bottom: Import, Export, New Folder, New Project, Open.
///
/// Opening goes through openRequested(): the window asks the switch guard first (the editor's
/// "save changes?" prompt) and only emits when the user did not cancel; the receiver opens the
/// project and closes the window. Every action is a public slot (dialogs) with a dialog-free
/// variant for tests. Keys: Enter opens, Delete moves to the trash, F2 renames, Backspace goes
/// up a folder. Non-modal by itself (show(); the caller sets the modality), so start-up and
/// tests never block on it.
class ProjectManagerWindow : public QDialog
{
    Q_OBJECT
public:
    /// @p library must outlive the window.
    explicit ProjectManagerWindow(ProjectLibrary* library, QWidget* parent = nullptr);
    ~ProjectManagerWindow() override;

    /// Called before a project is opened or created; returns false to stay (the user
    /// cancelled the "save changes?" prompt). Receives the window as the dialog parent.
    void setSwitchGuard(std::function<bool(QWidget*)> guard) { m_guard = std::move(guard); }
    /// The project open in the editor (marked "Open", cannot be deleted / renamed / moved).
    void setOpenProjectPath(const QString& path);
    /// Recent projects (File > Open Recent's list), shown under Recent Projects.
    void setRecentProjects(const QStringList& paths);

    /// Shows the library folder @p folder (absolute; empty = the library root).
    void showFolder(const QString& folder = {});
    /// Shows the Recent Projects list.
    void showRecent();
    /// Card (true) or list view.
    void setCardView(bool cards);
    [[nodiscard]] bool isCardView() const;
    /// Sorts by @p column (ProjectLibraryModel::Column) in @p order.
    void sortBy(int column, Qt::SortOrder order);
    /// Search text (filters by name).
    void setSearchText(const QString& text);

    /// Paths of the selected rows (in view order).
    [[nodiscard]] QStringList selectedPaths() const;
    /// Selects @p path (and makes it current); false if it is not shown.
    bool selectPath(const QString& path);

    // ---- Dialog-free operations (also used by the slots below) ----

    /// Creates a project named @p name of kind @p kindId (empty = NewProjectDialog's
    /// preferred kind) in the shown folder and asks to open it.
    QString createProject(const QString& name, const QString& kindId = {});
    /// Creates a folder named @p name in the shown folder.
    QString createFolder(const QString& name);
    /// Moves @p paths to the trash, after a confirmation when @p ask is set.
    bool trash(const QStringList& paths, bool ask);
    /// Opens @p path: a folder is entered, a project is requested (switch guard first).
    void openPath(const QString& path);

    [[nodiscard]] ProjectLibrary* library() const { return m_library; }
    [[nodiscard]] ProjectLibraryModel* model() const { return m_model; }
    [[nodiscard]] ProjectSortProxy* proxy() const { return m_proxy; }
    [[nodiscard]] QListView* cardView() const { return m_cardView; }
    [[nodiscard]] QTreeView* listView() const { return m_listView; }
    [[nodiscard]] ProjectCardDelegate* cardDelegate() const { return m_delegate; }
    [[nodiscard]] QSlider* cardSizeSlider() const { return m_sizeSlider; }

public slots:
    void newProject(); ///< asks for the name and the kind (NewProjectDialog)
    void newFolder(); ///< asks for the name
    void openSelected(); ///< the current item
    void renameSelected(); ///< inline editor on the current item
    void duplicateSelected();
    void deleteSelected(); ///< to the trash, with confirmation
    void showSelectedInFileManager(); ///< the current item, or the shown folder
    void importProject(); ///< file dialog, copies into the shown folder
    void exportSelected(); ///< save dialog
    void goUp(); ///< parent folder (not above the library root)

signals:
    /// The user chose to open @p path (the switch guard already agreed).
    void openRequested(const QString& path);
    /// Project Settings for the open project.
    void projectSettingsRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void rebuildCrumbs();
    void updateActions();
    void showContextMenu(QAbstractItemView* view, const QPoint& pos);
    void applyCardWidth(int width);
    void reportError(const QString& message);
    void saveSettings() const;
    void restoreSettings();
    [[nodiscard]] QAbstractItemView* activeView() const;
    [[nodiscard]] QString currentPath() const;

    ProjectLibrary* m_library;
    ProjectLibraryModel* m_model = nullptr;
    ProjectSortProxy* m_proxy = nullptr;
    ProjectCardDelegate* m_delegate = nullptr;
    std::function<bool(QWidget*)> m_guard;
    QStringList m_recent;

    QListWidget* m_places = nullptr;
    QToolButton* m_upButton = nullptr;
    QWidget* m_crumbs = nullptr;
    QHBoxLayout* m_crumbLayout = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_sortCombo = nullptr;
    QToolButton* m_orderButton = nullptr;
    QToolButton* m_cardsButton = nullptr;
    QToolButton* m_listButton = nullptr;
    QSlider* m_sizeSlider = nullptr;
    QStackedWidget* m_views = nullptr;
    QListView* m_cardView = nullptr;
    QTreeView* m_listView = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_importButton = nullptr;
    QPushButton* m_exportButton = nullptr;
    QPushButton* m_newFolderButton = nullptr;
    QPushButton* m_newProjectButton = nullptr;
    QPushButton* m_openButton = nullptr;
};

} // namespace kk
