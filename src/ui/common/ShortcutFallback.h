// SPDX-License-Identifier: GPL-3.0-or-later
// ShortcutFallback — makes shortcuts written with the unshifted key ("Ctrl+Shift+[", "Shift+,")
// work when the platform reports the shifted character instead (Ctrl+Shift+{ on a US layout,
// seen under Wayland with Qt 6): an application event filter that maps an unmatched key press
// back to the US base key and triggers the action bound to it.
#pragma once

#include <QObject>

namespace kk {

class ActionRegistry;

class ShortcutFallback : public QObject
{
    Q_OBJECT
public:
    /// Installs itself as an application event filter (removed again when destroyed).
    explicit ShortcutFallback(ActionRegistry& registry, QObject* parent = nullptr);
    ~ShortcutFallback() override;

    /// The US-layout base key of a shifted character (Qt::Key_BraceLeft -> Qt::Key_BracketLeft,
    /// Qt::Key_Less -> Qt::Key_Comma, Qt::Key_Exclam -> Qt::Key_1, ...), or 0.
    [[nodiscard]] static int unshiftedKey(int key);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ActionRegistry& m_registry;
};

} // namespace kk
