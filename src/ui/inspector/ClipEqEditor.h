// SPDX-License-Identifier: GPL-3.0-or-later
// ClipEqEditor — the graphical editor of the clip Equalizer (Inspector, Audio tab): the response
// curve of the four bands with a draggable handle each, above the number rows.
#pragma once

#include "core/MixerData.h"
#include "ui/inspector/ParamAccess.h"

#include <QWidget>

namespace kk {

class EqCurveView;
class ParamEditHost;

/// Shows the clip equalizer of one clip as an EqCurveView (the mixer EQ's widget, so the curve
/// comes from the same biquads the audio engine runs, render/Biquad.h) and writes the gestures
/// through the ParamEditHost, like the rows next to it: a drag is one undo step, selected
/// clips of the same kind follow, the parameters are the rows' own (keys::EqBand*).
///
/// Gestures: drag a handle = frequency + gain (Shift fine), wheel / Alt-drag = Q, double-click
/// or Delete = reset the band, arrows = move, Page Up / Down = Q, Tab = next band. Dragging
/// switches the equalizer on.
///
/// Thread-safety: GUI thread only.
class ClipEqEditor : public QWidget
{
    Q_OBJECT
public:
    ClipEqEditor(ParamEditHost& host, quint64 clipId, QWidget* parent = nullptr);

    /// Shows the equalizer of @p clip (no edits).
    void refresh(const ClipData& clip);
    [[nodiscard]] EqCurveView* curve() const { return m_curve; }
    /// The settings shown (tests).
    [[nodiscard]] const EqSettings& settings() const { return m_settings; }

private:
    [[nodiscard]] ParamRef ref(const QString& key) const;
    void applyBand(int band, const double* hz, const double* db, const double* q);
    void resetBand(int band);

    ParamEditHost& m_host;
    quint64 m_clipId;
    EqCurveView* m_curve = nullptr;
    EqSettings m_settings;
    QVector<ParamDescriptor> m_descs; ///< the AudioEq group's descriptors, by key
};

} // namespace kk
