// SPDX-License-Identifier: GPL-3.0-or-later
// MediaBrowserPanel — PLACEHOLDER file-system browser of the Media page.
// The panels agent adds favourites/places, previews on selection and multi-select import.
#pragma once

#include "ui/common/PanelFrame.h"

class QFileSystemModel;
class QTreeView;

namespace kk {

struct AppContext;

/// Browses the local file system; double-clicking a file imports it into the media pool.
class MediaBrowserPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the browser rooted at the user's home directory.
    MediaBrowserPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Changes the displayed directory.
    void setRootPath(const QString& path);

signals:
    /// A file was selected (e.g. to preview it in the viewer).
    void fileSelected(const QString& path);

private:
    AppContext& m_ctx;
    QFileSystemModel* m_model = nullptr;
    QTreeView* m_view = nullptr;
};

} // namespace kk
