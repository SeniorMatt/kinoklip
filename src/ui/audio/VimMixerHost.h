// SPDX-License-Identifier: GPL-3.0-or-later
// VimMixerHost — the keys of context "mixer" (spec section 83, 5.9): h and l choose a strip, j and
// k (or Ctrl+X and Ctrl+A) move its fader by half a dB, 0 resets it, m s r mute / solo / arm,
// > and < pan, i types the dB value, Enter opens the EQ. A frame marks the strip. The changes
// are the signals of the strip's own controls, so each one is one undo step as with the mouse.
#pragma once

#include "ui/common/VimPanelHost.h"

#include <QPointer>

namespace kk {

class MixerPanel;
class MixerStrip;
class VimCursorFrame;

class VimMixerHost final : public VimPanelHost
{
public:
    explicit VimMixerHost(ActionRegistry* actions);
    ~VimMixerHost() override;

    /// Fader step in dB.
    static constexpr double kFaderStepDb = 0.5;
    /// Pan step (the knob goes from -1 to 1).
    static constexpr double kPanStep = 0.05;

    void focusChanged(QWidget* focus) override;
    bool hostCommand(const QString& name, int count, bool hasCount) override;
    /// The strip the cursor is on, or nullptr.
    [[nodiscard]] MixerStrip* current() const;
    [[nodiscard]] int index() const { return m_index; }
    void setIndex(int index);
    /// Shows or hides the cursor frame (Vim mode on / off).
    void setCursorVisible(bool visible);

private:
    void show();

    QPointer<MixerPanel> m_panel;
    QPointer<VimCursorFrame> m_frame; ///< a child of the strip it frames
    int m_index = 0;
    bool m_visible = true;
};

} // namespace kk
