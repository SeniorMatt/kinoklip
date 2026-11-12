// SPDX-License-Identifier: GPL-3.0-or-later
// VimKeyframeHost — the keys of context "keyframes" (spec section 83, 5.7): the keyboard cursor of
// the Keyframes panel's dope sheet and curve editor. The cursor is a row and a frame, drawn as a
// ring round a keyframe; h and l move over the keys of the row, j and k over the rows. The work is
// done by the keyboard functions of KeyframeViewBase, which use the same commands as the mouse.
#pragma once

#include "ui/common/VimPanelHost.h"
#include "ui/keyframes/KeyframesPanel.h"

#include <QPointer>

namespace kk {

class KeyframeViewBase;

class VimKeyframeHost final : public VimPanelHost
{
public:
    explicit VimKeyframeHost(ActionRegistry* actions);

    /// The panel and view the keys work on.
    [[nodiscard]] KeyframesPanel* panel() const { return m_panel; }
    [[nodiscard]] KeyframeViewBase* view() const;
    void focusChanged(QWidget* focus) override;
    bool hostCommand(const QString& name, int count, bool hasCount) override;
    /// Shows or hides the cursor (Vim mode on / off).
    void setCursorVisible(bool visible);

private:
    void reveal();

    QPointer<KeyframesPanel> m_panel;
};

} // namespace kk
