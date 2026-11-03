// SPDX-License-Identifier: GPL-3.0-or-later
// NewProjectDialog — the Project Manager's New Project: the name and the kind of project.
#pragma once

#include <QDialog>

class QLineEdit;
class QRadioButton;

namespace kk {

/// Asks for the project name and the kind: **Single file** (everything in one file, media
/// copied in; the default) or **Linked** (media stays where it is). The last choice is
/// remembered (QSettings `projectManager/newProjectKind`). A kind this build cannot create
/// (ProjectKind::canCreate(); the single-file container before its integration) is shown
/// disabled with a tooltip, and the dialog falls back to the other one.
class NewProjectDialog : public QDialog
{
    Q_OBJECT
public:
    NewProjectDialog(const QString& suggestedName, QWidget* parent = nullptr);

    /// The typed name (trimmed).
    [[nodiscard]] QString name() const;
    /// ProjectKind::id of the chosen kind.
    [[nodiscard]] QString kindId() const;
    /// The kind New Project offers first: the remembered choice if this build can create it,
    /// else Single file if it can, else Linked.
    [[nodiscard]] static QString preferredKindId();
    /// Remembers @p kindId as the last choice.
    static void rememberKind(const QString& kindId);

    [[nodiscard]] QLineEdit* nameEdit() const { return m_name; }
    [[nodiscard]] QRadioButton* singleFileButton() const { return m_single; }
    [[nodiscard]] QRadioButton* linkedButton() const { return m_linked; }

    void accept() override;

private:
    QLineEdit* m_name = nullptr;
    QRadioButton* m_single = nullptr;
    QRadioButton* m_linked = nullptr;
};

} // namespace kk
