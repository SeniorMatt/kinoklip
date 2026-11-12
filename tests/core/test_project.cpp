// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for Project: snapshot undo/redo, nesting, cancel, dirty flag, settings.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/Project.h"

#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

class TestProject : public QObject
{
    Q_OBJECT
private slots:
    void newProjectDefaults()
    {
        Project p;
        QCOMPARE(p.name(), QStringLiteral("Untitled Project"));
        QCOMPARE(p.settings().fps, (Rational {24, 1})); // Resolve's default (spec 25.12)
        QCOMPARE(p.settings().timelineStart, int64_t(24 * 3600)); // 01:00:00:00
        QCOMPARE(p.timeline()->data().startTimecode, int64_t(24 * 3600));
        QCOMPARE(p.settings().width, 1920);
        QCOMPARE(p.settings().height, 1080);
        // V1 and A1, as a new Resolve timeline (spec 25.14).
        QCOMPARE(p.timeline()->data().videoTracks.size(), size_t(1));
        QCOMPARE(p.timeline()->data().audioTracks.size(), size_t(1));
        QVERIFY(!p.isDirty());
        QVERIFY(p.filePath().isEmpty());
        QCOMPARE(p.undoStack()->count(), 0);
    }

    void editPushesOneCommandAndUndoRedoRestores()
    {
        Project p;
        const TimelineData empty = p.timeline()->data();
        QSignalSpy changed(p.timeline(), &Timeline::changed);

        p.beginEdit(QStringLiteral("Add Clip"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        const TimelineData withClip = p.timeline()->data();

        QCOMPARE(p.undoStack()->count(), 1);
        QCOMPARE(p.undoStack()->undoText(), QStringLiteral("Add Clip"));
        QCOMPARE(changed.count(), 1); // first redo() is a no-op, no second notification
        QVERIFY(p.isDirty());

        p.undo();
        QCOMPARE(p.timeline()->data(), empty);
        QVERIFY(!p.isDirty());
        p.redo();
        QCOMPARE(p.timeline()->data(), withClip);
        QVERIFY(p.isDirty());
    }

    void unchangedEditPushesNothing()
    {
        Project p;
        p.beginEdit(QStringLiteral("Nothing"));
        (void)p.timeline()->mutableData();
        p.endEdit();
        QCOMPARE(p.undoStack()->count(), 0);
        QVERIFY(!p.isDirty());
    }

    void nestedEditsMergeIntoOne()
    {
        Project p;
        p.beginEdit(QStringLiteral("Outer"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.beginEdit(QStringLiteral("Inner"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(20, 10));
        p.endEdit();
        QCOMPARE(p.undoStack()->count(), 0); // still open
        QVERIFY(p.isEditing());
        p.endEdit();
        QVERIFY(!p.isEditing());
        QCOMPARE(p.undoStack()->count(), 1);
        QCOMPARE(p.undoStack()->undoText(), QStringLiteral("Outer"));
        p.undo();
        QCOMPARE(layout(p.timeline()->data(), kV1), QString());
    }

    void cancelRestoresState()
    {
        Project p;
        p.beginEdit(QStringLiteral("Drag"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.timeline()->notifyChanged(); // live update during a drag
        p.cancelEdit();
        QCOMPARE(layout(p.timeline()->data(), kV1), QString());
        QCOMPARE(p.undoStack()->count(), 0);
    }

    void nestedCancelCancelsOuterEdit()
    {
        Project p;
        p.beginEdit(QStringLiteral("Outer"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.beginEdit(QStringLiteral("Inner"));
        p.cancelEdit();
        p.endEdit();
        QCOMPARE(layout(p.timeline()->data(), kV1), QString());
        QCOMPARE(p.undoStack()->count(), 0);
    }

    void undoIgnoredWhileEditing()
    {
        Project p;
        p.beginEdit(QStringLiteral("A"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        p.beginEdit(QStringLiteral("B"));
        QTest::ignoreMessage(QtWarningMsg, "undo ignored while an edit is open");
        p.undo();
        p.endEdit();
        QCOMPARE(layout(p.timeline()->data(), kV1), QStringLiteral("0:10"));
    }

    void staleEditIsClosedKeepingItsChanges()
    {
        Project p;
        p.beginEdit(QStringLiteral("A"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        p.beginEdit(QStringLiteral("Lost Drag"));
        p.beginEdit(QStringLiteral("Nested"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(20, 5));
        QCOMPARE(p.openEditLabel(), QStringLiteral("Lost Drag"));
        QCOMPARE(p.closeStaleEdit(), QStringLiteral("Lost Drag"));
        QVERIFY(!p.isEditing());
        QCOMPARE(p.undoStack()->undoText(), QStringLiteral("Lost Drag"));
        p.undo();
        QCOMPARE(layout(p.timeline()->data(), kV1), QStringLiteral("0:10"));
        QCOMPARE(p.closeStaleEdit(), QString());
    }

    void mediaPoolChangesAreUndoable()
    {
        Project p;
        MediaItem item;
        item.path = QStringLiteral("/tmp/a.mp4");
        p.beginEdit(QStringLiteral("Add Media"));
        const QString id = p.mediaPool()->addItem(item);
        p.endEdit();
        QVERIFY(p.mediaPool()->item(id));
        p.undo();
        QVERIFY(!p.mediaPool()->item(id));
        p.redo();
        QVERIFY(p.mediaPool()->item(id));
    }

    void importSkipsInvalidFiles()
    {
        // media::probe() may be a stub or real; a nonexistent file is invalid either way.
        Project p;
        const ImportResult r = p.importMedia({QStringLiteral("/nonexistent/file.mov")});
        QVERIFY(r.importedIds.isEmpty());
        QCOMPARE(r.warnings.size(), 1);
        QCOMPARE(p.undoStack()->count(), 0);
    }

    void snapshotIsCachedUntilChange()
    {
        Project p;
        const auto s1 = p.timeline()->snapshot();
        QCOMPARE(p.timeline()->snapshot(), s1);
        p.beginEdit(QStringLiteral("x"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        const auto s2 = p.timeline()->snapshot();
        QVERIFY(s2 != s1);
        QCOMPARE(s1->videoTracks[0].clips.size(), size_t(0)); // old snapshot is immutable
        QCOMPARE(s2->videoTracks[0].clips.size(), size_t(1));
    }

    void playhead()
    {
        Project p;
        QSignalSpy spy(p.timeline(), &Timeline::playheadChanged);
        p.timeline()->setPlayhead(42);
        p.timeline()->setPlayhead(42);
        p.timeline()->setPlayhead(-5);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(p.timeline()->playhead(), 0);
    }

    void settingsAndRename()
    {
        Project p;
        QSignalSpy dirty(&p, &Project::dirtyChanged);
        ProjectSettings s = p.settings();
        s.name = QStringLiteral("Holiday");
        s.width = 1280;
        s.height = 720;
        s.fps = Rational {30000, 1001};
        p.setSettings(s);
        s.timelineStart = 30 * 3600; // still 01:00:00:00 at the new rate
        s.stdTransitionFrames = 30; // one second stays one second
        QCOMPARE(p.settings(), s);
        QVERIFY(p.isDirty());
        QCOMPARE(dirty.count(), 1);
        p.undo(); // resolution/fps are undoable, the name is not
        QCOMPARE(p.settings().width, 1920);
        QCOMPARE(p.name(), QStringLiteral("Holiday"));
        QVERIFY(p.isDirty());
    }

    void newProjectResets()
    {
        Project p;
        p.beginEdit(QStringLiteral("x"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        QSignalSpy reset(&p, &Project::projectReset);
        p.newProject();
        QCOMPARE(reset.count(), 1);
        QCOMPARE(p.undoStack()->count(), 0);
        QVERIFY(!p.isDirty());
        QCOMPARE(layout(p.timeline()->data(), kV1), QString());
    }
};

QTEST_GUILESS_MAIN(TestProject)
#include "test_project.moc"
