// SPDX-License-Identifier: GPL-3.0-or-later
// MetadataPanel — read-only technical metadata (probe result) of a media item.
#pragma once

#include "ui/common/PanelFrame.h"

class QFormLayout;
class QStackedWidget;

namespace kk {

struct AppContext;

/// Shows the probe result (MediaItem::info) of one media item, grouped into Clip / Video /
/// Audio sections, and whether the file is offline. Follows renames and undo.
class MetadataPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates an empty panel.
    MetadataPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Shows the metadata of media item @p mediaId (empty = none).
    void showMedia(const QString& mediaId);

private:
    AppContext& m_ctx;
    QStackedWidget* m_stack = nullptr;
    QFormLayout* m_form = nullptr;
    QString m_mediaId; ///< item shown (refreshed when the pool changes)
};

} // namespace kk
