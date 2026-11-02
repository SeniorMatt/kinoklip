// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/Mask: shape keyframes, mask geometry helpers, KeyframeOps integration,
// edit operations (split / paste) and the format-version-3 serialization (spec section 27).
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Mask.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace kk;
using namespace kk::test;
namespace ko = kk::keyframeops;

namespace {

MaskPath triangle(double dx = 0.0)
{
    return {MaskPoint {0 + dx, 0}, MaskPoint {100 + dx, 0}, MaskPoint {50 + dx, 80}};
}

MaskData polygonMask(quint64 id)
{
    MaskData m = makeMask(MaskShape::Polygon, id, {});
    m.closed = true;
    m.path.setShape(0, triangle());
    return m;
}

// A timeline with one 100-frame clip at frame 10 carrying an animated polygon mask.
struct Fixture
{
    TimelineData d = timeline();
    quint64 clipId = 0;
    quint64 maskId = 0;
    Fixture()
    {
        ClipData c = mediaClip(10, 100);
        maskId = d.allocateId();
        MaskData m = polygonMask(maskId);
        m.path.setKey(0, triangle(0));
        m.path.setKey(40, triangle(40));
        c.masks.append(m);
        clipId = editops::overwriteClip(d, kV1, c);
    }
    ClipData& clip() { return *editops::clipById(d, clipId); }
    MaskData& mask() { return clip().masks[0]; }
    QString shapePath() const { return ko::maskPath(maskId, maskkeys::Shape); }
};

bool near(double a, double b, double eps = 1e-6)
{
    return std::abs(a - b) <= eps;
}

} // namespace

class TestMasks : public QObject
{
    Q_OBJECT
private slots:
    void shapeKeysInterpolate()
    {
        AnimatedPath p;
        p.setShape(0, triangle());
        QVERIFY(!p.isAnimated());
        QCOMPARE(p.at(17.0), triangle()); // static
        p.setKey(0);
        p.setKey(10, triangle(10));
        QCOMPARE(p.shapes.size(), 2);
        QCOMPARE(p.at(0.0)[0].x, 0.0);
        QCOMPARE(p.at(10.0)[0].x, 10.0);
        QVERIFY(near(p.at(5.0)[0].x, 5.0)); // linear keys: halfway
        QVERIFY(near(p.at(2.5)[1].x, 102.5));
        QCOMPARE(p.at(-3.0)[0].x, 0.0); // held before the first / after the last key
        QCOMPARE(p.at(30.0)[0].x, 10.0);
        // Ease in and out: slower at the start than linear.
        applyInterp(p.timing.keys[0], Interp::EaseInOut);
        applyInterp(p.timing.keys[1], Interp::EaseInOut);
        QVERIFY(p.at(2.0)[0].x < 2.0);
        QVERIFY(near(p.at(5.0)[0].x, 5.0, 1e-3));
        // Hold: the first shape stays until the next key.
        p.timing.keys[0].hold = true;
        QCOMPARE(p.at(9.0)[0].x, 0.0);
        QCOMPARE(p.at(10.0)[0].x, 10.0);
    }

    void shapeKeyEditing()
    {
        AnimatedPath p;
        p.setShape(0, triangle());
        p.setKey(20, triangle(20)); // first key: replaces the static shape
        QCOMPARE(p.shapes.size(), 1);
        QCOMPARE(p.at(0.0)[0].x, 20.0);
        p.setKey(0, triangle(0)); // earlier key: shapes stay in frame order
        QCOMPARE(p.shapes.size(), 2);
        QCOMPARE(p.shapes[0][0].x, 0.0);
        QCOMPARE(p.timing.keys[0].value, 0.0);
        QCOMPARE(p.timing.keys[1].value, 1.0);
        p.setShape(20, triangle(25)); // animated: edits the key at the frame
        QCOMPARE(p.at(20.0)[0].x, 25.0);
        p.setShape(10, triangle(5)); // ... or adds one
        QCOMPARE(p.timing.keys.size(), 3);
        QCOMPARE(p.at(10.0)[0].x, 5.0);
        p.removeKey(10);
        p.removeKey(0);
        QCOMPARE(p.timing.keys.size(), 1);
        p.removeKey(20); // the last key's shape becomes the static shape
        QVERIFY(!p.isAnimated());
        QCOMPARE(p.shapes.size(), 1);
        QCOMPARE(p.at(0.0)[0].x, 25.0);
    }

    void keyframeOpsMoveAndDeleteShapeKeys()
    {
        Fixture f;
        const QString path = f.shapePath();
        QVERIFY(ko::findValue(f.clip(), path));
        QVERIFY(ko::isShapePath(path));
        // Move the key at 0 past the key at 40: the shapes follow their keys.
        const auto moved = ko::moveKeys(f.d, {{f.clipId, path, 0}}, 60);
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved[0].frame, 60);
        const MaskData& m = f.mask();
        QCOMPARE(m.path.timing.keys[0].frame, 40);
        QCOMPARE(m.path.at(40.0)[0].x, 40.0);
        QCOMPARE(m.path.at(60.0)[0].x, 0.0);
        QCOMPARE(m.path.timing.keys[0].value, 0.0); // normalized
        // Vertical drags never change shape key values.
        QCOMPARE(ko::offsetKeyValues(f.d, {{f.clipId, path, 40}}, 3.0), 0);
        // Delete: the remaining key's shape stays.
        QCOMPARE(ko::deleteKeys(f.d, {{f.clipId, path, 40}}), 1);
        QCOMPARE(f.mask().path.shapes.size(), 1);
        QCOMPARE(f.mask().path.at(0.0)[0].x, 0.0);
        QCOMPARE(ko::deleteKeys(f.d, {{f.clipId, path, 60}}), 1);
        QVERIFY(!f.mask().path.isAnimated());
        QCOMPARE(f.mask().path.at(0.0)[0].x, 0.0);
        KK_VERIFY_INVARIANTS(f.d);
    }

    void keyframeOpsListsMasksGroup()
    {
        Fixture f;
        f.mask().params.animated(maskkeys::Opacity, 100).setKey(5, 50.0);
        const auto params = ko::animatedParams(f.clip());
        QStringList labels;
        for (const auto& p : params) {
            QCOMPARE(p.groupId, QStringLiteral("builtin.masks"));
            QCOMPARE(p.groupName, QStringLiteral("Masks"));
            labels.append(p.label);
        }
        QCOMPARE(labels,
            (QStringList {
                QStringLiteral("Polygon 1: Opacity"), QStringLiteral("Polygon 1: Shape")}));
        // Navigation and the master row see mask keys too.
        QCOMPARE(ko::allKeyFrames(f.clip()), (QVector<int64_t> {0, 5, 40}));
        QCOMPARE(ko::adjacentKeyFrame(f.clip(), 5, true), std::optional<int64_t>(40));
        QCOMPARE(ko::keysAt(f.clip(), 40).size(), 1);
    }

    void copyPasteShapeKeys()
    {
        Fixture f;
        const QString path = f.shapePath();
        const auto board = ko::copyKeys(f.d, {{f.clipId, path, 40}});
        QCOMPARE(board.entries.size(), 1);
        QCOMPARE(board.entries[0].shape, triangle(40));
        const auto pasted = ko::pasteKeys(f.d, f.clipId, board, 80);
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(f.mask().path.timing.keys.size(), 3);
        QCOMPARE(f.mask().path.at(80.0), triangle(40));
        QCOMPARE(f.mask().path.at(60.0), triangle(40)); // held between equal keys
        // A mask with another point count does not accept the key.
        f.mask().path.forEachShape([](MaskPath& p) { p.append(MaskPoint {1, 1}); });
        QCOMPARE(ko::pasteKeys(f.d, f.clipId, board, 90).size(), 0);
    }

    void editOpsKeepMaskKeysWithTheContent()
    {
        Fixture f;
        f.mask().params.animated(maskkeys::PosX, 0).setKey(40, 7.0);
        // Split at timeline frame 30 (clip-local 20): the right part's keys shift by -20 and
        // its mask gets a fresh id.
        const auto right = editops::splitClips(f.d, {f.clipId}, 30);
        QCOMPARE(right.size(), 1);
        const ClipData* r = editops::clipById(f.d, right[0]);
        QVERIFY(r);
        QCOMPARE(r->masks.size(), 1);
        QVERIFY(r->masks[0].id != f.maskId);
        QCOMPARE(r->masks[0].path.timing.keys[1].frame, 20);
        QCOMPARE(r->masks[0].params.values[maskkeys::PosX].keys[0].frame, 20);
        QCOMPARE(r->masks[0].path.at(20.0)[0].x, 40.0);
        KK_VERIFY_INVARIANTS(f.d);
        // Copy and paste gives the pasted masks fresh ids as well.
        const auto board = editops::copyClips(f.d, {f.clipId});
        editops::pasteClips(f.d, board, 200, 1);
        KK_VERIFY_INVARIANTS(f.d);
    }

    void maskTransformRoundTrip()
    {
        MaskData m = makeMask(MaskShape::Rectangle, 1, QSizeF(100, 50));
        m.params.setBase(maskkeys::PosX, 30);
        m.params.setBase(maskkeys::PosY, -20);
        m.params.setBase(maskkeys::ScaleX, 2.0);
        m.params.setBase(maskkeys::ScaleY, 0.5);
        m.params.setBase(maskkeys::Rotation, 90);
        const QPointF c = maskToContent(m, 0, QPointF(10, 0));
        // Scale 2 → (20, 0); 90° clockwise (y down) → (0, 20); plus the position.
        QVERIFY(near(c.x(), 30.0, 1e-9));
        QVERIFY(near(c.y(), 0.0, 1e-9));
        const auto back = contentToMask(m, 0, c);
        QVERIFY(back);
        QVERIFY(near(back->x(), 10.0, 1e-9) && near(back->y(), 0.0, 1e-9));
        QCOMPARE(m.value(maskkeys::Width, 0), 100.0);
        QCOMPARE(m.value(maskkeys::Opacity, 0), 100.0);
    }

    void polygonOffset()
    {
        for (const bool reversed : {false, true}) {
            QVector<QPointF> sq {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
            if (reversed)
                std::reverse(sq.begin(), sq.end());
            offsetPolygon(sq, 2.0);
            const QRectF bounds = QPolygonF(sq).boundingRect();
            QCOMPARE(bounds, QRectF(-2, -2, 14, 14));
            offsetPolygon(sq, -4.0);
            QCOMPARE(QPolygonF(sq).boundingRect(), QRectF(2, 2, 6, 6));
        }
    }

    void flattenFollowsTheCurve()
    {
        // A circle approximated by four bezier segments (k = 0.5523 · r).
        const double r = 100;
        const double k = 0.5523 * r;
        const MaskPath circle {{r, 0, 0, -k, 0, k, true}, {0, r, k, 0, -k, 0, true},
            {-r, 0, 0, k, 0, -k, true}, {0, -r, -k, 0, k, 0, true}};
        QVector<QPointF> poly;
        flattenPath(circle, true, 0.25, [](QPointF p) { return p; }, poly);
        QVERIFY(poly.size() > 16);
        for (const QPointF& p : poly)
            QVERIFY2(std::abs(std::hypot(p.x(), p.y()) - r) < 0.5, "vertex off the circle");
        // Straight polygons produce only their vertices.
        flattenPath(triangle(), true, 0.25, [](QPointF p) { return p; }, poly);
        QCOMPARE(poly.size(), 3);
    }

    void insertPointKeepsTheOutline()
    {
        MaskData m = makeMask(MaskShape::Bezier, 1, {});
        m.closed = true;
        m.path.setShape(0, {{0, 0, 0, 0, 30, -40, true}, {100, 0, -30, -40, 0, 0, true}, {50, 80}});
        const MaskPath before = m.path.at(0);
        QVector<QPointF> polyBefore;
        flattenPath(before, true, 0.1, [](QPointF p) { return p; }, polyBefore);
        const qsizetype added = insertPathPoint(m, 0, 0.5);
        QCOMPARE(added, 1);
        const MaskPath after = m.path.at(0);
        QCOMPARE(after.size(), 4);
        // The new point lies on the original cubic at t = 0.5: B(0.5) = (50, -30).
        QVERIFY(near(after[1].x, 50.0) && near(after[1].y, -30.0));
        removePathPoint(m, 1);
        QCOMPARE(m.path.pointCount(), 3);
    }

    void freehandSimplifies()
    {
        QVector<QPointF> stroke;
        for (int i = 0; i < 360; ++i) {
            const double a = i * M_PI / 180.0;
            stroke.append({200 * std::cos(a), 120 * std::sin(a)});
        }
        const MaskPath path = simplifyStroke(stroke, 2.0);
        QVERIFY(path.size() >= 6);
        QVERIFY(path.size() < 60);
        for (const MaskPoint& p : path)
            QVERIFY(p.smooth && p.hasTangents());
    }

    void renderableRules()
    {
        MaskData m = makeMask(MaskShape::Polygon, 1, {});
        QVERIFY(!m.isRenderable()); // open, no points
        m.path.setShape(0, triangle());
        QVERIFY(!m.isRenderable()); // still open (being drawn)
        m.closed = true;
        QVERIFY(m.isRenderable());
        m.enabled = false;
        QVERIFY(!m.isRenderable());
        QVERIFY(makeMask(MaskShape::Ellipse, 2, QSizeF(10, 10)).isRenderable());
    }

    void serializationRoundTrip()
    {
        Fixture f;
        MaskData& m = f.mask();
        m.mode = MaskMode::Intersect;
        m.invert = true;
        m.name = QStringLiteral("Sky");
        applyInterp(m.path.timing.keys[0], Interp::EaseOut);
        f.clip().masks.append(makeMask(MaskShape::Ellipse, f.d.allocateId(), QSizeF(300, 200)));
        const QJsonObject json = serialization::timelineToJson(f.d);
        TimelineData restored;
        QString error;
        QVERIFY2(
            serialization::timelineFromJson(
                QJsonDocument::fromJson(QJsonDocument(json).toJson()).object(), restored, &error),
            qPrintable(error));
        QCOMPARE(restored, f.d);
        // Shape keys written out of order (hand-edited file) are sorted on load.
        QJsonObject clip = serialization::clipToJson(f.clip());
        QJsonArray masks = clip[QStringLiteral("masks")].toArray();
        QJsonObject mo = masks[0].toObject();
        QJsonArray keys = mo[QStringLiteral("shapeKeys")].toArray();
        QJsonArray swapped {keys[1], keys[0]};
        mo[QStringLiteral("shapeKeys")] = swapped;
        masks[0] = mo;
        clip[QStringLiteral("masks")] = masks;
        QCOMPARE(serialization::clipFromJson(clip).masks[0], f.clip().masks[0]);
    }

    void olderFormatVersionsLoad()
    {
        // The migration chain: a version-2 file (one "timeline" object) and a version-3 file
        // ("timelines", no masks) both load; saving writes version 4 with the masks.
        QTemporaryDir dir;
        auto write = [](const QString& path, const QJsonObject& root) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(root).toJson());
        };
        TimelineData t = timeline();
        t.uid = 0;
        const QString v2 = dir.filePath(QStringLiteral("v2.kkproj"));
        write(v2,
            QJsonObject {{QStringLiteral("formatVersion"), 2},
                {QStringLiteral("project"),
                    QJsonObject {{QStringLiteral("name"), QStringLiteral("Old")}}},
                {QStringLiteral("mediaPool"), QJsonObject {}},
                {QStringLiteral("timeline"), serialization::timelineToJson(t)}});
        Project p2;
        QString error;
        QVERIFY2(p2.load(v2, &error), qPrintable(error));
        QCOMPARE(p2.timeline()->data().videoTracks.size(), size_t(2));

        // Add a masked clip, save (version 4), then label the file version 3 without masks.
        Fixture f;
        p2.beginEdit(QStringLiteral("t"));
        TimelineData& d = p2.timeline()->mutableData();
        ClipData c = f.clip();
        c.masks[0].id = d.allocateId();
        editops::overwriteClip(d, kV1, c);
        p2.endEdit();
        const QString v4 = dir.filePath(QStringLiteral("v4.kkproj"));
        QVERIFY(p2.save(v4));
        QFile file(v4);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QCOMPARE(root[QStringLiteral("formatVersion")].toInt(),
            serialization::kFormatVersion); // written as the current version (>= 4)
        Project p4;
        QVERIFY2(p4.load(v4, &error), qPrintable(error));
        QCOMPARE(p4.timeline()->data().videoTracks[0].clips[0].masks.size(), 1);
        QCOMPARE(p4.timeline()->data().videoTracks[0].clips[0].masks[0].path,
            p2.timeline()->data().videoTracks[0].clips[0].masks[0].path);

        root[QStringLiteral("formatVersion")] = 3;
        const QString v3 = dir.filePath(QStringLiteral("v3.kkproj"));
        write(v3, root);
        Project p3;
        QVERIFY2(p3.load(v3, &error), qPrintable(error));
        QCOMPARE(p3.timeline()->data().videoTracks[0].clips.size(), size_t(1));
        // A newer version is refused instead of losing data.
        root[QStringLiteral("formatVersion")] = serialization::kFormatVersion + 1;
        write(v3, root);
        QVERIFY(!p3.load(v3, &error));
    }
};

QTEST_GUILESS_MAIN(TestMasks)
#include "test_masks.moc"
