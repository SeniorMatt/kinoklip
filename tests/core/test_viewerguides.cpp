// SPDX-License-Identifier: GPL-3.0-or-later
// Viewer guides as data (spec 41): JSON, project persistence per timeline and the format
// 13 -> 14 migration, undo, presets (built-in and user files), safe area / aspect marker /
// grid geometry, snapping and ruler steps.
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"
#include "core/ViewerGuides.h"

#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;

namespace {

const ViewerGuide kH540 {Qt::Horizontal, 540};
const ViewerGuide kV640 {Qt::Vertical, 640.5};

QJsonObject readJson(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeJson(const QString& path, const QJsonObject& root)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(QJsonDocument(root).toJson()) > 0;
}

} // namespace

class TestViewerGuides : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // 41.11: [{"o": "h" | "v", "pos": px}]; invalid entries are dropped.
    void jsonRoundTrip()
    {
        const std::vector<ViewerGuide> list {kH540, kV640};
        const QJsonArray json = guides::toJson(list);
        QCOMPARE(json.size(), 2);
        QCOMPARE(json[0].toObject().value(QStringLiteral("o")).toString(), QStringLiteral("h"));
        QCOMPARE(json[1].toObject().value(QStringLiteral("pos")).toDouble(), 640.5);
        QCOMPARE(guides::fromJson(json), list);

        QJsonArray bad = json;
        bad.append(QJsonObject {{QStringLiteral("o"), QStringLiteral("x")},
            {QStringLiteral("pos"), 3}}); // unknown orientation
        bad.append(QJsonObject {{QStringLiteral("o"), QStringLiteral("v")}}); // no position
        bad.append(QJsonObject {
            {QStringLiteral("o"), QStringLiteral("v")}, {QStringLiteral("pos"), 1e300}}); // absurd
        bad.append(QStringLiteral("junk"));
        QCOMPARE(guides::fromJson(bad), list);
        QVERIFY(guides::fromJson({}).empty());
    }

    // 41.11: saved per timeline in the project; written only when there are guides.
    void projectRoundTripPerTimeline()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("g.kkproj"));
        Project p;
        p.beginEdit(QStringLiteral("Add Guide"));
        p.timeline()->mutableData().guides = {kH540, kV640};
        p.endEdit();
        const quint64 first = p.activeTimelineUid();
        const quint64 second = p.createTimeline(QStringLiteral("Second"));
        p.setActiveTimeline(second);
        QVERIFY(p.timeline()->data().guides.empty()); // each timeline its own guides
        p.beginEdit(QStringLiteral("Add Guide"));
        p.timeline()->mutableData().guides = {ViewerGuide {Qt::Vertical, 10}};
        p.endEdit();
        QVERIFY(p.save(path));

        const QJsonObject root = readJson(path);
        QCOMPARE(root.value(QStringLiteral("formatVersion")).toInt(), 14);

        Project back;
        QVERIFY(back.load(path));
        QVERIFY(back.setActiveTimeline(first));
        QCOMPARE(back.timeline()->data().guides, (std::vector<ViewerGuide> {kH540, kV640}));
        back.setActiveTimeline(second);
        QCOMPARE(back.timeline()->data().guides,
            (std::vector<ViewerGuide> {ViewerGuide {Qt::Vertical, 10}}));

        // No guides: no "guides" key (a timeline without guides is written as format 13 did).
        const QJsonObject plain = serialization::timelineToJson(TimelineData {});
        QVERIFY(!plain.contains(QStringLiteral("guides")));
    }

    // 41.11: a format-13 project (no guides anywhere) loads without guides and is written
    // back as format 14; newer files are refused.
    void version13FilesMigrate()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("v13.kkproj"));
        {
            Project p;
            QVERIFY(p.save(path));
        }
        QJsonObject root = readJson(path);
        root[QStringLiteral("formatVersion")] = 13;
        QVERIFY(writeJson(path, root));
        Project old;
        QVERIFY(old.load(path));
        QVERIFY(old.timeline()->data().guides.empty());
        QVERIFY(old.save(path));
        QCOMPARE(readJson(path).value(QStringLiteral("formatVersion")).toInt(),
            serialization::kFormatVersion);

        root[QStringLiteral("formatVersion")] = serialization::kFormatVersion + 1;
        QVERIFY(writeJson(path, root));
        Project newer;
        QVERIFY(!newer.load(path));
    }

    // 41.3: guide edits are named undo steps of the document.
    void guideEditsAreUndoable()
    {
        Project p;
        QUndoStack* undo = p.undoStack();
        const int steps = undo->count();
        p.beginEdit(QStringLiteral("Add Guide"));
        p.timeline()->mutableData().guides = {kH540};
        p.endEdit();
        QCOMPARE(undo->count(), steps + 1);
        QCOMPARE(undo->undoText(), QStringLiteral("Add Guide"));
        undo->undo();
        QVERIFY(p.timeline()->data().guides.empty());
        undo->redo();
        QCOMPARE(p.timeline()->data().guides.size(), size_t(1));
    }

    void nearestGuideHitTest()
    {
        const std::vector<ViewerGuide> list {kH540, kV640, ViewerGuide {Qt::Horizontal, 542}};
        QCOMPARE(guides::nearestGuide(list, 0, 539, 4), 0);
        QCOMPARE(guides::nearestGuide(list, 0, 541, 4), 2); // tie: the later guide
        QCOMPARE(guides::nearestGuide(list, 643, 0, 4), 1);
        QCOMPARE(guides::nearestGuide(list, 100, 100, 4), -1);
    }

    // 41.7: built-in presets in fractions; user presets in AppData.
    void presets()
    {
        const std::vector<GuidePreset> builtIn = guides::builtInPresets();
        QVERIFY(builtIn.size() >= 2);
        const QSizeF hd(1920, 1080);
        const auto centre = guides::presetGuides(builtIn[0], hd);
        QCOMPARE(centre, (std::vector<ViewerGuide> {{Qt::Horizontal, 540}, {Qt::Vertical, 960}}));
        const auto thirds = guides::presetGuides(builtIn[1], hd);
        QCOMPARE(thirds.size(), size_t(4));
        QCOMPARE(thirds[0].position, 360.0);
        QCOMPARE(thirds[3].position, 1280.0);

        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("sub/guide-presets.json"));
        QVERIFY(guides::loadUserPresets(file).empty());
        const GuidePreset mine
            = guides::makeUserPreset(QStringLiteral("Lower third"), {{Qt::Horizontal, 810}}, hd);
        QCOMPARE(mine.fractions[0].position, 0.75);
        QString error;
        QVERIFY2(guides::saveUserPresets({mine}, file, &error), qPrintable(error));
        const auto loaded = guides::loadUserPresets(file);
        QCOMPARE(loaded.size(), size_t(1));
        QCOMPARE(loaded[0].name, QStringLiteral("Lower third"));
        // Fractions: the preset fits a vertical 1080 x 1920 timeline too.
        QCOMPARE(guides::presetGuides(loaded[0], QSizeF(1080, 1920))[0].position, 1440.0);
        const auto all = guides::allPresets(file);
        QCOMPARE(all.size(), builtIn.size() + 1);
        QVERIFY(all.front().builtIn && !all.back().builtIn);
        QVERIFY(guides::userPresetPath().endsWith(QStringLiteral("/guide-presets.json")));
    }

    // 41.13 / 41.14: safe areas centred at 90 % / 80 %, aspect markers fitted, grid lines.
    void safeAreaAndGridGeometry()
    {
        const QSizeF hd(1920, 1080);
        QCOMPARE(guides::safeArea(hd, guides::kActionSafe), QRectF(96, 54, 1728, 972));
        QCOMPARE(guides::safeArea(hd, guides::kTitleSafe), QRectF(192, 108, 1536, 864));
        // 2.39:1 letterbox inside 16:9: full width, bars top and bottom.
        const QRectF scope = guides::aspectMarker(hd, 2.39);
        QCOMPARE(scope.width(), 1920.0);
        QVERIFY(std::abs(scope.height() - 1920 / 2.39) < 1e-9);
        QVERIFY(std::abs(scope.center().y() - 540) < 1e-9);
        // 9:16 pillar box: full height.
        const QRectF vertical = guides::aspectMarker(hd, 9.0 / 16.0);
        QCOMPARE(vertical.height(), 1080.0);
        QVERIFY(std::abs(vertical.width() - 607.5) < 1e-9);
        QCOMPARE(vertical.center().x(), 960.0);
        QCOMPARE(guides::aspectMarker(hd, 0), QRectF(QPointF(0, 0), hd));

        std::vector<double> xs, ys;
        guides::gridLines(hd, 3, xs, ys);
        QCOMPARE(xs, (std::vector<double> {640, 1280}));
        QCOMPARE(ys, (std::vector<double> {360, 720}));
        guides::gridLines(hd, 4, xs, ys);
        QCOMPARE(xs.size(), size_t(3));
        QCOMPARE(ys[1], 540.0);
        guides::gridLines(hd, 1, xs, ys); // clamped to 2
        QCOMPARE(xs, (std::vector<double> {960}));
        guides::gridLines(hd, 1000, xs, ys);
        QCOMPARE(int(xs.size()), guides::kMaxGridDivisions - 1);
    }

    void snapOffsets()
    {
        const std::vector<double> targets {100, 500};
        const std::vector<double> features {97, 300, 504};
        auto d = guides::snapOffset(features, targets, 6);
        QVERIFY(d);
        QCOMPARE(*d, 3.0); // 97 -> 100 beats 504 -> 500
        QVERIFY(!guides::snapOffset(features, targets, 2));
        QVERIFY(!guides::snapOffset(features, {}, 50));
        QCOMPARE(guides::positions({kH540, kV640}, Qt::Vertical), (std::vector<double> {640.5}));
    }

    // 41.5: labelled ticks at least the given spacing apart, in 1 / 2 / 5 steps.
    void guideSnapTargets()
    {
        const QSizeF frame(1920, 1080);
        const std::vector<double> xs = guides::guideSnapTargets(frame, Qt::Vertical, {700.0});
        for (const double x : {0.0, 480.0, 640.0, 960.0, 1280.0, 1440.0, 1920.0, 96.0, 1824.0,
                 192.0, 1728.0, 700.0})
            QVERIFY2(std::find(xs.begin(), xs.end(), x) != xs.end(), qPrintable(QString::number(x)));
        QVERIFY(std::is_sorted(xs.begin(), xs.end()));
        const std::vector<double> ys = guides::guideSnapTargets(frame, Qt::Horizontal, {}, 4, 2.39);
        for (const double y : {0.0, 270.0, 540.0, 810.0, 1080.0, 54.0, 108.0})
            QVERIFY2(std::find(ys.begin(), ys.end(), y) != ys.end(), qPrintable(QString::number(y)));
        const QRectF marker = guides::aspectMarker(frame, 2.39);
        QVERIFY(std::find(ys.begin(), ys.end(), marker.top()) != ys.end());
        QVERIFY(guides::guideSnapTargets(QSizeF(), Qt::Vertical, {}).empty());
    }

    void rulerSteps()
    {
        QCOMPARE(guides::rulerStep(1.0, 50), 50.0);
        QCOMPARE(guides::rulerStep(0.5, 50), 100.0);
        QCOMPARE(guides::rulerStep(0.3, 56), 200.0);
        QCOMPARE(guides::rulerStep(2.0, 56), 50.0);
        QCOMPARE(guides::rulerStep(10.0, 56), 10.0);
        QCOMPARE(guides::rulerStep(100.0, 56), 1.0);
    }
};

QTEST_GUILESS_MAIN(TestViewerGuides)
#include "test_viewerguides.moc"
