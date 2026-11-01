// SPDX-License-Identifier: GPL-3.0-or-later
// DialogActions implementation.
#include "ui/dialogs/DialogActions.h"

#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"

#include <QAction>
#include <QWidget>

namespace kk {

void connectDialogActions(
    AppContext& ctx, QWidget* window, std::function<void()> onPreferencesChanged)
{
    applyHwAccelPreference(); // the stored choice applies from start-up on
    ActionRegistry& actions = *ctx.actions;
    QObject::connect(actions.action(QStringLiteral("file.project_settings")), &QAction::triggered,
        window, [&ctx, window] {
            ProjectSettingsDialog dialog(ctx.project, window);
            dialog.exec();
        });
    QObject::connect(actions.action(QStringLiteral("edit.keyboard_customization")),
        &QAction::triggered, window, [&ctx, window] {
            KeyboardCustomizationDialog dialog(*ctx.actions, window);
            dialog.exec();
        });
    QObject::connect(actions.action(QStringLiteral("edit.preferences")), &QAction::triggered,
        window, [&ctx, window, onPreferencesChanged] {
            PreferencesDialog dialog(ctx.actions, window);
            if (onPreferencesChanged)
                QObject::connect(
                    &dialog, &PreferencesDialog::preferencesChanged, window, onPreferencesChanged);
            dialog.exec();
        });
}

} // namespace kk
