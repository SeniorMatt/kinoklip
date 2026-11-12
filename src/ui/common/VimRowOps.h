// SPDX-License-Identifier: GPL-3.0-or-later
// VimRowOps — what a row of a form (an Inspector parameter, a section header) lets the Vim
// layer do with it from the keyboard (spec section 83). The form host (VimFormHost.h) finds
// the rows of a panel through the property kRowProperty of one widget of each row and calls
// these; every method has a harmless default, so a row implements what it can. All changes a
// method makes are one undo step.
#pragma once

#include <QList>
#include <QString>
#include <QVariant>
#include <QWidget>

namespace kk {

class VimRowOps
{
public:
    virtual ~VimRowOps() = default;

    /// Property (QObject*) of one widget of the row that points at the row object.
    static constexpr char kRowProperty[] = "kkVimRow";

    /// Keyframe interpolation presets of `<leader>k{l,i,o,b,s,h}`.
    enum class Ease {
        Linear,
        EaseIn,
        EaseOut,
        EaseInOut,
        Smooth,
        Hold
    };

    /// The name of the row for messages ("Opacity").
    [[nodiscard]] virtual QString vimName() const { return {}; }
    /// The widgets of the row: the cursor frame is drawn around them, and they are not rows
    /// of their own.
    [[nodiscard]] virtual QList<QWidget*> vimWidgets() const = 0;
    /// `<C-a>` / `<C-x>`: @p steps (negative: down) steps of the value, each step the
    /// parameter's own step times @p scale (1 normal, 10 coarse, 0.1 fine); a choice moves by
    /// entries (wrapping round), a checkbox toggles.
    virtual bool vimStep(int steps, double scale)
    {
        Q_UNUSED(steps);
        Q_UNUSED(scale);
        return false;
    }
    /// `dd`: back to the default value (the row's reset button).
    virtual bool vimReset() { return false; }
    /// `i` `<CR>`: starts editing by typing (a text field takes the focus: INSERT).
    virtual bool vimEdit() { return false; }
    /// `<CR>` `x`: the row's own action: toggle a checkbox, open a colour or file dialog or a
    /// choice, press a button, fold a section.
    virtual bool vimActivate() { return false; }
    /// `za` `zo` `zc`: -1 closes, 0 toggles, 1 opens. Only sections fold.
    virtual bool vimFold(int mode)
    {
        Q_UNUSED(mode);
        return false;
    }
    /// A section: `x` switches it on or off.
    virtual bool vimSwitch() { return false; }
    /// A section: `dd` removes it (an effect, a mask, a transition) or resets all of it.
    virtual bool vimRemove() { return false; }
    /// A section: `>>` (+1) and `<<` (-1) move it in its stack.
    virtual bool vimMove(int delta)
    {
        Q_UNUSED(delta);
        return false;
    }
    /// The value for `yy`; invalid when the row has none.
    [[nodiscard]] virtual QVariant vimValue() const { return {}; }
    /// `p`: sets the value.
    virtual bool vimSetValue(const QVariant& value)
    {
        Q_UNUSED(value);
        return false;
    }
    /// `<leader>kk`: adds a keyframe at the playhead, or removes the one there.
    virtual bool vimToggleKeyframe() { return false; }
    /// `]k` `[k`: moves the playhead to the next / previous keyframe of this parameter.
    virtual bool vimJumpKeyframe(bool forward)
    {
        Q_UNUSED(forward);
        return false;
    }
    /// `<leader>kd`: removes the keyframe at the playhead.
    virtual bool vimDeleteKeyframe() { return false; }
    /// `<leader>kl` ...: sets the interpolation of the keyframe at the playhead.
    virtual bool vimSetInterp(Ease interp)
    {
        Q_UNUSED(interp);
        return false;
    }
};

/// Makes @p panel a "form" for the Vim layer (key context "form", VimFormHost.h): a panel of
/// labelled controls. It can take the focus itself, so the row cursor can leave the controls.
inline void markVimForm(QWidget* panel)
{
    panel->setProperty("kkVimContext", QStringLiteral("form"));
    if (panel->focusPolicy() == Qt::NoFocus)
        panel->setFocusPolicy(Qt::StrongFocus);
}

} // namespace kk
