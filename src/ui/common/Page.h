// SPDX-License-Identifier: GPL-3.0-or-later
// Page — base class of the main window's pages (Media, Edit, Deliver, ...).
#pragma once

#include <QString>
#include <QWidget>

class QSettings;

namespace kk {

/// Panels that the interface toolbar can show/hide on a page.
enum class PanelId {
    MediaPool,
    Effects,
    EditIndex,
    Mixer,
    Metadata,
    Inspector,
    Keyframes ///< the large keyframe editor (dockable / floating)
};

/// Stable string of a PanelId, used for settings keys ("mediaPool", "inspector", ...).
[[nodiscard]] inline QString panelIdToString(PanelId id)
{
    switch (id) {
    case PanelId::MediaPool:
        return QStringLiteral("mediaPool");
    case PanelId::Effects:
        return QStringLiteral("effects");
    case PanelId::EditIndex:
        return QStringLiteral("editIndex");
    case PanelId::Mixer:
        return QStringLiteral("mixer");
    case PanelId::Metadata:
        return QStringLiteral("metadata");
    case PanelId::Inspector:
        return QStringLiteral("inspector");
    case PanelId::Keyframes:
        return QStringLiteral("keyframes");
    }
    return {};
}

/// All toggleable panels, in interface-toolbar order.
inline constexpr PanelId kAllPanels[] = {PanelId::MediaPool, PanelId::Effects, PanelId::EditIndex,
    PanelId::Keyframes, PanelId::Mixer, PanelId::Metadata, PanelId::Inspector};

/// A full-window page selected from the page bar. Pages report which toggleable panels they
/// have; the main window keeps the interface-toolbar toggles in sync with them.
class Page : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;

    /// Stable id used for settings and the --screenshot option ("media", "edit", "deliver").
    [[nodiscard]] virtual QString pageId() const = 0;

    /// True if the page contains a panel the interface toolbar can toggle.
    [[nodiscard]] virtual bool hasPanel(PanelId id) const
    {
        Q_UNUSED(id);
        return false;
    }
    /// Shows/hides a toggleable panel (ignored if !hasPanel(id)).
    virtual void setPanelVisible(PanelId id, bool visible)
    {
        Q_UNUSED(id);
        Q_UNUSED(visible);
    }
    /// Current visibility of a toggleable panel.
    [[nodiscard]] virtual bool isPanelVisible(PanelId id) const
    {
        Q_UNUSED(id);
        return false;
    }

    /// Saves splitter sizes / panel visibility under the settings group of this page.
    virtual void saveLayout(QSettings& settings) const { Q_UNUSED(settings); }
    /// Restores what saveLayout() wrote (missing keys keep the defaults).
    virtual void restoreLayout(const QSettings& settings) { Q_UNUSED(settings); }

signals:
    /// A panel was shown or hidden (by the toolbar or by the page itself).
    void panelVisibilityChanged(kk::PanelId id, bool visible);
};

} // namespace kk
