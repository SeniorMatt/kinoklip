// SPDX-License-Identifier: GPL-3.0-or-later
// DialogActions implementation.
#include "ui/dialogs/DialogActions.h"

#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/dialogs/ClipAttributesDialog.h"
#include "ui/dialogs/HistoryWindow.h"
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
        window, [&ctx, window, onPreferencesChanged] {
            ProjectSettingsDialog dialog(ctx.project, window);
            if (onPreferencesChanged) // the autosave interval lives in General Options too
                QObject::connect(&dialog, &ProjectSettingsDialog::preferencesChanged, window,
                    onPreferencesChanged);
            dialog.exec();
        });
    // Clip > Clip Attributes: the media of the clips selected in the timeline, else the media
    // pool selection (published into the action's data by the media pool panel).
    if (actions.contains(QStringLiteral("clip.attributes"))) {
        QAction* attributes = actions.action(QStringLiteral("clip.attributes"));
        QObject::connect(attributes, &QAction::triggered, window, [&ctx, window, attributes] {
            QStringList ids = clipattributes::timelineTargets(ctx);
            if (ids.isEmpty())
                ids = attributes->data().toStringList();
            clipattributes::edit(*ctx.project, ids, window);
        });
    }
    // Edit > History > Open History Window: one non-modal window per main window, kept
    // (hidden) when closed so it reopens where the user left it.
    QObject::connect(actions.action(QStringLiteral("edit.history_window")), &QAction::triggered,
        window, [&ctx, window] {
            auto* history = window->findChild<HistoryWindow*>();
            if (!history)
                history = new HistoryWindow(ctx.project, window);
            history->show();
            history->raise();
            history->activateWindow();
        });
    QObject::connect(actions.action(QStringLiteral("edit.keyboard_customization")),
        &QAction::triggered, window, [&ctx, window] {
            KeyboardCustomizationDialog dialog(*ctx.actions, window);
            dialog.exec();
        });
    QObject::connect(actions.action(QStringLiteral("edit.preferences")), &QAction::triggered,
        window, [&ctx, window, onPreferencesChanged] {
            PreferencesDialog dialog(ctx.settingsPages, window);
            if (onPreferencesChanged)
                QObject::connect(
                    &dialog, &PreferencesDialog::preferencesChanged, window, onPreferencesChanged);
            dialog.exec();
        });
}

} // namespace kk
