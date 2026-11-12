// SPDX-License-Identifier: GPL-3.0-or-later
// Standard transitions as project settings, and the default interpolation of new keyframes.
#include "core/Animation.h"
#include "core/AudioTransitions.h"
#include "core/EditBehaviour.h"
#include "core/Project.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;

class TestStdDefaults : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_stddefaults"));
    }

    void init() { QSettings().clear(); }

    void newProjectStartsFromPreference()
    {
        QSettings().setValue(QStringLiteral("edit/transitionSeconds"), 2.0);
        Project p;
        p.newProject();
        QCOMPARE(p.standardTransitionFrames(), int64_t(48)); // 24 fps
        QCOMPARE(p.standardTransitionId(false), QStringLiteral("cross_dissolve"));
        QCOMPARE(p.standardTransitionId(true), audiotransitions::defaultId());
        QSettings().setValue(QStringLiteral("edit/transitionSeconds"), 1.0);
        QCOMPARE(p.standardTransitionFrames(), int64_t(48)); // the project keeps its own
    }

    void roundTripAndOldProject()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("a.kinoklip"));
        Project p;
        ProjectSettings s = p.settings();
        s.stdVideoTransition = QStringLiteral("wipe");
        s.stdAudioTransition = QStringLiteral("audio_crossfade_scurve");
        s.stdTransitionFrames = 37;
        p.setSettings(s);
        QVERIFY(p.save(path));
        Project q;
        QVERIFY(q.load(path));
        QCOMPARE(q.standardTransitionId(false), QStringLiteral("wipe"));
        QCOMPARE(q.standardTransitionId(true), QStringLiteral("audio_crossfade_scurve"));
        QCOMPARE(q.standardTransitionFrames(), int64_t(37));

        // An old file has none of the keys: Cross Dissolve and the preference's length.
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QJsonObject project = root.value(QStringLiteral("project")).toObject();
        project.remove(QStringLiteral("stdVideoTransition"));
        project.remove(QStringLiteral("stdAudioTransition"));
        project.remove(QStringLiteral("stdTransitionFrames"));
        root[QStringLiteral("project")] = project;
        root[QStringLiteral("formatVersion")] = 17;
        const QString old = dir.filePath(QStringLiteral("old.kinoklip"));
        QFile out(old);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QJsonDocument(root).toJson());
        out.close();
        QSettings().setValue(QStringLiteral("edit/transitionSeconds"), 0.5);
        Project o;
        QVERIFY(o.load(old));
        QCOMPARE(o.standardTransitionId(false), QStringLiteral("cross_dissolve"));
        QCOMPARE(o.standardTransitionId(true), audiotransitions::defaultId());
        QCOMPARE(o.standardTransitionFrames(), int64_t(12)); // 0.5 s at 24 fps
        // Opening the settings and saving them unchanged keeps it following the preference.
        o.setSettings(o.settings());
        QSettings().setValue(QStringLiteral("edit/transitionSeconds"), 1.0);
        QCOMPARE(o.standardTransitionFrames(), int64_t(24));
    }

    void setStandardTransitionIsOneUndoStep()
    {
        Project p;
        const int before = p.undoStack()->count();
        QVERIFY(p.setStandardTransition(false, QStringLiteral("wipe")));
        QCOMPARE(p.undoStack()->count(), before + 1);
        QCOMPARE(p.standardTransitionId(false), QStringLiteral("wipe"));
        QVERIFY(p.setStandardTransition(true, QStringLiteral("audio_crossfade_0db")));
        QCOMPARE(p.undoStack()->count(), before + 2);
        // Wrong kind of transition, and an unchanged choice, make no step.
        QVERIFY(!p.setStandardTransition(false, QStringLiteral("audio_crossfade_0db")));
        QVERIFY(p.setStandardTransition(false, QStringLiteral("wipe")));
        QCOMPARE(p.undoStack()->count(), before + 2);
        p.undo();
        QCOMPARE(p.standardTransitionId(true), audiotransitions::defaultId());
        QCOMPARE(p.standardTransitionId(false), QStringLiteral("wipe"));
        p.undo();
        QCOMPARE(p.standardTransitionId(false), QStringLiteral("cross_dissolve"));
        p.redo();
        QCOMPARE(p.standardTransitionId(false), QStringLiteral("wipe"));
    }

    void newKeysUseDefaultInterp()
    {
        QCOMPARE(defaultKeyInterp(), Interp::Linear);
        AnimatedValue v(0.0);
        v.setKey(0, 1.0);
        QCOMPARE(keyInterp(v.keys[0]), Interp::Linear);
        for (const Interp i : {Interp::EaseIn, Interp::EaseOut, Interp::EaseInOut, Interp::Smooth,
                 Interp::Hold}) {
            setDefaultKeyInterp(i);
            QCOMPARE(defaultKeyInterp(), i);
            AnimatedValue w(0.0);
            w.setKey(10, 1.0);
            QCOMPARE(keyInterp(w.keys[0]), i);
            w.setKey(10, 2.0); // an existing key keeps its type
            QCOMPARE(keyInterp(w.keys[0]), i);
            setDefaultKeyInterp(Interp::Linear);
            w.setKey(10, 3.0);
            QCOMPARE(keyInterp(w.keys[0]), i);
            w.setKey(20, 3.0, Interp::Linear); // an explicit type wins
            QCOMPARE(keyInterp(w.keys[1]), Interp::Linear);
        }
        setDefaultKeyInterp(Interp::Custom);
        QCOMPARE(defaultKeyInterp(), Interp::Linear);
    }
};

QTEST_GUILESS_MAIN(TestStdDefaults)
#include "test_stddefaults.moc"
