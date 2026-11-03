// SPDX-License-Identifier: GPL-3.0-or-later
// GuideIndex implementation: defensive JSON parsing, context resolution, search.
#include "ui/help/GuideIndex.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>

namespace kk {

namespace {

/// A file name relative to the base: no scheme, no absolute path, no "..".
bool isPlainRelative(const QString& file)
{
    if (file.isEmpty() || file.startsWith(QLatin1Char('/')) || file.contains(QLatin1Char(':'))
        || file.contains(QLatin1Char('\\')))
        return false;
    const QStringList parts = file.section(QLatin1Char('#'), 0, 0).split(QLatin1Char('/'));
    return std::none_of(parts.begin(), parts.end(),
        [](const QString& part) { return part == QLatin1String(".."); });
}

QStringList stringList(const QJsonValue& value)
{
    QStringList out;
    for (const QJsonValue& item : value.toArray()) {
        if (item.isString() && !item.toString().trimmed().isEmpty())
            out.append(item.toString().trimmed());
    }
    return out;
}

void note(QString* error, const QString& text)
{
    if (!error)
        return;
    if (!error->isEmpty())
        error->append(QLatin1String("; "));
    error->append(text);
}

} // namespace

GuideIndex GuideIndex::parse(const QByteArray& json, QString* error)
{
    GuideIndex index;
    if (error)
        error->clear();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        note(error,
            parseError.error != QJsonParseError::NoError ? parseError.errorString()
                                                         : QStringLiteral("not a JSON object"));
        return index;
    }
    const QJsonObject root = doc.object();
    const QJsonValue version = root.value(QLatin1String("formatVersion"));
    if (!version.isDouble() || version.toInt() != kFormatVersion) {
        note(error, QStringLiteral("unknown formatVersion"));
        return index;
    }

    const QString base = root.value(QLatin1String("base")).toString().trimmed();
    const QUrl baseUrl(base, QUrl::StrictMode);
    if (baseUrl.isValid()
        && (baseUrl.scheme() == QLatin1String("https")
            || baseUrl.scheme() == QLatin1String("http"))) {
        index.m_base = base.endsWith(QLatin1Char('/')) ? base : base + QLatin1Char('/');
    } else if (!base.isEmpty()) {
        note(error, QStringLiteral("base is not an http(s) URL"));
    }
    const QString start = root.value(QLatin1String("start")).toString().trimmed();
    if (isPlainRelative(start))
        index.m_start = start;

    const QJsonArray articles = root.value(QLatin1String("articles")).toArray();
    for (const QJsonValue& value : articles) {
        const QJsonObject o = value.toObject();
        GuideArticle a;
        a.id = o.value(QLatin1String("id")).toString().trimmed();
        a.title = o.value(QLatin1String("title")).toString().trimmed();
        a.file = o.value(QLatin1String("file")).toString().trimmed();
        a.summary = o.value(QLatin1String("summary")).toString().trimmed();
        a.keywords = stringList(o.value(QLatin1String("keywords")));
        a.contexts = stringList(o.value(QLatin1String("contexts")));
        if (a.id.isEmpty() || a.title.isEmpty() || !isPlainRelative(a.file)
            || index.article(a.id)) {
            note(error, QStringLiteral("skipped article \"%1\"").arg(a.id));
            continue;
        }
        index.m_articles.append(a);
    }
    index.m_valid = true;
    return index;
}

GuideIndex GuideIndex::load(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    return parse(file.readAll(), error);
}

const GuideArticle* GuideIndex::article(const QString& id) const
{
    for (const GuideArticle& a : m_articles) {
        if (a.id == id)
            return &a;
    }
    return nullptr;
}

const GuideArticle* GuideIndex::articleFor(const QString& key) const
{
    if (key.isEmpty())
        return nullptr;
    for (const GuideArticle& a : m_articles) {
        if (a.contexts.contains(key))
            return &a;
    }
    return nullptr;
}

int GuideIndex::contextRank(const QString& key)
{
    static const char* const kinds[] = {"effect:", "action:", "panel:", "page:"};
    for (int i = 0; i < 4; ++i) {
        if (key.startsWith(QLatin1String(kinds[i])))
            return i;
    }
    return 4;
}

const GuideArticle* GuideIndex::resolve(const QStringList& contexts) const
{
    QStringList ordered = contexts;
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const QString& a, const QString& b) { return contextRank(a) < contextRank(b); });
    for (const QString& key : std::as_const(ordered)) {
        if (const GuideArticle* a = articleFor(key))
            return a;
    }
    return nullptr;
}

QVector<const GuideArticle*> GuideIndex::search(const QString& text) const
{
    const QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QVector<const GuideArticle*> out;
    for (const GuideArticle& a : m_articles) {
        const QString haystack = a.title + QLatin1Char('\n') + a.summary + QLatin1Char('\n')
            + a.keywords.join(QLatin1Char('\n'));
        const bool match = std::all_of(words.begin(), words.end(),
            [&](const QString& word) { return haystack.contains(word, Qt::CaseInsensitive); });
        if (match)
            out.append(&a);
    }
    return out;
}

QUrl GuideIndex::startUrl() const
{
    return QUrl(m_base + m_start);
}

QUrl GuideIndex::url(const GuideArticle& article) const
{
    return QUrl(m_base + article.file);
}

} // namespace kk
