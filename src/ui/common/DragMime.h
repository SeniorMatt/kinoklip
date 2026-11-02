// SPDX-License-Identifier: GPL-3.0-or-later
// DragMime — MIME types shared by every drag source / drop target in the UI.
// Keeping them in one header is the contract between panels (media pool, effects library,
// source viewer) and the timeline, which are written independently.
#pragma once

namespace kk::mime {

/// Media pool items. Payload: UTF-8, newline-separated MediaItem ids.
inline constexpr char MediaItems[] = "application/x-kinoklip-media-ids";

/// A ranged source clip dragged from the source viewer.
/// Payload: UTF-8 JSON {"mediaId": str, "inUs": int64, "outUs": int64} (out exclusive).
inline constexpr char SourceRange[] = "application/x-kinoklip-source-range";

/// An effect, transition or generator from the effects library.
/// Payload: UTF-8 EffectRegistry descriptor id (e.g. "gaussian_blur", "cross_dissolve",
/// "text_title").
inline constexpr char Effect[] = "application/x-kinoklip-effect";

/// Clips of one timeline dragged onto another (stacked timelines; always a copy).
/// Payload: UTF-8 JSON {"timeline": uid, "clips": [clip ids], "grabFrames": int64} where
/// grabFrames is the distance from the earliest dragged clip's start to the grab point.
inline constexpr char TimelineClips[] = "application/x-kinoklip-timeline-clips";

} // namespace kk::mime
