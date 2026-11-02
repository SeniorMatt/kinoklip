// SPDX-License-Identifier: GPL-3.0-or-later
// TimecodeEntry — typing a timecode with the timeline focused (spec 2.10): digits fill the
// timecode from the right, Enter jumps there; a leading + / - makes it an offset.
#pragma once

#include "core/Rational.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>

class QKeyEvent;

namespace kk {

/// A typed timecode: an absolute position or an offset, in frames.
struct TypedTimecode
{
    bool relative = false; ///< typed with a leading '+' or '-'
    int64_t frames = 0; ///< absolute frame, or the signed offset
};

namespace timecode {

/// Parses what was typed (spec 2.10). The digits fill "HH:MM:SS:FF" from the right like a
/// calculator ("1" = frame 1, "1000" = 00:00:10:00, "1.." = 00:01:00:00: each '.' adds "00");
/// ':' is ignored; fields may exceed their range ("+90" = 90 frames). A leading '+' / '-'
/// makes it an offset. Returns std::nullopt for text without digits or with other characters.
[[nodiscard]] std::optional<TypedTimecode> parseTyped(const QString& typed, Rational fps);

/// Display form of a partly typed entry: the digits right-aligned into "HH:MM:SS:FF", with
/// the sign of an offset in front ("+00:00:01:05").
[[nodiscard]] QString typedDisplay(const QString& typed);

} // namespace timecode

/// Collects a typed timecode while one of the widgets accepted by the predicate has focus.
///
/// Installed as an application event filter: digits (and '+' / '-' to start an offset) are
/// taken before any shortcut (ShortcutOverride accepted, so the number keys do not also cut
/// multicam angles), '.' and ':' while an entry is open, Backspace removes the last
/// character, Enter / Return applies it, Esc or any other key cancels it (the other key then
/// works as usual). Shortcut-free design: the keys are typed text, not commands, so they are
/// not in the ActionRegistry.
///
/// Thread-safety: GUI thread only.
class TimecodeEntry : public QObject
{
    Q_OBJECT
public:
    /// Is typing accepted now (focus in the timeline, not in a text field, no multicam keys)?
    using Predicate = std::function<bool()>;
    /// Frame rate for parsing.
    using FpsSource = std::function<Rational()>;

    TimecodeEntry(Predicate accepts, FpsSource fps, QObject* parent = nullptr);
    ~TimecodeEntry() override;

    /// True while an entry is being typed.
    [[nodiscard]] bool isActive() const { return !m_typed.isEmpty(); }
    /// The raw characters typed so far.
    [[nodiscard]] const QString& typed() const { return m_typed; }
    /// Cancels the entry being typed (no signal but displayChanged("")).
    void cancel();

signals:
    /// The text to show in the timecode display; empty = show the normal timecode again.
    void displayChanged(const QString& text);
    /// Enter was pressed on a valid entry.
    void entered(const kk::TypedTimecode& entry);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Whether @p key belongs to the entry (typed text, or editing an open entry).
    [[nodiscard]] bool wants(const QKeyEvent* key) const;
    void handle(const QKeyEvent* key);

    Predicate m_accepts;
    FpsSource m_fps;
    QString m_typed;
};

} // namespace kk
