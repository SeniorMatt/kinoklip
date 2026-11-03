// SPDX-License-Identifier: GPL-3.0-or-later
// Spec 41.12: viewer guides are display only. A project whose timeline carries guides renders
// (--render-frame and --render to an image sequence) exactly the same pixels as the same
// project without them, so guides never reach a render, an export or a frame grab.
#include "RenderTestUtil.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "export/RenderCommand.h"
#include "render/RenderFrameCommand.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kFrames = 4; ///< clip length: a short image sequence

/// Saves a 320 x 180 project with a coloured solid and a title-like second solid to @p path;
/// with @p guides the timeline also carries guides.
bool saveProject(const QString& path, bool guides)
{
    Project p;
    p.beginEdit(QStringLiteral("Build"));
    TimelineData& tl = p.timeline()->mutableData();
    tl.width = 320;
    tl.height = 180;
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Solid;
    c.duration = kFrames;
    c.solidColor = QColor(0x20, 0x80, 0x40);
    tl.videoTracks[0].clips.push_back(c);
    if (guides) {
        tl.guides = {{Qt::Horizontal, 60}, {Qt::Horizontal, 120}, {Qt::Vertical, 106.5},
            {Qt::Vertical, 213}};
    }
    p.endEdit();
    return p.save(path);
}

} // namespace

class TestGuidesRender : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void rendersIgnoreGuides()
    {
        QTemporaryDir dir;
        const QString plain = dir.filePath(QStringLiteral("plain.kkproj"));
        const QString guided = dir.filePath(QStringLiteral("guided.kkproj"));
        QVERIFY(saveProject(plain, false));
        QVERIFY(saveProject(guided, true));
        // The guides really are in the document the renderer reads.
        QFile file(guided);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("\"guides\""));

        // --render-frame
        const QString a = dir.filePath(QStringLiteral("a.png"));
        const QString b = dir.filePath(QStringLiteral("b.png"));
        QCOMPARE(runRenderFrame(plain, 1, a), 0);
        QCOMPARE(runRenderFrame(guided, 1, b), 0);
        const QImage imageA(a), imageB(b);
        QVERIFY(!imageA.isNull());
        QCOMPARE(imageB, imageA);

        // --render (export) to an image sequence.
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("plain")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("guided")));
        QCOMPARE(runRender(plain, dir.filePath(QStringLiteral("plain/f_%03d.png"))), 0);
        QCOMPARE(runRender(guided, dir.filePath(QStringLiteral("guided/f_%03d.png"))), 0);
        const QStringList frames
            = QDir(dir.filePath(QStringLiteral("plain"))).entryList({QStringLiteral("*.png")});
        QVERIFY(!frames.isEmpty());
        QCOMPARE(QDir(dir.filePath(QStringLiteral("guided"))).entryList({QStringLiteral("*.png")}),
            frames);
        for (const QString& f : frames) {
            QCOMPARE(QImage(dir.filePath(QStringLiteral("guided/") + f)),
                QImage(dir.filePath(QStringLiteral("plain/") + f)));
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestGuidesRender>(argc, argv);
}

#include "test_guidesrender.moc"
