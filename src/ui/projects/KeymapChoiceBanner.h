// SPDX-License-Identifier: GPL-3.0-or-later
// KeymapChoiceBanner — the one-time "Coming from another editor?" choice of keyboard preset at
// the top of the Project Manager on the first start.
#pragma once

#include <QFrame>

class QComboBox;

namespace kk {

class ActionRegistry;

/// A slim bar: "Coming from another editor? Use its keyboard shortcuts: [preset v] [Use]
/// [No Thanks]". The presets are the registry's built-in profiles, the default (DaVinci
/// Resolve) first. Use activates the chosen preset; both buttons hide the bar for good.
///
/// shouldOffer() is true on the very first start only: no keyboard profile was ever chosen
/// (QSettings "keymap/profile" unset) and the bar was never shown. The bar records itself as
/// shown ("keymap/choiceOffered") the first time it appears, so a user who ignores it and goes
/// on is not asked again; Keyboard Customization offers the same presets at any time.
class KeymapChoiceBanner : public QFrame
{
    Q_OBJECT
public:
    /// @p registry must outlive the banner.
    explicit KeymapChoiceBanner(ActionRegistry& registry, QWidget* parent = nullptr);

    /// True when the choice should be offered (first start, see the class comment).
    [[nodiscard]] static bool shouldOffer();
    /// The preset list (tests).
    [[nodiscard]] QComboBox* presetCombo() const { return m_presets; }
    /// Activates the preset shown in the list and hides the bar (the Use button).
    void useSelected();
    /// Hides the bar without changing the keys (the No Thanks button).
    void skip();

signals:
    /// The user chose (@p profileId is the activated preset) or skipped (empty).
    void chosen(const QString& profileId);

protected:
    void showEvent(QShowEvent* event) override;

private:
    ActionRegistry& m_registry;
    QComboBox* m_presets = nullptr;
};

} // namespace kk
