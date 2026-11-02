// SPDX-License-Identifier: GPL-3.0-or-later
// PreferencesDialog implementation: System / User tabs, section list, stacked section pages.
// The standard pages themselves are in PreferencePages.cpp.
#include "ui/dialogs/PreferencesDialog.h"

#include "ui/common/Metrics.h"
#include "ui/common/SettingsPages.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

const QSize kDialogSize(860, 600); ///< Resolve's preferences window proportions
constexpr int kSectionListWidth = 200;
constexpr int kTitlePointDelta = 2; ///< section title: base font + 2 pt
constexpr int kSectionRole = Qt::UserRole; ///< section id on list items
constexpr int kGroupRole = Qt::UserRole + 1; ///< group id on list items

QString groupTitle(const QString& group)
{
    return group == QLatin1String("system") ? PreferencesDialog::tr("System")
                                            : PreferencesDialog::tr("User");
}

/// One section to show: its pages in display order.
struct Section
{
    QString id;
    QString group;
    QString title;
    QVector<SettingsPageInfo> pages;
};

/// Groups the registered pages into sections (registry order = page order).
QVector<Section> buildSections(const QVector<SettingsPageInfo>& pages)
{
    QVector<Section> sections;
    for (const SettingsPageInfo& info : pages) {
        const QString id = info.section.isEmpty() ? info.id : info.section;
        auto it = std::find_if(
            sections.begin(), sections.end(), [&id](const Section& s) { return s.id == id; });
        if (it == sections.end()) {
            sections.append(Section {id, info.group, info.title, {}});
            it = sections.end() - 1;
        }
        it->pages.append(info);
        if (info.id == id) {
            // The page that owns the section id names it and decides its group.
            it->title = info.title;
            it->group = info.group;
        }
    }
    return sections;
}

/// Gives every form label of @p page the same minimum width, so fields of all pages in a
/// section start at the same x (pages are built independently).
void alignForms(QWidget* page)
{
    for (QFormLayout* form : page->findChildren<QFormLayout*>()) {
        // Left-aligned labels in one column, as in Resolve's preferences.
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(metrics::kSpaceL);
        form->setVerticalSpacing(metrics::kSpaceM);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        for (int row = 0; row < form->rowCount(); ++row) {
            if (QLayoutItem* item = form->itemAt(row, QFormLayout::LabelRole)) {
                if (QWidget* label = item->widget())
                    label->setMinimumWidth(metrics::kFormLabelMinWidth);
            }
            if (QLayoutItem* item = form->itemAt(row, QFormLayout::FieldRole)) {
                if (auto* combo = qobject_cast<QComboBox*>(item->widget()))
                    combo->setMinimumWidth(metrics::kFormComboMinWidth);
            }
        }
    }
}

QLabel* heading(const QString& text, int pointDelta)
{
    auto* label = new QLabel(text);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() + pointDelta);
    label->setFont(font);
    return label;
}

} // namespace

PreferencesDialog::PreferencesDialog(const SettingsPageRegistry* registry, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    resize(kDialogSize);

    // Without a registry (tests, tools) the dialog shows the standard pages.
    SettingsPageRegistry standard;
    if (!registry) {
        registerStandardSettingsPages(standard);
        registry = &standard;
    }
    const QVector<Section> sections = buildSections(registry->pages());

    // ---- System / User tabs (centred, like Resolve's) ----
    m_groups = new QTabBar;
    m_groups->setObjectName(QStringLiteral("PreferencesGroups"));
    m_groups->setDocumentMode(true);
    m_groups->setDrawBase(false);
    m_groups->setExpanding(false);
    for (const QString& group : {QStringLiteral("system"), QStringLiteral("user")}) {
        const bool used = std::any_of(sections.cbegin(), sections.cend(),
            [&group](const Section& s) { return s.group == group; });
        if (used) {
            m_groupIds.append(group);
            m_groups->addTab(groupTitle(group));
        }
    }
    auto* tabRow = new QHBoxLayout;
    tabRow->addStretch(1);
    tabRow->addWidget(m_groups);
    tabRow->addStretch(1);

    // ---- Section list | section pages ----
    m_sections = new QListWidget;
    m_sections->setObjectName(QStringLiteral("PreferencesSections"));
    m_sections->setFixedWidth(kSectionListWidth);
    m_sections->setFrameShape(QFrame::NoFrame);
    m_sections->setSpacing(metrics::kSpaceXs);
    m_stack = new QStackedWidget;
    for (const Section& section : sections) {
        auto* content = new QWidget;
        content->setObjectName(QStringLiteral("PreferencesSection_") + section.id);
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(
            metrics::kSpaceXl, metrics::kSpaceL, metrics::kSpaceXl, metrics::kSpaceL);
        layout->setSpacing(metrics::kSpaceL);
        layout->addWidget(heading(section.title, kTitlePointDelta));
        for (const SettingsPageInfo& info : section.pages) {
            // Several pages in one section continue one list of settings (no group boxes:
            // their titles sat oddly in Breeze, owner feedback on alpha.1).
            SettingsPage* page = info.create(content);
            page->setObjectName(QStringLiteral("SettingsPage_") + info.id);
            alignForms(page);
            layout->addWidget(page);
            m_pages.append(page);
        }
        layout->addStretch(1);
        auto* scroll = new QScrollArea;
        scroll->setWidget(content);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        m_stack->addWidget(scroll);

        auto* item = new QListWidgetItem(section.title, m_sections);
        item->setData(kSectionRole, section.id);
        item->setData(kGroupRole, section.group);
    }
    connect(m_sections, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    connect(m_groups, &QTabBar::currentChanged, this, &PreferencesDialog::showGroup);

    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    auto* body = new QHBoxLayout;
    body->setSpacing(0);
    body->addWidget(m_sections);
    body->addWidget(separator);
    body->addWidget(m_stack, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceL, metrics::kSpaceM, metrics::kSpaceL, metrics::kSpaceL);
    layout->setSpacing(metrics::kSpaceM);
    layout->addLayout(tabRow);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);

    // Resolve opens on the User tab (the settings people change most).
    const int userTab = int(m_groupIds.indexOf(QStringLiteral("user")));
    m_groups->setCurrentIndex(std::max(0, userTab));
    showGroup(m_groups->currentIndex());
}

void PreferencesDialog::showGroup(int group)
{
    const QString id = m_groupIds.value(group);
    int first = -1;
    for (int row = 0; row < m_sections->count(); ++row) {
        QListWidgetItem* item = m_sections->item(row);
        const bool inGroup = item->data(kGroupRole).toString() == id;
        item->setHidden(!inGroup);
        if (inGroup && first < 0)
            first = row;
    }
    const QListWidgetItem* current = m_sections->currentItem();
    if (!current || current->isHidden())
        m_sections->setCurrentRow(first);
}

bool PreferencesDialog::showSection(const QString& sectionId)
{
    for (int row = 0; row < m_sections->count(); ++row) {
        QListWidgetItem* item = m_sections->item(row);
        if (item->data(kSectionRole).toString() != sectionId)
            continue;
        m_groups->setCurrentIndex(int(m_groupIds.indexOf(item->data(kGroupRole).toString())));
        m_sections->setCurrentRow(row);
        return true;
    }
    return false;
}

void PreferencesDialog::apply()
{
    for (SettingsPage* page : std::as_const(m_pages))
        page->apply();
    emit preferencesChanged();
}

void PreferencesDialog::accept()
{
    apply();
    QDialog::accept();
}

} // namespace kk
