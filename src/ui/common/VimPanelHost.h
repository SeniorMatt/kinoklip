// SPDX-License-Identifier: GPL-3.0-or-later
// VimPanelHost — the part every panel host of the Vim layer has in common (spec section 83):
// the forms (VimFormHost), the keyframe editor, the node graphs and the mixer. These panels are
// not a buffer the grammar can run motions over; their keys are commands of the host
// (`vim:form_*`, `vim:kf_*`, ...) that arrive in hostCommand(). This base answers the rest of
// the VimHost interface: action triggers, the `:` line, messages.
#pragma once

#include "ui/common/VimHost.h"

#include <QString>

#include <functional>

namespace kk {

class ActionRegistry;

class VimPanelHost : public VimHost
{
public:
    explicit VimPanelHost(ActionRegistry* actions);

    bool trigger(const QString& actionId) override;
    [[nodiscard]] int64_t playhead() const override { return 0; }
    void setPlayhead(int64_t frame) override { Q_UNUSED(frame); }
    void clearSelection() override { }
    void openCommandLine() override;
    void openSearch() override;
    void runEx(const QString& line) override;
    void showMessage(const QString& text) override;

    /// Set by the window: `:`, `/`, an ex line bound to a key, a message for the mode line.
    std::function<void()> onCommandLine;
    std::function<void()> onSearch;
    std::function<void(const QString&)> onEx;
    std::function<void(const QString&)> onMessage;

protected:
    ActionRegistry* m_actions;
};

} // namespace kk
