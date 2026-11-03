// SPDX-License-Identifier: GPL-3.0-or-later
// test_embeddedexport — the render path reads media stored inside a single-file project: the
// export fixture project rendered once from the plain files and once with every media pool
// item pointing into a container gives the same frames and audio.
#include "ExportTestUtil.h"
#include "export/ExportJob.h"
#include "pack/ProjectPackage.h"
#include "render/Compositor.h"

#include <QTest>

using namespace kk;
using namespace kk::test;

class TestEmbeddedExport : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Project> m_project;
    std::unique_ptr<Compositor> m_compositor;
    QString m_skip;

    ExportResult render(const QString& out)
    {
        ExportSettings settings; // default H.264 MP4 at the timeline format
        settings.outputPath = out;
        ExportJob job(settings, m_project->timeline()->snapshot(),
            makeMediaPathMap(m_project->mediaPool()->state()));
        job.setCompositor(m_compositor.get());
        return job.run({});
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_project = std::make_unique<Project>();
        if (!buildExportProject(*m_project, &m_skip))
            return;
        m_compositor = std::make_unique<Compositor>();
    }

    void cleanupTestCase() { m_compositor.reset(); }

    void rendersFromInsideTheContainer()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString plainOut = m_dir.filePath(QStringLiteral("plain.mp4"));
        ExportResult r = render(plainOut);
        QVERIFY2(r.ok, qPrintable(r.error));

        // Copy every file into a container and point the media pool at the copies.
        const QString container = m_dir.filePath(QStringLiteral("render.kinoklip"));
        QString error;
        auto package = pack::ProjectPackage::create(container, &error);
        QVERIFY2(package, qPrintable(error));
        MediaPoolState pool = m_project->mediaPool()->state();
        QStringList files;
        for (const MediaItem& m : pool.items) {
            if (!m.isTimeline())
                files.append(m.path);
        }
        QVector<pack::ProjectPackage::Added> added;
        QCOMPARE(package->addMedia(files, &added, {}, &error), pack::Status::Ok);
        int swapped = 0;
        for (MediaItem& m : pool.items) {
            for (const auto& a : std::as_const(added)) {
                if (a.source == m.path) {
                    m.path = a.path;
                    m.info.path = a.path;
                    ++swapped;
                }
            }
        }
        QCOMPARE(swapped, files.size());
        m_project->mediaPool()->setState(pool);

        const QString embeddedOut = m_dir.filePath(QStringLiteral("embedded.mp4"));
        r = render(embeddedOut);
        QVERIFY2(r.ok, qPrintable(r.error));
        for (const int64_t t :
            {int64_t(0), int64_t(500'000), int64_t(1'300'000), int64_t(1'900'000)}) {
            const QImage a = decodeAt(plainOut, t);
            const QImage b = decodeAt(embeddedOut, t);
            QVERIFY(!a.isNull() && !b.isNull());
            QVERIFY2(qAbs(meanLuma(a) - meanLuma(b)) < 0.5,
                qPrintable(
                    QStringLiteral("t=%1: %2 vs %3").arg(t).arg(meanLuma(a)).arg(meanLuma(b))));
        }
        QVERIFY(meanLuma(decodeAt(embeddedOut, 500'000)) > 5); // not black
        QVERIFY(qAbs(audioRms(plainOut, 48000) - audioRms(embeddedOut, 48000)) < 1e-3);
        QVERIFY(audioRms(embeddedOut, 48000) > 0.01);
    }
};

QTEST_MAIN(TestEmbeddedExport)
#include "test_embeddedexport.moc"
