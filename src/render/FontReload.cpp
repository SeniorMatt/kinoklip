// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FontReload.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFontDatabase>
#include <QStandardPaths>

#include <atomic>

namespace kk::render::fonts {

namespace {

std::atomic<int> g_generation {0};
/// Upper bound of watched folders (a few hundred in /usr/share/fonts is normal).
constexpr int kMaxWatched = 2048;
constexpr int kMaxRetries = 4;
constexpr int kRetryMs = 3000;

bool isFontFile(const QString& name)
{
    static const QStringList suffixes {QStringLiteral("ttf"), QStringLiteral("otf"),
        QStringLiteral("ttc"), QStringLiteral("otc"), QStringLiteral("pfb")};
    return suffixes.contains(QFileInfo(name).suffix(), Qt::CaseInsensitive);
}

} // namespace

int fontGeneration()
{
    return g_generation.load(std::memory_order_relaxed);
}

QStringList defaultFontDirs()
{
    if (const QByteArray env = qgetenv("KK_FONT_DIRS"); !env.isNull())
        return QString::fromLocal8Bit(env).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    QStringList dirs {QDir::home().filePath(QStringLiteral(".local/share/fonts")),
        QDir::home().filePath(QStringLiteral(".fonts"))};
    const QStringList data = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString& d : data)
        dirs << QDir(d).filePath(QStringLiteral("fonts"));
    dirs << QStringLiteral("/usr/share/fonts") << QStringLiteral("/usr/local/share/fonts");
    QStringList unique;
    for (const QString& d : std::as_const(dirs)) {
        const QString clean = QDir::cleanPath(d);
        if (!unique.contains(clean))
            unique << clean;
    }
    return unique;
}

FontWatcher::FontWatcher(const QStringList& dirs, QObject* parent, int debounceMs)
    : QObject(parent)
    , m_dirs(dirs)
{
    m_known = scan();
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(debounceMs);
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        m_retries = 0;
        m_debounce.start();
    });
    connect(&m_debounce, &QTimer::timeout, this, [this] { reload(); });
    connect(&m_retry, &QTimer::timeout, this, [this] { reload(); });
    watchFolders();
}

QSet<QString> FontWatcher::scan() const
{
    QSet<QString> files;
    for (const QString& dir : m_dirs) {
        QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            const QString path = it.next();
            if (isFontFile(path))
                files.insert(QFileInfo(path).canonicalFilePath());
        }
    }
    return files;
}

void FontWatcher::watchFolders()
{
    QStringList want;
    for (const QString& dir : m_dirs) {
        if (QFileInfo(dir).isDir()) {
            want << dir;
            QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot,
                QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
            while (it.hasNext() && want.size() < kMaxWatched)
                want << it.next();
        } else {
            // Not there yet (~/.fonts): watch the nearest existing parent for its creation.
            QDir parent = QFileInfo(dir).dir();
            while (!parent.exists() && parent.cdUp()) { }
            want << parent.absolutePath();
        }
    }
    const QStringList have = m_watcher.directories();
    QStringList add;
    for (const QString& d : std::as_const(want)) {
        if (!have.contains(d))
            add << d;
    }
    if (!add.isEmpty())
        m_watcher.addPaths(add);
}

QStringList FontWatcher::reload()
{
    watchFolders();
    QStringList families;
    bool failed = false;
    const QSet<QString> now = scan();
    for (const QString& path : now) {
        if (m_known.contains(path))
            continue;
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) {
            failed = true; // maybe still being copied: tried again later
            continue;
        }
        m_known.insert(path);
        families << QFontDatabase::applicationFontFamilies(id);
    }
    families.removeDuplicates();
    if (failed && m_retries < kMaxRetries) {
        ++m_retries;
        m_retry.start();
    }
    if (!families.isEmpty()) {
        g_generation.fetch_add(1, std::memory_order_relaxed);
        emit fontsAdded(families);
    }
    return families;
}

} // namespace kk::render::fonts
