// SPDX-License-Identifier: GPL-3.0-or-later
// BugReport — Help > Report a Bug or Inconsistency: the system information a report may carry
// and the GitHub issue-form link that carries it. Nothing here touches the network: the link
// is opened in the user's browser, where they read the form and submit it themselves.
// What may be collected is a privacy rule (docs/privacy.md): versions and settings, never
// names, paths, host names, files or projects.
#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

namespace kk::bugreport {

/// The issue forms in .github/ISSUE_TEMPLATE (the file names and field ids are a contract
/// with those files; tests/app/test_bugreport.cpp checks both sides).
enum class Kind {
    Bug, ///< bug_report.yml
    ResolveDifference, ///< resolve_difference.yml ("Works differently from DaVinci Resolve")
    Idea, ///< feature_request.yml
};

/// Where the forms live (`<base>/new?template=...`).
inline constexpr char kIssuesUrl[] = "https://github.com/SeniorMatt/kinoklip/issues";
/// Field ids the app fills (`id:` in the forms).
inline constexpr char kVersionField[] = "version";
inline constexpr char kEnvironmentField[] = "environment";
/// GitHub refuses or cuts very long URLs; the link stays below this many characters.
inline constexpr int kMaxUrlLength = 7500;

/// One fact about the system: a label and its value ("Qt", "6.8.2").
using Line = QPair<QString, QString>;

/// What a report can carry. Every value is something about the software and settings, never
/// about the person (see collect()).
struct SystemInfo
{
    QString version; ///< "1.1.2-beta.1"
    QList<Line> lines; ///< in order of importance (the first ones survive shortening)
};

/// Reads the facts of this process and machine, in memory only (nothing is stored or sent):
/// Kinoklip version and commit, package kind (AppImage / Flatpak / source build), distribution
/// (NAME, VERSION_ID of /etc/os-release) and kernel release, desktop and session type, the Qt
/// platform plugin, Qt and FFmpeg library versions, the OpenGL strings and hardware decoding
/// state only if this process already has them, screens, widget style, colour theme, language.
/// Values are scrubbed of the home folder, user name and host name (scrub()).
[[nodiscard]] SystemInfo collect();

/// The template file of @p kind ("bug_report.yml").
[[nodiscard]] QString templateFile(Kind kind);
/// The title prefix of @p kind ("[Bug] "), as in the form's `title:`.
[[nodiscard]] QString titlePrefix(Kind kind);
/// Whether the form of @p kind has the environment field (the Idea form has the version only).
[[nodiscard]] bool hasEnvironment(Kind kind);

/// "Label: value" lines, one per fact, as shared and copied.
[[nodiscard]] QString environmentText(const QList<Line>& lines);

/// Replaces the home folder, user name and host name in @p text (whole words) by
/// "<removed>", so no value can leak them even if a library string contains them.
[[nodiscard]] QString scrub(const QString& text);

/// NAME and VERSION_ID of an os-release file's text ("Linux Mint", "22.1"); empty if absent.
[[nodiscard]] QPair<QString, QString> parseOsRelease(const QByteArray& text);

/// A ready link and exactly what it shares.
struct IssueLink
{
    QUrl url;
    QString version; ///< the version field's value (empty: not shared)
    QString environment; ///< the environment field's value (empty: not shared)
    /// What the user is shown: the version / environment as they go into the form.
    [[nodiscard]] QString sharedText() const;
};

/// The issue-form link for @p kind: `<kIssuesUrl>/new?template=<file>` and, when @p share is
/// true, `title=<prefix>`, `version=` and (if the form has it) `environment=`, every value
/// percent-encoded. Lines are dropped from the end (then the last one is cut) until the URL
/// is at most kMaxUrlLength characters. Without @p share only the template is named.
[[nodiscard]] IssueLink issueLink(Kind kind, const SystemInfo& info, bool share);

} // namespace kk::bugreport
