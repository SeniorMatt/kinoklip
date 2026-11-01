// SPDX-License-Identifier: GPL-3.0-or-later
// MetadataPanel — PLACEHOLDER read-only technical metadata of a media item.
// The panels agent may add editable fields (scene, take, notes) later.
#pragma once

#include "ui/common/PanelFrame.h"

class QFormLayout;
class QStackedWidget;

namespace kk {

struct AppContext;

/// Shows the probe result (MediaItem::info) of one media item.
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
};

} // namespace kk
