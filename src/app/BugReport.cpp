// SPDX-License-Identifier: GPL-3.0-or-later
// BugReport implementation.
#include "app/BugReport.h"

#include "app/AppInfo.h"
#include "app/Theme.h"
#include "media/HwAccel.h"
#include "render/GlInfo.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QScreen>
#include <QSysInfo>
#include <QUrlQuery>
#include <QtGlobal>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// The commit is set on this file only by CMakeLists.txt (configure time), so a new commit does
// not rebuild the whole library.
#ifndef KK_GIT_COMMIT
#define KK_GIT_COMMIT ""
#endif

namespace kk::bugreport {

namespace {

constexpr char kRemoved[] = "<removed>";

QString libVersion(unsigned v)
{
    return QStringLiteral("%1.%2.%3")
        .arg(AV_VERSION_MAJOR(v))
        .arg(AV_VERSION_MINOR(v))
        .arg(AV_VERSION_MICRO(v));
}

QString packageKind()
{
    // Only whether the variables / files exist: $APPIMAGE is a path in the user's folders.
    if (qEnvironmentVariableIsSet("APPIMAGE"))
        return QStringLiteral("AppImage");
    if (QFile::exists(QStringLiteral("/.flatpak-info")))
        return QStringLiteral("Flatpak");
    return QStringLiteral("built from source");
}

QString distribution()
{
    // Inside the AppImage this is still the host's file (AppImages do not bring one).
    QFile file(QStringLiteral("/etc/os-release"));
    if (!file.open(QIODevice::ReadOnly)) {
        file.setFileName(QStringLiteral("/usr/lib/os-release"));
        if (!file.open(QIODevice::ReadOnly))
            return QSysInfo::prettyProductName();
    }
    const auto [name, version] = parseOsRelease(file.read(64 * 1024));
    if (name.isEmpty())
        return QSysInfo::prettyProductName();
    return version.isEmpty() ? name : name + QLatin1Char(' ') + version;
}

QString hardwareDecoding()
{
    QString text = media::hwAccelName(media::preferredHwAccel());
    QStringList tried;
    for (const media::HwDeviceInfo& info : media::probedHwDevices()) {
        QString entry = info.name
            + (info.available ? QStringLiteral(" available") : QStringLiteral(" unavailable"));
        if (info.available && !info.driver.isEmpty())
            entry += QStringLiteral(" (") + info.driver + QLatin1Char(')');
        tried.append(entry);
    }
    if (!tried.isEmpty())
        text += QStringLiteral(" setting; ") + tried.join(QStringLiteral(", "));
    else
        text += QStringLiteral(" setting; no device opened yet");
    return text;
}

QString screens()
{
    const QList<QScreen*> all = QGuiApplication::screens();
    QString text = QString::number(all.size());
    if (const QScreen* primary = QGuiApplication::primaryScreen()) {
        const QSize size = primary->size();
        text += QStringLiteral(", primary %1x%2 at scale %3")
                    .arg(size.width())
                    .arg(size.height())
                    .arg(primary->devicePixelRatio());
    }
    return text;
}

QString styleName()
{
    const QString style = theme::currentStyle();
    return style.isEmpty() ? QStringLiteral("default") : style;
}

QString colorSchemeName()
{
    const QString scheme = theme::currentColorScheme();
    return scheme.isEmpty() ? QStringLiteral("system") : scheme;
}

/// Whole-word, case-sensitive replacement of @p word (skipped when too short to be told
/// apart from ordinary words).
void removeWord(QString& text, const QString& word)
{
    if (word.size() < 3)
        return;
    const QRegularExpression re(
        QStringLiteral("(?<![\\w.-])%1(?![\\w-])").arg(QRegularExpression::escape(word)));
    text.replace(re, QLatin1String(kRemoved));
}

} // namespace

QString templateFile(Kind kind)
{
    switch (kind) {
    case Kind::Bug:
        return QStringLiteral("bug_report.yml");
    case Kind::ResolveDifference:
        return QStringLiteral("resolve_difference.yml");
    case Kind::Idea:
        return QStringLiteral("feature_request.yml");
    }
    return QStringLiteral("bug_report.yml");
}

QString titlePrefix(Kind kind)
{
    switch (kind) {
    case Kind::Bug:
        return QStringLiteral("[Bug] ");
    case Kind::ResolveDifference:
        return QStringLiteral("[Resolve] ");
    case Kind::Idea:
        return QStringLiteral("[Idea] ");
    }
    return {};
}

bool hasEnvironment(Kind kind)
{
    return kind != Kind::Idea;
}

QString scrub(const QString& text)
{
    QString out = text;
    // The home folder first (a path contains the user name too).
    const QString home = QDir::homePath();
    if (home.size() > 1)
        out.replace(home, QLatin1String(kRemoved));
    for (const char* var : {"USER", "LOGNAME"})
        removeWord(out, qEnvironmentVariable(var));
    removeWord(out, QSysInfo::machineHostName());
    return out;
}

QPair<QString, QString> parseOsRelease(const QByteArray& text)
{
    QString name;
    QString version;
    for (const QByteArray& raw : text.split('\n')) {
        const QByteArray line = raw.trimmed();
        const int eq = line.indexOf('=');
        if (eq <= 0 || line.startsWith('#'))
            continue;
        const QByteArray key = line.left(eq);
        QString value = QString::fromUtf8(line.mid(eq + 1)).trimmed();
        if (value.size() >= 2 && (value.front() == QLatin1Char('"') || value.front() == '\'')
            && value.back() == value.front())
            value = value.mid(1, value.size() - 2);
        value.remove(QLatin1Char('\\'));
        if (key == "NAME")
            name = value;
        else if (key == "VERSION_ID")
            version = value;
    }
    return {name, version};
}

SystemInfo collect()
{
    SystemInfo info;
    info.version = appinfo::displayVersion();
    QList<Line>& l = info.lines;

    QString kinoklip = info.version;
    if (const QString commit = QLatin1String(KK_GIT_COMMIT); !commit.isEmpty())
        kinoklip += QStringLiteral(" (commit %1)").arg(commit);
    l.append({QStringLiteral("Kinoklip"), kinoklip});
    l.append({QStringLiteral("Package"), packageKind()});
    l.append({QStringLiteral("Distribution"), distribution()});
    l.append({QStringLiteral("Kernel"), QSysInfo::kernelVersion()});
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    const QString session = qEnvironmentVariable("XDG_SESSION_TYPE");
    l.append({QStringLiteral("Desktop"),
        QStringLiteral("%1, %2 session, Qt platform %3")
            .arg(desktop.isEmpty() ? QStringLiteral("unknown") : desktop,
                session.isEmpty() ? QStringLiteral("unknown") : session,
                QGuiApplication::platformName())});
    l.append({QStringLiteral("Qt"),
        QStringLiteral("%1 (built with %2)")
            .arg(QLatin1String(qVersion()), QLatin1String(QT_VERSION_STR))});
    l.append({QStringLiteral("FFmpeg"),
        QStringLiteral("%1; avcodec %2, avformat %3, avutil %4, swscale %5, swresample %6")
            .arg(QLatin1String(av_version_info()), libVersion(avcodec_version()),
                libVersion(avformat_version()), libVersion(avutil_version()),
                libVersion(swscale_version()), libVersion(swresample_version()))});
    // Only what an already made context reported: no window or context just for this.
    if (const render::GlDriverInfo gl = render::knownGlDriverInfo(); !gl.isEmpty()) {
        l.append({QStringLiteral("OpenGL"),
            QStringLiteral("%1; %2; %3").arg(gl.renderer, gl.version, gl.vendor)});
    } else {
        l.append({QStringLiteral("OpenGL"), QStringLiteral("not started yet")});
    }
    l.append({QStringLiteral("Hardware decoding"), hardwareDecoding()});
    l.append({QStringLiteral("Screens"), screens()});
    l.append({QStringLiteral("Look"),
        QStringLiteral("%1 style, %2 colour theme").arg(styleName(), colorSchemeName())});
    l.append({QStringLiteral("Language"), QLocale::system().name()});

    for (Line& line : l)
        line.second = scrub(line.second.simplified());
    info.version = scrub(info.version);
    return info;
}

QString environmentText(const QList<Line>& lines)
{
    QStringList out;
    for (const Line& line : lines)
        out.append(line.first + QStringLiteral(": ") + line.second);
    return out.join(QLatin1Char('\n'));
}

QString IssueLink::sharedText() const
{
    if (!environment.isEmpty())
        return environment;
    if (!version.isEmpty())
        return QStringLiteral("Kinoklip: ") + version;
    return {};
}

IssueLink issueLink(Kind kind, const SystemInfo& info, bool share)
{
    // Every value fully percent-encoded ('+' included: some servers read it as a space).
    auto param = [](const char* key, const QString& value) {
        return QByteArray(key) + '=' + QUrl::toPercentEncoding(value);
    };
    const QByteArray base
        = QByteArray(kIssuesUrl) + "/new?" + param("template", templateFile(kind));

    IssueLink link;
    if (!share) {
        link.url = QUrl::fromEncoded(base, QUrl::StrictMode);
        return link;
    }
    link.version = info.version;
    QByteArray head
        = base + '&' + param("title", titlePrefix(kind)) + '&' + param(kVersionField, info.version);
    if (!hasEnvironment(kind)) {
        link.url = QUrl::fromEncoded(head, QUrl::StrictMode);
        return link;
    }

    QList<Line> lines = info.lines;
    auto build = [&](const QString& env) { return head + '&' + param(kEnvironmentField, env); };
    QString env = environmentText(lines);
    QByteArray url = build(env);
    // Drop the least important facts first, keeping at least the first line.
    while (url.size() > kMaxUrlLength && lines.size() > 1) {
        lines.removeLast();
        env = environmentText(lines);
        url = build(env);
    }
    // Then cut that line (one very long value).
    while (url.size() > kMaxUrlLength && !env.isEmpty()) {
        const qsizetype over = url.size() - kMaxUrlLength;
        // An encoded character takes up to 9 bytes (3 per UTF-8 byte); cut generously.
        env.chop(std::max<qsizetype>(1, std::min<qsizetype>(env.size(), over / 9 + 1)));
        url = build(env + QStringLiteral("..."));
        if (url.size() <= kMaxUrlLength)
            env += QStringLiteral("...");
    }
    if (url.size() > kMaxUrlLength) { // the head alone is too long (cannot happen in practice)
        env.clear();
        url = head;
    }
    link.environment = env;
    link.url = QUrl::fromEncoded(url, QUrl::StrictMode);
    return link;
}

} // namespace kk::bugreport
