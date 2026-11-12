// SPDX-License-Identifier: GPL-3.0-or-later
// TitlePresets — descriptor ids of the Effects library's titles (Text, the lower thirds,
// Scroll, Text Box). Every preset is a Title clip with the same parameters (core/ParamKeys.h,
// "Title"); a preset only gives them other defaults. A Title clip remembers its preset in
// ClipData::generator, so the Inspector's Title tab names it and resets to its defaults.
#pragma once

#include <QString>

namespace kk {

struct ClipData;
struct EffectDescriptor;

namespace titles {

// Ids are stored in project files (ClipData::generator of Title clips): never change them.
inline const QString Text = QStringLiteral("text_title"); ///< also: titles without a preset
inline const QString LeftLowerThird = QStringLiteral("title_lower_third_left");
inline const QString MiddleLowerThird = QStringLiteral("title_lower_third_middle");
inline const QString RightLowerThird = QStringLiteral("title_lower_third_right");
inline const QString Scroll = QStringLiteral("title_scroll");
inline const QString Typewriter = QStringLiteral("title_typewriter");
inline const QString TextBox = QStringLiteral("title_text_box");

/// The title descriptor whose parameters @p clip shows (a Title clip): its preset, else Text.
[[nodiscard]] const EffectDescriptor* descriptorOf(const ClipData& clip);

} // namespace titles
} // namespace kk
