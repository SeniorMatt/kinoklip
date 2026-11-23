// SPDX-License-Identifier: GPL-3.0-or-later
// RelinkMatcher — fuzzy relinking of media items to the files of a folder (Media Pool >
// Relink): a pure planner, the dialog shows its result as a preview table before anything
// changes. A file is found by, in this order of quality:
//   exact name > same name in another case > same name with another extension >
//   a find / replace pattern applied to the old name > the closest name.
// Every file is used for at most one item (the best match claims it).
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace kk::relink {

/// How a new file was found for an item.
enum class MatchKind {
    None, ///< no file found
    Exact, ///< the same file name
    CaseInsensitive, ///< the same name in another case ("CLIP.MOV")
    OtherExtension, ///< the same name without its extension ("clip.mp4" for "clip.mov")
    Pattern, ///< the find / replace pattern turned the old name into the file's name
    Closest, ///< the most similar name (similarity in MatchRow::quality)
    Manual ///< a file the user chose for the item (set by the dialog, never by plan())
};

/// What to match and how.
struct Options
{
    /// Compare names without their extension (a "clip.mov" is found as "clip.mp4").
    bool ignoreExtension = true;
    /// Compare names without regard to case.
    bool ignoreCase = true;
    /// Find a file whose name is merely similar (typo, other numbering, a suffix).
    bool closestName = false;
    /// Closest name: smallest similarity (0..1) that counts as a match.
    double minimumSimilarity = 0.7;
    /// Regular expression (QRegularExpression syntax) found in the old name without extension and
    /// replaced by @c replace ("\1" or "$1" for groups): the result is the name to look for.
    /// Empty = no pattern. An invalid expression is reported by patternError().
    QString find;
    QString replace;
};

/// An item to relink.
struct Source
{
    QString itemId;
    QString oldPath; ///< the path now (offline, or online when relinking everything)
    bool hasVideo = false; ///< the media has a picture: audio-only files are no match
};

/// The plan for one item.
struct MatchRow
{
    QString itemId;
    QString oldPath;
    QString newPath; ///< empty = no match
    MatchKind kind = MatchKind::None;
    double quality = 0.0; ///< 0..1: 1 = the same name; for Closest the name similarity
    /// More than one file of the same quality was possible (the first, by path, was taken).
    bool ambiguous = false;
};

/// Error text of the find expression (empty: valid or empty).
[[nodiscard]] QString patternError(const QString& find);

/// File name without its last extension ("a.b.mov" -> "a.b"; a name starting with a dot has none).
[[nodiscard]] QString baseName(const QString& fileName);

/// Similarity of two names, 0..1: 1 - edit distance / longer length, on the case-folded
/// names; a name that contains the other, or differs only in separators or numbering around
/// a shared stem, scores at least as high as plain edit distance.
/// @p floor: a similarity below it may be reported as any smaller value (lets the search stop
/// early); 0 gives the exact value.
[[nodiscard]] double nameSimilarity(const QString& a, const QString& b, double floor = 0.0);

/// Plans the relink of @p sources to @p files (absolute paths of the folder, in the order to
/// prefer: shallow first). One row per source, in input order.
[[nodiscard]] QVector<MatchRow> plan(
    const QVector<Source>& sources, const QStringList& files, const Options& options);

/// Short name of a match kind for the preview table ("Exact name", "Other extension", ...).
[[nodiscard]] QString kindText(MatchKind kind);

/// Files of @p folder (with subfolders when @p subfolders) that look like media (supported
/// extensions), shallowest first then by path; at most @p limit files.
[[nodiscard]] QStringList scanFolder(const QString& folder, bool subfolders, int limit = 200000);

} // namespace kk::relink
