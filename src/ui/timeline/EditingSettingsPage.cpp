// SPDX-License-Identifier: GPL-3.0-or-later
// EditingSettingsPage implementation: two combo boxes bound to EditBehaviour.
#include "ui/timeline/EditingSettingsPage.h"

#include "core/EditBehaviour.h"
#include "ui/common/SettingsPages.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::EditingSettingsPage", text);
}

/// Mouse wheel behaviour of the timeline.
class EditingSettingsPage final : public SettingsPage
{
public:
    explicit EditingSettingsPage(QWidget* parent)
        : SettingsPage(parent)
    {
        const EditBehaviour b = EditBehaviour::fromSettings();
        auto* form = new QFormLayout(this);
        m_anchor = new QComboBox;
        m_anchor->setObjectName(QStringLiteral("zoomAround"));
        m_anchor->addItem(tr("Playhead"), int(ZoomAnchor::Playhead));
        m_anchor->addItem(tr("Mouse cursor"), int(ZoomAnchor::Cursor));
        m_anchor->setCurrentIndex(m_anchor->findData(int(b.zoomAnchor)));
        form->addRow(tr("Zoom around"), m_anchor);
        m_wheel = new QComboBox;
        m_wheel->setObjectName(QStringLiteral("wheelScheme"));
        m_wheel->addItem(tr("Resolve (Alt zooms, Ctrl scrolls time, Shift track height)"),
            int(WheelScheme::Resolve));
        m_wheel->addItem(tr("Classic (Ctrl zooms, Shift scrolls time)"), int(WheelScheme::Classic));
        m_wheel->setCurrentIndex(m_wheel->findData(int(b.wheelScheme)));
        form->addRow(tr("Mouse wheel"), m_wheel);
    }

    void apply() override
    {
        EditBehaviour::setZoomAnchor(ZoomAnchor(m_anchor->currentData().toInt()));
        EditBehaviour::setWheelScheme(WheelScheme(m_wheel->currentData().toInt()));
    }

private:
    QComboBox* m_anchor = nullptr;
    QComboBox* m_wheel = nullptr;
};

} // namespace

void registerEditingSettingsPage(SettingsPageRegistry& registry)
{
    // Order 30: between "Project" (20) and "Interface" (40).
    registry.add({QStringLiteral("editing"), tr("Editing"), QStringLiteral("edit-cut"), 30,
        [](QWidget* parent) { return new EditingSettingsPage(parent); }});
}

} // namespace kk
