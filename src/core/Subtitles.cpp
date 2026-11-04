// SPDX-License-Identifier: GPL-3.0-or-later
// Subtitles — SRT / WebVTT reader and writer, caption style defaults, cue <-> clip conversion.
#include "core/Subtitles.h"

#include "core/ParamKeys.h"
#include "media/MediaSource.h"
#include "pack/EmbeddedMedia.h"

#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace kk::subtitles {

namespace {

/// Parses "[H+:]MM:SS[,.]f+" starting at @p pos of @p line; advances @p pos past it.
/// Returns -1 when the text is not a timestamp.
int64_t parseTimestamp(QStringView line, qsizetype& pos)
{
    qsizetype i = pos;
    const qsizetype n = line.size();
    int64_t fields[3] = {0, 0, 0};
    int count = 0;
    while (true) {
        if (i >= n || !line[i].isDigit())
            return -1;
        int64_t v = 0;
        int digits = 0;
        while (i < n && line[i].isDigit()) {
            v = v * 10 + line[i].digitValue();
            ++i;
            ++digits;
        }
        if (count == 3 || digits > 9)
            return -1;
        fields[count++] = v;
        if (i < n && line[i] == QLatin1Char(':')) {
            ++i;
            continue;
        }
        break;
    }
    if (count < 2)
        return -1;
    // Fraction: ',' (SRT) or '.' (WebVTT, and many SRT files); 1-3+ digits = milliseconds.
    int64_t ms = 0;
    if (i < n && (line[i] == QLatin1Char(',') || line[i] == QLatin1Char('.'))) {
        ++i;
        int digits = 0;
        while (i < n && line[i].isDigit()) {
            if (digits < 3)
                ms = ms * 10 + line[i].digitValue();
            ++digits;
            ++i;
        }
        if (digits == 0)
            return -1;
        for (int d = digits; d < 3; ++d)
            ms *= 10;
    }
    const int64_t h = count == 3 ? fields[0] : 0;
    const int64_t m = count == 3 ? fields[1] : fields[0];
    const int64_t s = count == 3 ? fields[2] : fields[1];
    if (m > 59 || s > 59)
        return -1;
    pos = i;
    return ((h * 60 + m) * 60 + s) * 1000000 + ms * 1000;
}

/// Parses a timing line "start --> end [settings]". False if it is not one.
bool parseTiming(QStringView line, int64_t* start, int64_t* end)
{
    const qsizetype arrow = line.indexOf(QLatin1String("-->"));
    if (arrow < 0)
        return false;
    QStringView left = line.left(arrow).trimmed();
    QStringView right = line.mid(arrow + 3).trimmed();
    qsizetype p = 0;
    *start = parseTimestamp(left, p);
    if (*start < 0 || p != left.size())
        return false;
    p = 0;
    *end = parseTimestamp(right, p); // cue settings may follow (WebVTT "line:90%" etc.)
    return *end >= 0;
}

/// Removes markup: HTML-like tags (<i>, </font>, <c.yellow>, <00:01.000> karaoke stamps) and
/// ASS override blocks ({\an8}); decodes the common entities (WebVTT requires &amp; &lt; &gt;).
QString plainText(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('<')) {
            const qsizetype close = text.indexOf(QLatin1Char('>'), i + 1);
            // Only things that look like tags: a letter, '/', or a digit (timestamp tag).
            if (close > i && i + 1 < text.size()
                && (text[i + 1].isLetter() || text[i + 1] == QLatin1Char('/')
                    || text[i + 1].isDigit())) {
                i = close;
                continue;
            }
        } else if (c == QLatin1Char('{') && i + 1 < text.size()
            && text[i + 1] == QLatin1Char('\\')) {
            const qsizetype close = text.indexOf(QLatin1Char('}'), i + 1);
            if (close > i) {
                i = close;
                continue;
            }
        } else if (c == QLatin1Char('&')) {
            static const struct
            {
                const char* name;
                char16_t value;
            } kEntities[] = {{"&amp;", u'&'}, {"&lt;", u'<'}, {"&gt;", u'>'}, {"&nbsp;", u' '},
                {"&lrm;", u'‎'}, {"&rlm;", u'‏'}, {"&quot;", u'"'}, {"&apos;", u'\''}};
            bool matched = false;
            for (const auto& e : kEntities) {
                const QLatin1String name(e.name);
                if (QStringView(text).mid(i).startsWith(name)) {
                    out.append(QChar(e.value));
                    i += name.size() - 1;
                    matched = true;
                    break;
                }
            }
            if (matched)
                continue;
        }
        out.append(c);
    }
    return out;
}

QString escapeVtt(const QString& text)
{
    QString out = text;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return out;
}

/// A caption text never contains a blank line (it would end the cue in both formats).
QString compactLines(const QString& text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    lines.removeIf([](const QString& l) { return l.trimmed().isEmpty(); });
    return lines.join(QLatin1Char('\n'));
}

/// Subtitle files come in UTF-8 (with or without BOM), UTF-16 (Windows tools; BOM or not) and
/// legacy 8-bit code pages. Text that is not valid UTF-8 is read as Windows-1252 (Latin-1 when
/// the codec is unavailable) instead of turning every accented letter into U+FFFD.
QString decodeSubtitleText(const QByteArray& data)
{
    const auto utf16 = [&](QStringDecoder::Encoding encoding, qsizetype skip) {
        QStringDecoder decoder(encoding);
        return QString(decoder(QByteArrayView(data).sliced(skip)));
    };
    if (data.startsWith("\xFF\xFE"))
        return utf16(QStringDecoder::Utf16LE, 2);
    if (data.startsWith("\xFE\xFF"))
        return utf16(QStringDecoder::Utf16BE, 2);
    if (data.size() >= 2 && data[0] != 0 && data[1] == 0)
        return utf16(QStringDecoder::Utf16LE, 0);
    if (data.size() >= 2 && data[0] == 0 && data[1] != 0)
        return utf16(QStringDecoder::Utf16BE, 0);
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8(data);
    if (!utf8.hasError())
        return text;
    QStringDecoder cp1252("windows-1252");
    if (cp1252.isValid()) {
        text = cp1252(data);
        if (!cp1252.hasError())
            return text;
    }
    return QString::fromLatin1(data);
}

int64_t usToNearestFrame(int64_t us, Rational fps)
{
    return fps.isValid() ? int64_t(std::llround(double(us) * fps.num / (1e6 * fps.den))) : 0;
}

} // namespace

const ParamSet& defaultStyle()
{
    static const ParamSet style = [] {
        ParamSet p;
        p.setStatic(keys::Font, QStringLiteral("Sans Serif"));
        p.setStatic(keys::Bold, false);
        p.setStatic(keys::Italic, false);
        p.setStatic(keys::Align, QStringLiteral("center"));
        p.setStatic(keys::Color, QColor(Qt::white));
        p.setStatic(keys::OutlineColor, QColor(Qt::black));
        p.setStatic(keys::BgEnabled, false);
        p.setStatic(keys::BgColor, QColor(Qt::black));
        p.setStatic(keys::Shadow, true);
        p.setStatic(Position, QStringLiteral("bottom"));
        p.setBase(keys::Size, kDefaultSize);
        p.setBase(keys::OutlineWidth, kDefaultOutlineWidth);
        p.setBase(keys::BgOpacity, kDefaultBgOpacity);
        p.setBase(keys::LineSpacing, 100.0);
        p.setBase(keys::Tracking, 0.0);
        p.setBase(MarginV, kDefaultMarginV);
        p.setBase(MarginH, kDefaultMarginH);
        return p;
    }();
    return style;
}

double styleValue(
    const ClipData& caption, const TrackData* track, const QString& key, int64_t localFrame)
{
    if (hasOwnStyle(caption)) {
        if (const auto it = caption.title.values.constFind(key);
            it != caption.title.values.constEnd())
            return it->valueAt(localFrame);
    }
    if (track) {
        // Track styles are not animated per caption: evaluated at frame 0.
        if (const auto it = track->subtitleStyle.values.constFind(key);
            it != track->subtitleStyle.values.constEnd())
            return it->valueAt(0);
    }
    return defaultStyle().value(key, 0);
}

QVariant styleStatic(const ClipData& caption, const TrackData* track, const QString& key)
{
    if (hasOwnStyle(caption)) {
        if (const auto it = caption.title.statics.constFind(key);
            it != caption.title.statics.constEnd())
            return *it;
    }
    if (track) {
        if (const auto it = track->subtitleStyle.statics.constFind(key);
            it != track->subtitleStyle.statics.constEnd())
            return *it;
    }
    return defaultStyle().staticValue(key);
}

bool hasOwnStyle(const ClipData& caption)
{
    const auto it = caption.title.statics.constFind(OwnStyle);
    return it != caption.title.statics.constEnd() && it->toBool();
}

QString captionText(const ClipData& caption)
{
    return caption.title.staticValue(keys::Text).toString();
}

ClipData makeCaption(const QString& text, int64_t startFrame, int64_t frames)
{
    ClipData c;
    c.kind = ClipKind::Subtitle;
    c.start = startFrame;
    c.duration = std::max(kMinCaptionFrames, frames);
    c.title.setStatic(keys::Text, text);
    c.name = text.section(QLatin1Char('\n'), 0, 0);
    return c;
}

TrackData makeSubtitleTrack()
{
    TrackData t;
    t.type = TrackType::Subtitle;
    t.subtitleStyle = defaultStyle();
    return t;
}

// ---- Files ----

bool parse(const QByteArray& data, QVector<SubtitleCue>* cues, QString* error,
    SubtitleFormat* detected, int* emptyCues)
{
    cues->clear();
    int empty = 0;
    QString text = decodeSubtitleText(data);
    if (text.startsWith(QChar(0xfeff)))
        text.remove(0, 1);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = text.split(QLatin1Char('\n'));
    const bool vtt = !lines.isEmpty() && lines.front().startsWith(QLatin1String("WEBVTT"));
    if (detected)
        *detected = vtt ? SubtitleFormat::WebVtt : SubtitleFormat::Srt;

    // Block-based: a cue is a timing line, the text lines after it up to a blank line. The
    // optional identifier / index line before the timing line is ignored. WebVTT NOTE / STYLE
    // / REGION blocks have no timing line and are skipped with their block.
    qsizetype i = vtt ? 1 : 0;
    const qsizetype n = lines.size();
    while (i < n) {
        // Skip to the next non-empty line (block start).
        while (i < n && lines[i].trimmed().isEmpty())
            ++i;
        if (i >= n)
            break;
        int64_t start = 0, end = 0;
        qsizetype t = i;
        if (!parseTiming(lines[t], &start, &end) && t + 1 < n
            && parseTiming(lines[t + 1], &start, &end))
            ++t; // identifier / index line
        else if (!parseTiming(lines[t], &start, &end)) {
            while (i < n && !lines[i].trimmed().isEmpty()) // not a cue: skip the block
                ++i;
            continue;
        }
        QStringList textLines;
        i = t + 1;
        while (i < n && !lines[i].trimmed().isEmpty()) {
            textLines.append(lines[i]);
            ++i;
        }
        if (end <= start)
            continue; // zero-length / reversed cue: nothing to show
        const QString body = plainText(textLines.join(QLatin1Char('\n'))).trimmed();
        if (body.isEmpty()) {
            ++empty; // an invisible caption: skipped, the caller reports the count
            continue;
        }
        cues->append({start, end, body});
    }
    if (emptyCues)
        *emptyCues = empty;
    if (cues->isEmpty() && empty > 0) {
        if (error)
            *error = QCoreApplication::translate(
                "kk::subtitles", "Every subtitle cue in the file is empty.");
        return false;
    }
    if (cues->isEmpty()) {
        if (error)
            *error = QCoreApplication::translate(
                "kk::subtitles", "No subtitle cues found (expected SubRip or WebVTT).");
        return false;
    }
    return true;
}

bool readFile(const QString& path, QVector<SubtitleCue>* cues, QString* error, int* emptyCues)
{
    // Through MediaSource: a subtitle file stored inside a single-file project is read in place.
    if (pack::mayBeEmbeddedPath(path)) {
        const media::MediaSource source = media::MediaSource::resolve(path);
        if (source.isEmbedded()) {
            QString why;
            const QByteArray bytes = source.readAll(&why, 64LL << 20);
            if (bytes.isNull()) {
                if (error)
                    *error = why;
                return false;
            }
            return parse(bytes, cues, error, nullptr, emptyCues);
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return parse(file.readAll(), cues, error, nullptr, emptyCues);
}

QString formatTimestamp(int64_t us, SubtitleFormat format)
{
    const int64_t ms = std::max<int64_t>(0, (us + 500) / 1000);
    const int64_t h = ms / 3600000;
    const int64_t m = ms / 60000 % 60;
    const int64_t s = ms / 1000 % 60;
    const QChar sep = format == SubtitleFormat::Srt ? QLatin1Char(',') : QLatin1Char('.');
    return QStringLiteral("%1:%2:%3%4%5")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(sep)
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QByteArray write(const QVector<SubtitleCue>& cues, SubtitleFormat format)
{
    QString out;
    if (format == SubtitleFormat::WebVtt)
        out += QLatin1String("WEBVTT\n\n");
    int index = 1;
    for (const SubtitleCue& c : cues) {
        const QString body = compactLines(c.text);
        if (body.isEmpty())
            continue;
        if (format == SubtitleFormat::Srt)
            out += QString::number(index++) + QLatin1Char('\n');
        out += formatTimestamp(c.startUs, format) + QLatin1String(" --> ")
            + formatTimestamp(c.endUs, format) + QLatin1Char('\n');
        out += (format == SubtitleFormat::WebVtt ? escapeVtt(body) : body) + QLatin1String("\n\n");
    }
    return out.toUtf8();
}

bool writeFile(
    const QString& path, const QVector<SubtitleCue>& cues, SubtitleFormat format, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(write(cues, format)) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

QString suffix(SubtitleFormat format)
{
    return format == SubtitleFormat::Srt ? QStringLiteral("srt") : QStringLiteral("vtt");
}

bool isSubtitleFile(const QString& path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    return s == QLatin1String("srt") || s == QLatin1String("vtt");
}

// ---- Timeline conversion ----

QVector<SubtitleCue> cuesFromTrack(
    const TrackData& track, Rational fps, int64_t rangeStart, int64_t rangeEnd)
{
    QVector<SubtitleCue> cues;
    for (const ClipData& c : track.clips) {
        if (!c.enabled || c.kind != ClipKind::Subtitle)
            continue;
        const int64_t s = std::max(c.start, rangeStart);
        const int64_t e = std::min(c.end(), rangeEnd);
        if (e <= s)
            continue;
        const QString text = captionText(c).trimmed();
        if (text.isEmpty())
            continue;
        cues.append({framesToUs(s - rangeStart, fps), framesToUs(e - rangeStart, fps), text});
    }
    return cues;
}

QVector<PlacedCaption> captionsFromCues(
    const QVector<SubtitleCue>& cues, Rational fps, int64_t startFrame)
{
    // Sort by start (stable: equal starts keep file order), then fill lanes greedily.
    QVector<const SubtitleCue*> order;
    order.reserve(cues.size());
    for (const SubtitleCue& c : cues)
        order.append(&c);
    std::stable_sort(order.begin(), order.end(),
        [](const SubtitleCue* a, const SubtitleCue* b) { return a->startUs < b->startUs; });
    QVector<int64_t> laneEnd; // first free frame per lane
    QVector<PlacedCaption> out;
    out.reserve(cues.size());
    for (const SubtitleCue* c : order) {
        const int64_t s = startFrame + usToNearestFrame(c->startUs, fps);
        const int64_t e
            = std::max(s + kMinCaptionFrames, startFrame + usToNearestFrame(c->endUs, fps));
        int lane = 0;
        while (lane < laneEnd.size() && laneEnd[lane] > s)
            ++lane;
        if (lane == laneEnd.size())
            laneEnd.append(e);
        else
            laneEnd[lane] = e;
        out.append({lane, makeCaption(c->text, s, e - s)});
    }
    return out;
}

int64_t cuesDurationUs(const QVector<SubtitleCue>& cues)
{
    int64_t end = 0;
    for (const SubtitleCue& c : cues)
        end = std::max(end, c.endUs);
    return end;
}

} // namespace kk::subtitles
