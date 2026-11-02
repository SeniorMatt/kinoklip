// SPDX-License-Identifier: GPL-3.0-or-later
// AudioScales — pure mappings between dB values and positions on the fader, the level meters
// and the EQ curve (log frequency), shared by the widgets and their tests.
#pragma once

namespace kk::audioscale {

/// Fader travel position (0 = bottom = -inf, 1 = top = +12 dB) of @p db. Piecewise linear in
/// dB like a console fader: the 0 dB mark sits at about three quarters, the range below
/// -40 dB is compressed.
[[nodiscard]] double faderPosition(double db);
/// Inverse of faderPosition() (clamped to 0 ... 1); 0 gives kFaderMinDb (silence).
[[nodiscard]] double faderDb(double position);

/// Lowest level shown by the meters (dBFS); everything below draws empty.
inline constexpr double kMeterFloorDb = -60.0;
/// Meter colour zones: below kMeterNeutralDb the scheme's positive colour (green), up to
/// kMeterNegativeDb the neutral one (yellow), above it the negative one (red).
inline constexpr double kMeterNeutralDb = -18.0;
inline constexpr double kMeterNegativeDb = -6.0;
/// Meter fill (0 ... 1) of @p db: linear in dB from kMeterFloorDb to 0 dBFS.
[[nodiscard]] double meterPosition(double db);

/// Linear amplitude to dB (silence: a very low value, not -inf).
[[nodiscard]] double toDb(double linear);

/// EQ curve x (0 ... 1) of @p hz on a log axis from 20 Hz to 20 kHz.
[[nodiscard]] double frequencyPosition(double hz);
/// Inverse of frequencyPosition().
[[nodiscard]] double positionFrequency(double x);

} // namespace kk::audioscale
