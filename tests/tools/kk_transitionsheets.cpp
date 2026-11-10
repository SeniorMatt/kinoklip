// SPDX-License-Identifier: GPL-3.0-or-later
// kk_transitionsheets — contact sheets of the video transitions, to judge their look by eye. Draws
// every transition of the catalogue (default parameters) at progress 0.25, 0.5 and 0.75 between
// two distinct test pictures and writes one PNG per Effects library folder, the names drawn on
// them. A test tool: not part of the application and not installed.
//
//   xvfb-run -a env QT_QPA_PLATFORM=xcb ./kk_transitionsheets [output-directory]
//
// The default output directory is "transition-sheets" next to the executable (the build
// directory); nothing is ever written into the repository.
#include "../render/TransitionRig.h"
#include "core/EffectRegistry.h"

#include <QDir>
#include <QGuiApplication>
#include <QPainter>
#include <QSurfaceFormat>

#include <cstdio>

using namespace kk;
using namespace kk::test;

namespace {

constexpr QSize kTile(224, 126);
constexpr int kGap = 8;
constexpr int kLabel = 22;
constexpr int kNameColumn = 150;
constexpr double kProgress[3] = {0.25, 0.5, 0.75};

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_LOGGING_RULES", "qt.qpa.*=false");
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    const QString outDir = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                    : QDir(QCoreApplication::applicationDirPath())
                                          .filePath(QStringLiteral("transition-sheets"));
    QDir().mkpath(outDir);

    TransitionRig rig;
    QString why;
    if (!rig.initialize(&why)) {
        std::fprintf(stderr, "no OpenGL: %s\n", qPrintable(why));
        return 1;
    }
    const QImage a = testPicture(false, kTile);
    const QImage b = testPicture(true, kTile);
    const EffectRegistry& registry = EffectRegistry::instance();

    int failed = 0;
    for (const QString& group : registry.groups(EffectCategory::Transition)) {
        QVector<const EffectDescriptor*> rows;
        for (const EffectDescriptor* d : registry.byCategory(EffectCategory::Transition)) {
            if (d->group == group)
                rows.append(d);
        }
        const int rowHeight = kTile.height() + kLabel;
        const int width = kNameColumn + 5 * (kTile.width() + kGap) + kGap;
        QImage sheet(width, int(rows.size()) * (rowHeight + kGap) + kGap, QImage::Format_RGB32);
        sheet.fill(QColor(40, 42, 46));
        QPainter p(&sheet);
        p.setPen(Qt::white);
        QFont font = p.font();
        font.setPixelSize(13);
        p.setFont(font);
        int y = kGap;
        for (const EffectDescriptor* d : rows) {
            p.drawText(QRect(kGap, y, kNameColumn - kGap, rowHeight),
                Qt::AlignVCenter | Qt::TextWordWrap,
                d->name + QStringLiteral("\n(") + d->id + QLatin1Char(')'));
            // Outgoing picture, three steps, incoming picture.
            int x = kNameColumn;
            p.drawImage(x, y + kLabel, a);
            p.drawText(x, y, kTile.width(), kLabel, Qt::AlignCenter, QStringLiteral("A"));
            const ParamSet params = registry.defaultParams(d->id);
            for (double progress : kProgress) {
                x += kTile.width() + kGap;
                QString error;
                const QImage frame = rig.draw(d->id, params, progress, a, b, 0, &error);
                if (frame.isNull()) {
                    std::fprintf(
                        stderr, "%s: no picture (%s)\n", qPrintable(d->id), qPrintable(error));
                    ++failed;
                    p.fillRect(x, y + kLabel, kTile.width(), kTile.height(), Qt::darkRed);
                } else {
                    // On black, like the viewer: transparent parts show the background.
                    QImage onBlack(frame.size(), QImage::Format_RGB32);
                    onBlack.fill(Qt::black);
                    QPainter q(&onBlack);
                    q.drawImage(0, 0, frame);
                    q.end();
                    p.drawImage(x, y + kLabel, onBlack);
                }
                p.drawText(x, y, kTile.width(), kLabel, Qt::AlignCenter,
                    QStringLiteral("%1").arg(progress));
            }
            x += kTile.width() + kGap;
            p.drawImage(x, y + kLabel, b);
            p.drawText(x, y, kTile.width(), kLabel, Qt::AlignCenter, QStringLiteral("B"));
            y += rowHeight + kGap;
        }
        p.end();
        const QString file = QDir(outDir).filePath(group + QStringLiteral(".png"));
        if (!sheet.save(file)) {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(file));
            return 1;
        }
        std::printf("%s (%lld transitions)\n", qPrintable(file), (long long)rows.size());
    }
    rig.destroy();
    return failed ? 1 : 0;
}
