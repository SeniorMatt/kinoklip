// SPDX-License-Identifier: GPL-3.0-or-later
// ParamRows — one editor row per descriptor parameter (number, bool, colour, choice, text).
// Rows place their widgets into a shared QGridLayout so all labels/fields line up.
#pragma once

#include "ui/inspector/ParamAccess.h"

#include <QObject>

#include <cstdint>

class QGridLayout;
class QLabel;
class QToolButton;
class QWidget;

namespace kk {

class ParamEditHost;

/// Grid columns used by every row (see ParamRow::create()).
namespace paramgrid {
inline constexpr int kLabel = 0; ///< right-aligned parameter name
inline constexpr int kEditor = 1; ///< value field / checkbox / combo / swatch / text
inline constexpr int kSlider = 2; ///< optional slider (number rows); others span into it
inline constexpr int kKeyframe = 3; ///< prev / diamond / next (animatable number rows)
inline constexpr int kReset = 4; ///< reset-to-default button
inline constexpr int kColumnCount = 5;
} // namespace paramgrid

/// Editor of one parameter. Created once per structure (clip, effect list) and then only
/// refreshed: refresh() must be cheap (O(1) per row, no widget creation, no allocation when
/// nothing changed) because it runs on every playhead move.
///
/// Rows own no model state: they read the clip passed to refresh() and write through the
/// ParamEditHost, which wraps every change in Project::beginEdit()/endEdit().
class ParamRow : public QObject
{
    Q_OBJECT
public:
    /// Creates the matching row type for @p desc and adds its widgets to row @p gridRow of
    /// @p grid (widgets are parented to @p parentWidget). The row object is parented to
    /// @p parentWidget as well, so it dies with the group.
    static ParamRow* create(const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host,
        QGridLayout* grid, int gridRow, QWidget* parentWidget);

    /// Updates the widgets from @p clip at clip-local frame @p localFrame. @p playheadInside is
    /// false when the playhead is outside the clip (keyframe toggling is then disabled).
    virtual void refresh(const ClipData& clip, int64_t localFrame, bool playheadInside) = 0;

    /// Closes an undo group the row keeps open while the user types (title text), so a save
    /// right now includes it as a finished step. Default: nothing open.
    virtual void finishPendingEdit() { }

    /// Address of the parameter.
    [[nodiscard]] const ParamRef& ref() const { return m_ref; }
    /// Descriptor of the parameter (a copy, so synthetic descriptors work too).
    [[nodiscard]] const ParamDescriptor& descriptor() const { return m_desc; }

protected:
    ParamRow(
        const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host, QObject* parent);

    /// Adds the label and the reset button (common to all rows).
    void addCommonWidgets(QGridLayout* grid, int gridRow, QWidget* parentWidget);
    /// Enables the reset button when the value differs from the default.
    void refreshReset(const ClipData& clip, int64_t localFrame);
    /// Label of undo commands changing this parameter ("Change Opacity").
    [[nodiscard]] QString changeLabel() const;

    ParamRef m_ref;
    ParamDescriptor m_desc;
    ParamEditHost& m_host;
    QLabel* m_label = nullptr;
    QToolButton* m_reset = nullptr;
    bool m_resetEnabled = true; ///< cached so refresh() only touches the button on change
};

} // namespace kk
