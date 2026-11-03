// SPDX-License-Identifier: GPL-3.0-or-later
// GuideIndex — the user guide's article list (docs/guide/index.json) and context resolution.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace kk {

/// One article of the user guide, as listed in docs/guide/index.json.
struct GuideArticle
{
    QString id; ///< "remove-a-green-screen"
    QString title; ///< "Remove a green screen"
    QString file; ///< path relative to the guide's base URL ("remove-a-green-screen.md")
    QString summary;
    QStringList keywords;
    QStringList contexts; ///< context keys: "effect:chroma_key", "panel:inspector", "page:edit"
};

/// The parsed guide index (format: the guide contract, `formatVersion` 1).
///
/// Parsing is defensive: bad JSON or an unknown format version gives an invalid index without
/// articles, so the app shows no "?" buttons while startUrl() still points at the guide's
/// start page under the built-in base URL. Articles without an id, title or a plain relative
/// file are skipped; a base that is not an http(s) URL falls back to the built-in one.
///
/// Context keys are `effect:<EffectRegistry id>`, `action:<ActionRegistry id>`,
/// `panel:<panel key>` and `page:<page id>`. resolve() is pure: the most specific kind wins
/// (effect > action > panel > page); within a kind the caller's order decides; an article
/// listed earlier in the index wins a key it shares with a later one.
class GuideIndex
{
public:
    /// The format version this build understands.
    static constexpr int kFormatVersion = 1;
    /// Where the guide lives when the index does not say (or cannot be read).
    static constexpr char kDefaultBase[]
        = "https://github.com/SeniorMatt/kinoklip/blob/main/docs/guide/";
    static constexpr char kDefaultStart[] = "readme.md";

    /// An empty, invalid index (start page only).
    GuideIndex() = default;

    /// Parses index JSON. Never fails: see the class comment. @p error (optional) receives
    /// why the index is invalid, or why entries were skipped.
    [[nodiscard]] static GuideIndex parse(const QByteArray& json, QString* error = nullptr);
    /// Reads and parses @p path (a file or a Qt resource such as ":/guide/index.json").
    [[nodiscard]] static GuideIndex load(const QString& path, QString* error = nullptr);

    /// True if the JSON was read and its format version is understood.
    [[nodiscard]] bool isValid() const { return m_valid; }
    /// The articles in index order.
    [[nodiscard]] const QVector<GuideArticle>& articles() const { return m_articles; }
    /// The article with id @p id, or nullptr.
    [[nodiscard]] const GuideArticle* article(const QString& id) const;
    /// The first article listing context @p key, or nullptr.
    [[nodiscard]] const GuideArticle* articleFor(const QString& key) const;
    /// The best article for the contexts the user is in (any order), or nullptr when none
    /// matches (the caller then opens the start page).
    [[nodiscard]] const GuideArticle* resolve(const QStringList& contexts) const;
    /// Articles whose title, summary or keywords contain every word of @p text
    /// (case-insensitive); all articles for an empty text.
    [[nodiscard]] QVector<const GuideArticle*> search(const QString& text) const;

    /// The base URL every file is relative to.
    [[nodiscard]] QString base() const { return m_base; }
    /// The guide's start page.
    [[nodiscard]] QUrl startUrl() const;
    /// The public URL of @p article.
    [[nodiscard]] QUrl url(const GuideArticle& article) const;

    /// Rank of a context key's kind: 0 effect, 1 action, 2 panel, 3 page, 4 anything else.
    [[nodiscard]] static int contextRank(const QString& key);

private:
    bool m_valid = false;
    QString m_base = QLatin1String(kDefaultBase);
    QString m_start = QLatin1String(kDefaultStart);
    QVector<GuideArticle> m_articles;
};

} // namespace kk
