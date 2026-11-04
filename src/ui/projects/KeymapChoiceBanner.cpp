// SPDX-License-Identifier: GPL-3.0-or-later
// KeymapChoiceBanner implementation.
#include "ui/projects/KeymapChoiceBanner.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/Metrics.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>

namespace kk {

namespace {

const QString kProfileKey = QStringLiteral("keymap/profile");
const QString kOfferedKey = QStringLiteral("keymap/choiceOffered");

} // namespace

KeymapChoiceBanner::KeymapChoiceBanner(ActionRegistry& registry, QWidget* parent)
    : QFrame(parent)
    , m_registry(registry)
{
    setObjectName(QStringLiteral("KeymapChoiceBanner"));
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
    layout->setSpacing(metrics::kSpaceS);
    auto* label = new QLabel(tr("Coming from another editor? Use its keyboard shortcuts:"));
    m_presets = new QComboBox;
    m_presets->setObjectName(QStringLiteral("KeymapChoicePresets"));
    for (const ActionRegistry::KeymapProfile& p : registry.profiles()) {
        if (!p.builtIn)
            continue;
        m_presets->addItem(p.id == QLatin1String(ActionRegistry::kDefaultProfile)
                ? tr("%1 (default)").arg(p.name)
                : p.name,
            p.id);
        if (p.id == registry.activeProfile())
            m_presets->setCurrentIndex(m_presets->count() - 1);
    }
    auto* use = new QPushButton(tr("Use"));
    use->setObjectName(QStringLiteral("KeymapChoiceUse"));
    auto* skipButton = new QPushButton(tr("No Thanks"));
    skipButton->setObjectName(QStringLiteral("KeymapChoiceSkip"));
    skipButton->setToolTip(tr("Keep Kinoklip's keys (DaVinci Resolve's). Keyboard "
                              "Customization offers the other editors' keys at any time."));
    layout->addWidget(label);
    layout->addWidget(m_presets);
    layout->addWidget(use);
    layout->addStretch(1);
    layout->addWidget(skipButton);
    connect(use, &QPushButton::clicked, this, &KeymapChoiceBanner::useSelected);
    connect(skipButton, &QPushButton::clicked, this, &KeymapChoiceBanner::skip);
}

bool KeymapChoiceBanner::shouldOffer()
{
    const QSettings settings;
    return !settings.contains(kProfileKey) && !settings.value(kOfferedKey, false).toBool();
}

void KeymapChoiceBanner::showEvent(QShowEvent* event)
{
    QFrame::showEvent(event);
    QSettings().setValue(kOfferedKey, true);
}

void KeymapChoiceBanner::useSelected()
{
    const QString id = m_presets->currentData().toString();
    QString error;
    if (!m_registry.setActiveProfile(id, &error)) {
        QMessageBox::warning(this, tr("Keyboard Profile"), error);
        return;
    }
    QSettings().setValue(kOfferedKey, true);
    hide();
    emit chosen(id);
}

void KeymapChoiceBanner::skip()
{
    // Writing the default profile marks the keymap as chosen, like a choice would.
    m_registry.setActiveProfile(m_registry.activeProfile());
    QSettings().setValue(kOfferedKey, true);
    hide();
    emit chosen({});
}

} // namespace kk
