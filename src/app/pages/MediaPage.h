// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPage — browse the file system, preview, inspect metadata and organise the media pool.
#pragma once

#include "ui/common/Page.h"

class QSplitter;

namespace cw {

struct AppContext;
class MediaBrowserPanel;
class MediaPoolPanel;
class MetadataPanel;
class ViewerPanel;

/// Media page layout (PLAN §4):
/// ```
/// | Media storage browser | Preview viewer | Metadata |
/// |                 Media Pool                       |
/// ```
class MediaPage : public Page
{
    Q_OBJECT
public:
    /// Builds the page and its panels.
    MediaPage(AppContext& ctx, QWidget* parent = nullptr);

    [[nodiscard]] QString pageId() const override { return QStringLiteral("media"); }
    [[nodiscard]] bool hasPanel(PanelId id) const override;
    void setPanelVisible(PanelId id, bool visible) override;
    [[nodiscard]] bool isPanelVisible(PanelId id) const override;
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;

    [[nodiscard]] MediaBrowserPanel* browser() const { return m_browser; }
    [[nodiscard]] ViewerPanel* viewer() const { return m_viewer; }
    [[nodiscard]] MetadataPanel* metadata() const { return m_metadata; }
    [[nodiscard]] MediaPoolPanel* mediaPool() const { return m_mediaPool; }

private:
    [[nodiscard]] QWidget* panel(PanelId id) const;

    AppContext& m_ctx;
    MediaBrowserPanel* m_browser = nullptr;
    ViewerPanel* m_viewer = nullptr;
    MetadataPanel* m_metadata = nullptr;
    MediaPoolPanel* m_mediaPool = nullptr;
    QSplitter* m_topRow = nullptr;
    QSplitter* m_main = nullptr;
};

} // namespace cw
