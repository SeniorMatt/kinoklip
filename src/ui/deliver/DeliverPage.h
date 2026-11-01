// SPDX-License-Identifier: GPL-3.0-or-later
// DeliverPage — PLACEHOLDER render page: settings | viewer | render queue over a mini timeline.
// The export agent replaces the implementation; keep the class name and constructor.
#pragma once

#include "ui/common/Page.h"

class QSplitter;

namespace kk {

struct AppContext;
class ViewerPanel;

/// The Deliver page (PLAN §4): render settings (left), viewer (centre), render queue (right),
/// mini timeline (bottom).
class DeliverPage : public Page
{
    Q_OBJECT
public:
    /// Builds the page.
    DeliverPage(AppContext& ctx, QWidget* parent = nullptr);

    [[nodiscard]] QString pageId() const override { return QStringLiteral("deliver"); }
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;

    /// The page's program viewer.
    [[nodiscard]] ViewerPanel* viewer() const { return m_viewer; }

private:
    AppContext& m_ctx;
    ViewerPanel* m_viewer = nullptr;
    QSplitter* m_topRow = nullptr;
    QSplitter* m_main = nullptr;
};

} // namespace kk
