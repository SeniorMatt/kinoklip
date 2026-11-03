// SPDX-License-Identifier: GPL-3.0-or-later
// The parameter types added with the second effects batch (docs/behavior-spec.md section 40):
// Curve values (core/CurveParam.h: parsing, formatting, interpolation) and File values
// (core/FileParams.h: stored absolute + relative to the project, found again after the
// project folder moved), including a project save / load.
#include "core/CurveParam.h"
#include "core/EffectRegistry.h"
#include "core/FileParams.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(bytes) == bytes.size();
}

} // namespace

class TestEffectParams : public QObject
{
    Q_OBJECT

private slots:
    void curveParseAndFormat()
    {
        const curve::Points p = curve::parse(QStringLiteral("0,0 0.25,0.4 1,1"));
        QCOMPARE(p.size(), 3);
        QCOMPARE(p[1].x, 0.25);
        QCOMPARE(p[1].y, 0.4);
        QCOMPARE(curve::format(p), QStringLiteral("0,0 0.25,0.4 1,1"));
        QCOMPARE(curve::parse(curve::format(p)), p);
        // Unsorted, out of range and too close points are normalised.
        const curve::Points n = curve::parse(QStringLiteral("1,1.5 0,-1 0.5,0.5 0.505,0.9"));
        QCOMPARE(n.size(), 3);
        QCOMPARE(n.first().y, 0.0);
        QCOMPARE(n.last().y, 1.0);
        // Malformed text and fewer than two points give the identity.
        QCOMPARE(curve::parse(QStringLiteral("garbage")), curve::identity());
        QCOMPARE(curve::parse(QStringLiteral("0.5,0.5")), curve::identity());
        QCOMPARE(curve::parse(QString()), curve::identity());
        QVERIFY(curve::isIdentity(curve::parse(curve::identityText())));
        QVERIFY(curve::isIdentity(curve::parse(QStringLiteral("0,0 0.5,0.5 1,1"))));
        QVERIFY(!curve::isIdentity(p));
        // At most kMaxPoints points.
        QString many;
        for (int i = 0; i <= 40; ++i)
            many += QStringLiteral("%1,%1 ").arg(i / 40.0);
        QCOMPARE(curve::parse(many).size(), curve::kMaxPoints);
    }

    void curveInterpolation()
    {
        // Passes through every point; monotone data gives a monotone curve without overshoot.
        const curve::Points s = curve::parse(QStringLiteral("0,0 0.25,0.1 0.75,0.9 1,1"));
        QCOMPARE(curve::evaluate(s, 0.25), 0.1);
        QCOMPARE(curve::evaluate(s, 0.75), 0.9);
        double last = -1;
        for (int i = 0; i <= 100; ++i) {
            const double v = curve::evaluate(s, i / 100.0);
            QVERIFY(v >= last - 1e-12);
            QVERIFY(v >= 0.0 && v <= 1.0);
            last = v;
        }
        // Flat outside the end points' x range.
        const curve::Points lifted = curve::parse(QStringLiteral("0.2,0.3 1,1"));
        QCOMPARE(curve::evaluate(lifted, 0.0), 0.3);
        QCOMPARE(curve::evaluate(lifted, 0.1), 0.3);
        // A peak stays within its neighbours (no ringing above 0.8).
        const curve::Points peak = curve::parse(QStringLiteral("0,0 0.5,0.8 1,0"));
        for (int i = 0; i <= 100; ++i)
            QVERIFY(curve::evaluate(peak, i / 100.0) <= 0.8 + 1e-9);
        float table[256];
        curve::sample(s, table, 256);
        QCOMPARE(double(table[0]), 0.0);
        QCOMPARE(double(table[255]), 1.0);
        QVERIFY(std::abs(table[64] - curve::evaluate(s, 64 / 255.0)) < 1e-6);
    }

    void fileParamsEncodeDecode()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString lut = dir.filePath(QStringLiteral("project/luts/look.cube"));
        QVERIFY(writeFile(lut, "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n"));
        TimelineData tl = makeDefaultTimeline(QStringLiteral("t"), {25, 1}, 320, 180);
        ClipData clip;
        clip.id = tl.allocateId();
        clip.kind = ClipKind::Solid;
        clip.duration = 10;
        EffectInstance e = EffectRegistry::instance().createEffect(
            QLatin1String(fxid::ApplyLut), tl.allocateId());
        QVERIFY(!fileparams::hasFileParams(tl));
        e.params.setStatic(fxkeys::LutFile, lut);
        clip.effects.append(e);
        tl.videoTracks[0].clips.push_back(clip);
        QVERIFY(fileparams::hasFileParams(tl));

        const QDir projectDir(dir.filePath(QStringLiteral("project")));
        TimelineData stored = tl;
        fileparams::encodeForSave(stored, projectDir);
        const QVariantMap map = stored.videoTracks[0]
                                    .clips[0]
                                    .effects[0]
                                    .params.statics.value(fxkeys::LutFile)
                                    .toMap();
        QCOMPARE(map.value(QLatin1String(fileparams::kRelativeKey)).toString(),
            QStringLiteral("luts/look.cube"));
        QCOMPARE(map.value(QLatin1String(fileparams::kFileKey)).toString(), lut);

        // Through JSON (the stored form survives the variant conversion).
        const QJsonObject json = serialization::timelineToJson(stored);
        TimelineData back;
        QString why;
        QVERIFY(serialization::timelineFromJson(json, back, &why));
        fileparams::decodeAfterLoad(back, projectDir);
        QCOMPARE(
            back.videoTracks[0].clips[0].effects[0].params.staticValue(fxkeys::LutFile).toString(),
            lut);

        // The absolute path is gone (the folder moved): the relative one is used.
        TimelineData moved = stored;
        QVariantMap broken = map;
        broken.insert(QLatin1String(fileparams::kFileKey), QStringLiteral("/nowhere/look.cube"));
        moved.videoTracks[0].clips[0].effects[0].params.setStatic(fxkeys::LutFile, broken);
        fileparams::decodeAfterLoad(moved, projectDir);
        QCOMPARE(
            moved.videoTracks[0].clips[0].effects[0].params.staticValue(fxkeys::LutFile).toString(),
            lut);
        // Neither exists: the absolute path stays (the effect reports the missing file).
        TimelineData lost = stored;
        broken.insert(QLatin1String(fileparams::kRelativeKey), QStringLiteral("gone.cube"));
        lost.videoTracks[0].clips[0].effects[0].params.setStatic(fxkeys::LutFile, broken);
        fileparams::decodeAfterLoad(lost, projectDir);
        QCOMPARE(
            lost.videoTracks[0].clips[0].effects[0].params.staticValue(fxkeys::LutFile).toString(),
            QStringLiteral("/nowhere/look.cube"));
    }

    void projectWithLutMovesWithItsFolder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString folder = dir.filePath(QStringLiteral("a"));
        const QString lut = folder + QStringLiteral("/luts/look.cube");
        QVERIFY(writeFile(lut, "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n"));
        Project project;
        TimelineData& tl = project.timeline()->mutableData();
        ClipData clip;
        clip.id = tl.allocateId();
        clip.kind = ClipKind::Solid;
        clip.duration = 10;
        EffectInstance e = EffectRegistry::instance().createEffect(
            QLatin1String(fxid::ApplyLut), tl.allocateId());
        e.params.setStatic(fxkeys::LutFile, lut);
        EffectInstance c
            = EffectRegistry::instance().createEffect(QLatin1String(fxid::Curves), tl.allocateId());
        c.params.setStatic(fxkeys::MasterCurve, QStringLiteral("0,0 0.4,0.6 1,1"));
        clip.effects = {e, c};
        tl.videoTracks[0].clips.push_back(clip);
        project.timeline()->notifyChanged();
        QString error;
        QVERIFY2(project.save(folder + QStringLiteral("/p.kinoklip"), &error), qPrintable(error));
        // The in-memory value stays absolute after saving.
        QCOMPARE(project.timeline()->data().videoTracks[0].clips[0].effects[0].params.staticValue(
                     fxkeys::LutFile),
            QVariant(lut));
        // Move the whole folder: the LUT is found next to the project again.
        const QString movedFolder = dir.filePath(QStringLiteral("b"));
        QVERIFY(QDir().rename(folder, movedFolder));
        Project loaded;
        QVERIFY2(
            loaded.load(movedFolder + QStringLiteral("/p.kinoklip"), &error), qPrintable(error));
        const ClipData& back = loaded.timeline()->data().videoTracks[0].clips[0];
        QCOMPARE(back.effects[0].params.staticValue(fxkeys::LutFile).toString(),
            movedFolder + QStringLiteral("/luts/look.cube"));
        QCOMPARE(back.effects[1].params.staticValue(fxkeys::MasterCurve).toString(),
            QStringLiteral("0,0 0.4,0.6 1,1"));
    }

    void descriptorsOfTheNewTypes()
    {
        const EffectRegistry& r = EffectRegistry::instance();
        const ParamDescriptor* master
            = r.find(QLatin1String(fxid::Curves))->param(fxkeys::MasterCurve);
        QVERIFY(master && master->type == ParamType::Curve && !master->isScalar());
        QCOMPARE(master->defStatic.toString(), curve::identityText());
        const ParamDescriptor* file = r.find(QLatin1String(fxid::ApplyLut))->param(fxkeys::LutFile);
        QVERIFY(file && file->type == ParamType::File
            && file->fileFilter.contains(QLatin1String("*.cube")));
        QVERIFY(r.defaultParams(QLatin1String(fxid::ApplyLut)).statics.contains(fxkeys::LutFile));
    }
};

QTEST_GUILESS_MAIN(TestEffectParams)

#include "test_effectparams.moc"
