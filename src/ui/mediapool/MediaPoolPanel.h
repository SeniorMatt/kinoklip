// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel — PLACEHOLDER media pool: icon list of the imported items, drop to import.
// The panels agent adds the bin tree, thumbnails (MediaCache), list/grid modes and dragging
// clips to the timeline/source viewer; keep the public API.
#pragma once

#include "ui/common/PanelFrame.h"

#include <QStringList>

class QListWidget;
class QStackedWidget;

namespace cw {

struct AppContext;

/// Shows the MediaPool's items. Accepts files dropped from a file manager (imports them).
class MediaPoolPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the panel for ctx.project's media pool.
    MediaPoolPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Ids of the selected media items.
    [[nodiscard]] QStringList selectedMediaIds() const;
    /// The current (focused) media item id, or empty.
    [[nodiscard]] QString currentMediaId() const;

signals:
    /// The current item changed (e.g. to update the metadata panel). Empty = none.
    void currentMediaChanged(const QString& mediaId);
    /// The user double-clicked an item (e.g. open it in the source viewer).
    void mediaActivated(const QString& mediaId);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void rebuild();

    AppContext& m_ctx;
    QStackedWidget* m_stack = nullptr;
    QListWidget* m_list = nullptr;
};

} // namespace cw
