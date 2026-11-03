// SPDX-License-Identifier: GPL-3.0-or-later
// NewProjectDialog implementation.
#include "ui/projects/NewProjectDialog.h"

#include "projects/ProjectKinds.h"
#include "ui/common/Metrics.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr char kKindKey[] = "projectManager/newProjectKind";

QString kindIdFor(ProjectStorage storage)
{
    const ProjectKind* k = projectkinds::byStorage(storage);
    return k ? k->id : QString();
}

bool creatable(ProjectStorage storage)
{
    const ProjectKind* k = projectkinds::byStorage(storage);
    return k && k->canCreate();
}
} // namespace

NewProjectDialog::NewProjectDialog(const QString& suggestedName, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New Project"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceL, metrics::kSpaceL, metrics::kSpaceL, metrics::kSpaceL);
    auto* form = new QFormLayout;
    m_name = new QLineEdit(suggestedName);
    m_name->setObjectName(QStringLiteral("ProjectName"));
    m_name->setMinimumWidth(320);
    m_name->selectAll();
    form->addRow(tr("Project name:"), m_name);

    m_single = new QRadioButton(tr("Single file"));
    m_single->setObjectName(QStringLiteral("SingleFileKind"));
    m_linked = new QRadioButton(tr("Linked"));
    m_linked->setObjectName(QStringLiteral("LinkedKind"));
    auto* kinds = new QVBoxLayout;
    kinds->setSpacing(metrics::kSpaceXs);
    const auto addKind = [&](QRadioButton* button, const QString& note) {
        kinds->addWidget(button);
        auto* label = new QLabel(note);
        label->setEnabled(false); // dim, like a form note
        label->setWordWrap(true);
        label->setContentsMargins(metrics::kSpaceXl + metrics::kSpaceS, 0, 0, metrics::kSpaceS);
        kinds->addWidget(label);
        return label;
    };
    QLabel* singleNote = addKind(m_single,
        tr("Everything in one file: the media is copied in, so the project moves like any "
           "other file."));
    addKind(m_linked, tr("The media stays where it is; the project file refers to it."));
    form->addRow(tr("Kind:"), kinds);
    layout->addLayout(form);

    if (!creatable(ProjectStorage::SingleFile)) {
        // Only a build without the single-file container (a kind registered without create).
        const QString why = tr("This build cannot create single-file projects");
        m_single->setEnabled(false);
        m_single->setToolTip(why);
        singleNote->setToolTip(why);
    }
    m_linked->setEnabled(creatable(ProjectStorage::Linked));
    (preferredKindId() == kindIdFor(ProjectStorage::SingleFile) ? m_single : m_linked)
        ->setChecked(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    connect(buttons, &QDialogButtonBox::accepted, this, &NewProjectDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &NewProjectDialog::reject);
    connect(m_name, &QLineEdit::textChanged, this, [this, buttons](const QString& t) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!t.trimmed().isEmpty());
    });
    layout->addWidget(buttons);
}

QString NewProjectDialog::name() const
{
    return m_name->text().trimmed();
}

QString NewProjectDialog::kindId() const
{
    return kindIdFor(m_single->isChecked() ? ProjectStorage::SingleFile : ProjectStorage::Linked);
}

QString NewProjectDialog::preferredKindId()
{
    const QString stored = QSettings().value(QLatin1String(kKindKey)).toString();
    if (const ProjectKind* k = projectkinds::byId(stored); k && k->canCreate())
        return k->id;
    // Owner decision: Single file is the default; Linked while this build cannot create it.
    if (creatable(ProjectStorage::SingleFile))
        return kindIdFor(ProjectStorage::SingleFile);
    return kindIdFor(ProjectStorage::Linked);
}

void NewProjectDialog::rememberKind(const QString& kindId)
{
    QSettings().setValue(QLatin1String(kKindKey), kindId);
}

void NewProjectDialog::accept()
{
    if (name().isEmpty())
        return;
    // Only an explicit, creatable choice is remembered (never the fallback of a build without
    // the single-file container, so the default comes back once it is integrated).
    if (m_single->isEnabled() && m_linked->isEnabled())
        rememberKind(kindId());
    QDialog::accept();
}

} // namespace kk
