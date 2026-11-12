// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Vim grammar on the timeline (spec section 83): motions (clips are words, tracks
// are lines), operators with motions and text objects, counts, registers, dot-repeat, marks,
// VISUAL modes, one undo step per Vim command. Two parts: the state machine against a model
// host that records what it is asked to do, and the real host (VimTimelineHost) on a project,
// where the edit operations (ripple or gap, linked selection, locked tracks) are checked.
#include "../timeline/TimelineTestUtils.h"
#include "core/EditOps.h"
#include "ui/common/VimGrammar.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimKeymap.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/VimTimelineHost.h"

#include <QAction>
#include <QSettings>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

// ---- The model host ----

/// Tracks, in display order: 0 = V2, 1 = V1, 2 = A1, 3 = A2.
///   V2: 21 [10,30)  22 [50,70)
///   V1: 11 [0,40)   12 [40,80)  13 [100,120)
///   A1: 31 [0,40)   32 [40,80)  33 [100,120)
/// Markers at 20, 60, 90; 11 and 31 are linked. 25 frames per second.
class ModelHost final : public VimHost
{
public:
    QVector<QVector<VimClipSpan>> tracks;
    QVector<int64_t> markers {20, 60, 90};
    QVector<int64_t> keys {15, 45};
    std::pair<int64_t, int64_t> marked {-1, -1};
    int64_t frame = 5;
    int track = 1;
    QStringList ops; ///< the operations asked for, as text
    QStringList log; ///< triggered actions, pastes, messages
    int groupDepth = 0;
    int groupsOpened = 0;
    VimVisual visual;
    bool pasteWorks = true;

    ModelHost()
    {
        tracks = {{{21, 10, 30}, {22, 50, 70}}, {{11, 0, 40}, {12, 40, 80}, {13, 100, 120}},
            {{31, 0, 40}, {32, 40, 80}, {33, 100, 120}}, {}};
    }

    bool trigger(const QString& id) override
    {
        log.append(id);
        return true;
    }
    int64_t playhead() const override { return frame; }
    void setPlayhead(int64_t f) override { frame = f; }
    void clearSelection() override { log.append(QStringLiteral("clear")); }
    void openCommandLine() override { log.append(QStringLiteral("colon")); }
    void openSearch() override { log.append(QStringLiteral("search")); }
    void runEx(const QString& line) override { log.append(QStringLiteral("ex ") + line); }
    int trackCount() const override { return int(tracks.size()); }
    int cursorTrack() const override { return track; }
    void setCursorTrack(int index) override { track = index; }
    QVector<VimClipSpan> clipSpans(int t) const override { return tracks.value(t); }
    QVector<quint64> linkedClips(quint64 id) const override
    {
        if (id == 11 || id == 31)
            return {11, 31};
        return {id};
    }
    QVector<int64_t> markerFrames() const override { return markers; }
    QVector<int64_t> keyframeFrames() const override { return keys; }
    int64_t timelineEnd() const override { return 120; }
    double framesPerSecond() const override { return 25.0; }
    std::pair<int64_t, int64_t> markedRange() const override { return marked; }
    void beginUndoGroup() override
    {
        ++groupDepth;
        ++groupsOpened;
    }
    void endUndoGroup() override { --groupDepth; }
    bool operate(const VimOperation& o) override
    {
        static const char* names[] = {"d", "c", "X", "y"};
        QString text = QString::fromLatin1(names[int(o.op)]);
        if (!o.reg.isNull())
            text += QStringLiteral("[%1]").arg(o.reg);
        text += QLatin1Char(' ') + rangeText(o.range);
        ops.append(text);
        return true;
    }
    bool pasteRegister(QChar reg, int count, bool insert) override
    {
        log.append(QStringLiteral("paste[%1] %2 %3")
                       .arg(reg.isNull() ? QStringLiteral("-") : QString(reg))
                       .arg(count)
                       .arg(insert ? QStringLiteral("insert") : QStringLiteral("over")));
        return pasteWorks;
    }
    void showVisual(const VimVisual& v) override { visual = v; }
    bool selectCursorClip() override
    {
        log.append(QStringLiteral("cursor-select"));
        return true;
    }
    void restoreSelection(bool by) override
    {
        log.append(QStringLiteral("cursor-unselect %1").arg(by));
    }
    void showMessage(const QString& text) override { log.append(QStringLiteral("msg ") + text); }
    bool searchStep(bool forward, int count) override
    {
        log.append(QStringLiteral("search-step %1 %2").arg(forward).arg(count));
        return true;
    }

    static QString rangeText(const VimRange& r)
    {
        if (r.kind == VimRange::Kind::Clips) {
            QStringList ids;
            for (const quint64 id : r.clips)
                ids.append(QString::number(id));
            return QStringLiteral("C ") + ids.join(QLatin1Char(','));
        }
        QString tracksText = r.allTracks ? QStringLiteral("all")
            : r.trackFirst == r.trackLast
            ? QStringLiteral("t%1").arg(r.trackFirst)
            : QStringLiteral("t%1-%2").arg(r.trackFirst).arg(r.trackLast);
        return QStringLiteral("F %1-%2 %3").arg(r.start).arg(r.end).arg(tracksText);
    }
};

struct ModelRig
{
    VimMachine m;
    ModelHost host;
    ModelRig() { m.setHost(&host); }

    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            const QString t(c);
            QVERIFY2(m.handleKey(c.unicode(), {}, t), qPrintable(keys));
        }
    }
    void key(int k, Qt::KeyboardModifiers mods = {}, const QString& text = {})
    {
        QVERIFY(m.handleKey(k, mods, text));
    }
    /// Where the cursor is after typing @p keys from (frame, track), as "frame/track".
    QString go(const QString& keys, int64_t frame = 5, int track = 1)
    {
        host.frame = frame;
        host.track = track;
        type(keys);
        return QStringLiteral("%1/%2").arg(host.frame).arg(host.track);
    }
    /// The operation the keys caused from (frame, track).
    QString op(const QString& keys, int64_t frame = 5, int track = 1)
    {
        host.frame = frame;
        host.track = track;
        host.ops.clear();
        type(keys);
        return host.ops.join(QLatin1Char('|'));
    }
};

// ---- The real host ----

/// V1: [0,40) [40,80) [80,120) with linked audio on A1; V2: [20,60).
struct Rig
{
    TimelineFixture f;
    TimelineController controller {f.ctx};
    VimTimelineHost host {f.ctx, &controller};
    VimMachine m;
    QStringList messages;
    QVector<quint64> v1Ids;

    Rig()
    {
        controller.connectActions();
        m.setHost(&host);
        host.onMessage = [this](const QString& text) { messages.append(text); };
        f.project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < 3; ++i) {
            const quint64 v = ops::overwriteClip(f.data(), kV1, solidClip(i * 40, 40));
            const quint64 a = ops::overwriteClip(f.data(), kA1, solidClip(i * 40, 40));
            ops::linkClips(f.data(), {v, a});
            v1Ids.append(v);
        }
        ops::overwriteClip(f.data(), kV2, solidClip(20, 40));
        f.project.endEdit();
        f.project.undoStack()->clear();
        host.showCursor(true);
        f.project.timeline()->setPlayhead(0);
    }

    static constexpr TrackRef kV1 {TrackType::Video, 0};
    static constexpr TrackRef kV2 {TrackType::Video, 1};
    static constexpr TrackRef kA1 {TrackType::Audio, 0};

    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            const QString t(c);
            QVERIFY2(m.handleKey(c.unicode(), {}, t), qPrintable(keys));
        }
    }
    void key(int k, Qt::KeyboardModifiers mods = {}, const QString& text = {})
    {
        QVERIFY(m.handleKey(k, mods, text));
    }
    QString layout(TrackRef ref) const
    {
        QStringList parts;
        for (const ClipData& c : f.view().track(ref)->clips)
            parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
        return parts.join(QLatin1Char(' '));
    }
    void at(int64_t frame) { f.project.timeline()->setPlayhead(frame); }
    /// Undo steps taken: the index of the stack (an undone step does not count).
    int steps() const { return f.project.undoStack()->index(); }
    void undo() { f.project.undoStack()->undo(); }
};

} // namespace

class TestVimGrammar : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_vimgrammar"); }
    void init() { QSettings().clear(); }

    // ---- Motions on the model ----

    void clipMotionsAreWords()
    {
        ModelRig r;
        QCOMPARE(r.go(QStringLiteral("w")), QStringLiteral("40/1"));
        QCOMPARE(r.go(QStringLiteral("2w")), QStringLiteral("100/1"));
        QCOMPARE(r.go(QStringLiteral("3w")), QStringLiteral("120/1")); // the end of the last clip
        QCOMPARE(r.go(QStringLiteral("w"), 130), QStringLiteral("130/1")); // nothing after
        QCOMPARE(r.go(QStringLiteral("w"), 90), QStringLiteral("100/1")); // from a gap
        QCOMPARE(r.go(QStringLiteral("e")), QStringLiteral("39/1"));
        QCOMPARE(r.go(QStringLiteral("e"), 39), QStringLiteral("79/1"));
        QCOMPARE(r.go(QStringLiteral("3e")), QStringLiteral("119/1"));
        QCOMPARE(r.go(QStringLiteral("b"), 50), QStringLiteral("40/1"));
        QCOMPARE(r.go(QStringLiteral("b"), 40), QStringLiteral("0/1"));
        QCOMPARE(r.go(QStringLiteral("2b"), 50), QStringLiteral("0/1"));
        QCOMPARE(r.go(QStringLiteral("ge"), 50), QStringLiteral("39/1"));
        QCOMPARE(r.go(QStringLiteral("ge"), 5), QStringLiteral("5/1")); // no clip before
        // the same words on the audio track and the empty one
        QCOMPARE(r.go(QStringLiteral("w"), 5, 2), QStringLiteral("40/2"));
        QCOMPARE(r.go(QStringLiteral("w"), 5, 3), QStringLiteral("5/3"));
        // V2 has other edit points
        QCOMPARE(r.go(QStringLiteral("w"), 5, 0), QStringLiteral("10/0"));
    }

    void editPointMotionsLookAtEveryTrack()
    {
        ModelRig r;
        // edit points: 0 10 30 40 50 70 80 100 120
        QCOMPARE(r.go(QStringLiteral("W")), QStringLiteral("10/1"));
        QCOMPARE(r.go(QStringLiteral("2W")), QStringLiteral("30/1"));
        QCOMPARE(r.go(QStringLiteral("B"), 55), QStringLiteral("50/1"));
        QCOMPARE(r.go(QStringLiteral("2B"), 55), QStringLiteral("40/1"));
        QCOMPARE(r.go(QStringLiteral("E")), QStringLiteral("9/1"));
        QCOMPARE(r.go(QStringLiteral("E"), 9), QStringLiteral("29/1"));
        QCOMPARE(r.go(QStringLiteral("gE"), 45), QStringLiteral("39/1"));
        QCOMPARE(r.go(QStringLiteral("gE"), 5), QStringLiteral("5/1"));
    }

    void timeMotions()
    {
        ModelRig r;
        QCOMPARE(r.go(QStringLiteral("l")), QStringLiteral("6/1"));
        QCOMPARE(r.go(QStringLiteral("7l")), QStringLiteral("12/1"));
        QCOMPARE(r.go(QStringLiteral("h")), QStringLiteral("4/1"));
        QCOMPARE(r.go(QStringLiteral("99h")), QStringLiteral("0/1")); // clamped at the start
        QCOMPARE(r.go(QStringLiteral("}")), QStringLiteral("30/1")); // one second = 25 frames
        QCOMPARE(r.go(QStringLiteral("2}")), QStringLiteral("55/1"));
        QCOMPARE(r.go(QStringLiteral("{"), 100), QStringLiteral("75/1"));
        QCOMPARE(r.go(QStringLiteral("{"), 10), QStringLiteral("0/1"));
        QCOMPARE(r.go(QStringLiteral("0")), QStringLiteral("0/1"));
        QCOMPARE(r.go(QStringLiteral("$")), QStringLiteral("120/1"));
        QCOMPARE(r.go(QStringLiteral("47|")), QStringLiteral("47/1"));
        QCOMPARE(r.go(QStringLiteral("|")), QStringLiteral("0/1"));
        QCOMPARE(r.go(QStringLiteral("^"), 77), QStringLiteral("0/1"));
        QCOMPARE(r.go(QStringLiteral("]]"), 0), QStringLiteral("20/1"));
        QCOMPARE(r.go(QStringLiteral("2]]"), 0), QStringLiteral("60/1"));
        QCOMPARE(r.go(QStringLiteral("]]"), 90), QStringLiteral("90/1"));
        QCOMPARE(r.go(QStringLiteral("[["), 70), QStringLiteral("60/1"));
        QCOMPARE(r.go(QStringLiteral("]k"), 0), QStringLiteral("15/1"));
        QCOMPARE(r.go(QStringLiteral("2]k"), 0), QStringLiteral("45/1"));
        QCOMPARE(r.go(QStringLiteral("[k"), 50), QStringLiteral("45/1"));
        // a zero after other digits is part of the count; the first zero is the motion
        QCOMPARE(r.go(QStringLiteral("10l")), QStringLiteral("15/1"));
        QVERIFY(r.m.pending().isEmpty());
    }

    void trackMotions()
    {
        ModelRig r;
        QCOMPARE(r.go(QStringLiteral("j")), QStringLiteral("5/2"));
        QCOMPARE(r.go(QStringLiteral("jj")), QStringLiteral("5/3"));
        QCOMPARE(r.go(QStringLiteral("9j")), QStringLiteral("5/3")); // clamped
        QCOMPARE(r.go(QStringLiteral("k")), QStringLiteral("5/0"));
        QCOMPARE(r.go(QStringLiteral("5k")), QStringLiteral("5/0"));
        QCOMPARE(r.go(QStringLiteral("gg"), 5, 3), QStringLiteral("5/0"));
        QCOMPARE(r.go(QStringLiteral("G")), QStringLiteral("5/3"));
        QCOMPARE(
            r.go(QStringLiteral("2G")), QStringLiteral("5/1")); // the second track from the top
        QCOMPARE(r.go(QStringLiteral("1G")), QStringLiteral("5/0"));
        QCOMPARE(r.go(QStringLiteral("9G")), QStringLiteral("5/3"));
        // gg and G keep the frame; 0 and $ keep the track
        QCOMPARE(r.go(QStringLiteral("$"), 5, 2), QStringLiteral("120/2"));
    }

    // ---- Operators with motions ----

    void operatorsWithCharwiseMotions()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("dw")), QStringLiteral("d F 5-40 t1"));
        QCOMPARE(r.op(QStringLiteral("d2w")), QStringLiteral("d F 5-100 t1"));
        QCOMPARE(r.op(QStringLiteral("de")), QStringLiteral("d F 5-40 t1")); // inclusive
        QCOMPARE(r.op(QStringLiteral("dl")), QStringLiteral("d F 5-6 t1"));
        QCOMPARE(r.op(QStringLiteral("d3l")), QStringLiteral("d F 5-8 t1"));
        QCOMPARE(r.op(QStringLiteral("dh")), QStringLiteral("d F 4-5 t1"));
        QCOMPARE(r.op(QStringLiteral("d$")), QStringLiteral("d F 5-120 t1"));
        QCOMPARE(r.op(QStringLiteral("d0")), QStringLiteral("d F 0-5 t1"));
        QCOMPARE(r.op(QStringLiteral("db"), 50), QStringLiteral("d F 40-50 t1"));
        QCOMPARE(r.op(QStringLiteral("d}")), QStringLiteral("d F 5-30 t1"));
        QCOMPARE(r.op(QStringLiteral("d]]")), QStringLiteral("d F 5-20 t1"));
        QCOMPARE(r.op(QStringLiteral("yw")), QStringLiteral("y F 5-40 t1"));
        QCOMPARE(r.op(QStringLiteral("cl")), QStringLiteral("c F 5-6 t1"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Insert);
        // an operator and a motion that goes nowhere
        r.m.reset();
        QCOMPARE(r.op(QStringLiteral("dw"), 130), QString());
        QCOMPARE(r.op(QStringLiteral("dl"), 0).isEmpty(), false);
        QCOMPARE(r.op(QStringLiteral("dh"), 0), QString()); // nothing left of frame 0
        // an empty range is never handed to the host
        QCOMPARE(r.op(QStringLiteral("d0"), 0), QString());
    }

    void edgeWordsAcrossTracks()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("dW")), QStringLiteral("d F 5-10 all"));
        QCOMPARE(r.op(QStringLiteral("dE")), QStringLiteral("d F 5-10 all"));
        QCOMPARE(r.op(QStringLiteral("dB"), 55), QStringLiteral("d F 50-55 all"));
        // the word motions of one track use that track's clips
        QCOMPARE(r.op(QStringLiteral("dw"), 5, 0), QStringLiteral("d F 5-10 t0"));
    }

    void changeWordWorksLikeChangeEnd()
    {
        ModelRig r;
        // Neovim change.txt: "cw" on a word changes the word, not the white space after it.
        QCOMPARE(r.op(QStringLiteral("cw")), QStringLiteral("c F 5-40 t1"));
        r.m.reset();
        QCOMPARE(r.op(QStringLiteral("cw"), 39), QStringLiteral("c F 39-40 t1"));
        r.m.reset();
        QCOMPARE(r.op(QStringLiteral("c2w"), 5), QStringLiteral("c F 5-80 t1"));
        r.m.reset();
        // in a gap it is a plain w
        QCOMPARE(r.op(QStringLiteral("cw"), 85), QStringLiteral("c F 85-100 t1"));
    }

    void trackMotionsMakeClipStacks()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("dj")), QStringLiteral("d C 11,31"));
        QCOMPARE(r.op(QStringLiteral("dk")), QStringLiteral("d C 11")); // V2 has no clip at 5
        QCOMPARE(r.op(QStringLiteral("dk"), 15), QStringLiteral("d C 21,11"));
        QCOMPARE(r.op(QStringLiteral("dG")), QStringLiteral("d C 11,31"));
        QCOMPARE(r.op(QStringLiteral("dgg")), QStringLiteral("d C 11"));
        QCOMPARE(r.op(QStringLiteral("d2G")), QStringLiteral("d C 11"));
        QCOMPARE(r.op(QStringLiteral("d3G")), QStringLiteral("d C 11,31"));
    }

    void doubledOperatorsWorkOnClips()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("dd")), QStringLiteral("d C 11"));
        QCOMPARE(r.op(QStringLiteral("3dd")), QStringLiteral("d C 11,12,13"));
        QCOMPARE(r.op(QStringLiteral("d2d")), QStringLiteral("d C 11,12"));
        QCOMPARE(r.op(QStringLiteral("2d2d")), QStringLiteral("d C 11,12,13")); // 4 clips asked
        QCOMPARE(r.op(QStringLiteral("2dd"), 50), QStringLiteral("d C 12,13"));
        QCOMPARE(r.op(QStringLiteral("yy")), QStringLiteral("y C 11"));
        QCOMPARE(r.op(QStringLiteral("cc")), QStringLiteral("c C 11"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Insert);
        r.m.reset();
        // in a gap the gap is the unit
        QCOMPARE(r.op(QStringLiteral("dd"), 90), QStringLiteral("d F 80-100 t1"));
        // nothing after the last clip
        QCOMPARE(r.op(QStringLiteral("dd"), 125), QString());
    }

    void singleKeyCommands()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("x")), QStringLiteral("d C 11"));
        QCOMPARE(r.op(QStringLiteral("2x")), QStringLiteral("d C 11,12"));
        QCOMPARE(r.op(QStringLiteral("X")), QStringLiteral("X C 11"));
        QCOMPARE(r.op(QStringLiteral("Y")), QStringLiteral("y C 11"));
        QCOMPARE(r.op(QStringLiteral("D")), QStringLiteral("d F 5-40 t1"));
        QCOMPARE(r.op(QStringLiteral("D"), 90), QString()); // not in a clip
        r.host.log.clear();
        r.type(QStringLiteral("p"));
        r.type(QStringLiteral("3P"));
        QCOMPARE(r.host.log,
            (QStringList {QStringLiteral("paste[-] 1 insert"), QStringLiteral("paste[-] 3 over")}));
        r.host.log.clear();
        r.host.pasteWorks = false;
        r.type(QStringLiteral("p"));
        QVERIFY(r.host.log.last().startsWith(QLatin1String("msg ")));
    }

    // ---- Text objects ----

    void textObjects()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("diw")), QStringLiteral("d C 11"));
        QCOMPARE(r.op(QStringLiteral("d2iw")), QStringLiteral("d C 11,12"));
        QCOMPARE(r.op(QStringLiteral("daw")), QStringLiteral("d F 0-40 t1"));
        QCOMPARE(r.op(QStringLiteral("daw"), 90), QStringLiteral("d F 80-120 t1")); // gap and next
        QCOMPARE(r.op(QStringLiteral("daw"), 110), QStringLiteral("d F 100-120 t1")); // the last
        QCOMPARE(r.op(QStringLiteral("diw"), 90), QStringLiteral("d F 80-100 t1"));
        QCOMPARE(r.op(QStringLiteral("diw"), 125), QString());
        QCOMPARE(r.op(QStringLiteral("diW")), QStringLiteral("d C 11,31"));
        QCOMPARE(r.op(QStringLiteral("daW"), 45), QStringLiteral("d C 12"));
        QCOMPARE(r.op(QStringLiteral("dip")), QStringLiteral("d C 11,12"));
        QCOMPARE(r.op(QStringLiteral("dip"), 110), QStringLiteral("d C 13"));
        QCOMPARE(r.op(QStringLiteral("dip"), 90), QString());
        QCOMPARE(r.op(QStringLiteral("dap")), QStringLiteral("d F 0-100 t1"));
        QCOMPARE(r.op(QStringLiteral("dim"), 25), QStringLiteral("d F 20-60 all"));
        QCOMPARE(r.op(QStringLiteral("dim"), 5), QStringLiteral("d F 0-20 all"));
        QCOMPARE(r.op(QStringLiteral("dim"), 95), QStringLiteral("d F 90-120 all"));
        QCOMPARE(r.op(QStringLiteral("dir")), QString()); // no In / Out marks
        r.host.marked = {10, 50};
        QCOMPARE(r.op(QStringLiteral("dir")), QStringLiteral("d F 10-50 all"));
        QCOMPARE(r.op(QStringLiteral("dit")), QStringLiteral("d C 11,31"));
        QCOMPARE(r.op(QStringLiteral("dit"), 15), QStringLiteral("d C 21,11,31"));
        QCOMPARE(r.op(QStringLiteral("yiw")), QStringLiteral("y C 11"));
        QCOMPARE(r.op(QStringLiteral("ciw")), QStringLiteral("c C 11"));
    }

    // ---- Counts, registers, pending keys ----

    void countsMultiply()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("2d3l")), QStringLiteral("d F 5-11 t1"));
        QCOMPARE(r.op(QStringLiteral("d2l")), QStringLiteral("d F 5-7 t1"));
        QCOMPARE(r.op(QStringLiteral("12d2l")), QStringLiteral("d F 5-29 t1"));
        QCOMPARE(r.op(QStringLiteral("2d3w")), QStringLiteral("d F 5-120 t1"));
        QCOMPARE(r.go(QStringLiteral("2j2k")), QStringLiteral("5/1"));
    }

    void registers()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("\"ayw")), QStringLiteral("y[a] F 5-40 t1"));
        QCOMPARE(r.op(QStringLiteral("\"add")), QStringLiteral("d[a] C 11"));
        QCOMPARE(r.op(QStringLiteral("\"Ayy")), QStringLiteral("y[A] C 11")); // the host appends
        QCOMPARE(r.op(QStringLiteral("\"_dd")), QStringLiteral("d[_] C 11"));
        QCOMPARE(r.op(QStringLiteral("\"0yy")), QStringLiteral("y[0] C 11"));
        QCOMPARE(r.op(QStringLiteral("3\"add")), QStringLiteral("d[a] C 11,12,13"));
        QCOMPARE(r.op(QStringLiteral("\"a3dd")), QStringLiteral("d[a] C 11,12,13"));
        QCOMPARE(r.op(QStringLiteral("d\"")), QString()); // a register is not a motion
        QVERIFY(!r.m.operatorPending());
        r.m.reset();
        r.host.log.clear();
        r.type(QStringLiteral("\"a2p"));
        QCOMPARE(r.host.log.last(), QStringLiteral("paste[a] 2 insert"));
        // a key that is no register name drops the command
        r.type(QStringLiteral("\"!"));
        QVERIFY(r.m.pending().isEmpty());
        QCOMPARE(r.m.pendingRegister(), QChar());
    }

    void pendingTextAndEscape()
    {
        ModelRig r;
        r.type(QStringLiteral("2d"));
        QCOMPARE(r.m.pending(), QStringLiteral("2d"));
        QVERIFY(r.m.operatorPending());
        QCOMPARE(r.m.statusText(), QStringLiteral("NORMAL  2d"));
        r.type(QStringLiteral("3"));
        QCOMPARE(r.m.pending(), QStringLiteral("2d3"));
        r.key(Qt::Key_Escape);
        QVERIFY(r.m.pending().isEmpty());
        QVERIFY(!r.m.operatorPending());
        QVERIFY(r.host.ops.isEmpty());
        // after Escape w moves
        r.type(QStringLiteral("d"));
        r.host.ops.clear();
        r.key(Qt::Key_Escape);
        r.type(QStringLiteral("w"));
        QVERIFY(r.host.ops.isEmpty());
        QCOMPARE(r.host.frame, int64_t(40));
        // a register shows
        r.type(QStringLiteral("\"a"));
        QCOMPARE(r.m.pending(), QStringLiteral("\"a"));
        r.type(QStringLiteral("d"));
        QCOMPARE(r.m.pending(), QStringLiteral("\"ad"));
        r.key(Qt::Key_Escape);
        // an unbound key after an operator drops it (Vim does the same)
        r.type(QStringLiteral("dq"));
        QVERIFY(r.m.pending().isEmpty());
        QVERIFY(!r.m.operatorPending());
        QVERIFY(r.host.ops.isEmpty());
        // an operator cannot be followed by another one
        r.type(QStringLiteral("dc"));
        QVERIFY(!r.m.operatorPending());
        QVERIFY(r.host.ops.isEmpty());
    }

    // ---- Dot-repeat ----

    void dotRepeatsTheLastChange()
    {
        ModelRig r;
        QCOMPARE(r.op(QStringLiteral("dw")), QStringLiteral("d F 5-40 t1"));
        r.host.frame = 50;
        r.host.ops.clear();
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 50-100 t1"));
        // a count replaces the old one, and it sticks
        r.host.frame = 5;
        r.host.ops.clear();
        r.type(QStringLiteral("2."));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 5-100 t1"));
        r.host.ops.clear();
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 5-100 t1"));
        // the repeat does not become the last change by itself, and a yank is not a change
        r.host.ops.clear();
        r.type(QStringLiteral("yy."));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("y C 11|d F 5-100 t1"));
    }

    void dotKeepsRegisterAndCount()
    {
        ModelRig r;
        r.op(QStringLiteral("\"a2dd"));
        r.host.ops.clear();
        r.host.frame = 50;
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d[a] C 12,13"));
        // objects, X and D repeat as well
        r.op(QStringLiteral("daw"), 45);
        r.host.ops.clear();
        r.host.frame = 5;
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 0-40 t1"));
        r.op(QStringLiteral("D"));
        r.host.ops.clear();
        r.host.frame = 45;
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 45-80 t1"));
        // nothing to repeat yet
        ModelRig fresh;
        fresh.type(QStringLiteral("."));
        QVERIFY(fresh.host.ops.isEmpty());
    }

    void dotRepeatsActionsAndPastes()
    {
        ModelRig r;
        r.type(QStringLiteral("~"));
        QCOMPARE(r.host.log,
            (QStringList {QStringLiteral("cursor-select"), QStringLiteral("clip.toggle_enabled"),
                QStringLiteral("cursor-unselect 1")}));
        r.host.log.clear();
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.log.size(), 3);
        QCOMPARE(r.host.log[1], QStringLiteral("clip.toggle_enabled"));
        r.host.log.clear();
        r.type(QStringLiteral("\"b2p"));
        r.host.log.clear();
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.log, QStringList {QStringLiteral("paste[b] 2 insert")});
        r.host.log.clear();
        r.type(QStringLiteral("3>"));
        QCOMPARE(r.host.log.count(QStringLiteral("trim.nudge_right")), 3);
        r.host.log.clear();
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.log.count(QStringLiteral("trim.nudge_right")), 3);
    }

    // ---- One command is one undo step ----

    void everyChangeIsOneGroup()
    {
        ModelRig r;
        r.type(QStringLiteral("3dd"));
        QCOMPARE(r.host.groupsOpened, 1);
        QCOMPARE(r.host.groupDepth, 0);
        r.type(QStringLiteral("dw"));
        QCOMPARE(r.host.groupsOpened, 2);
        r.type(QStringLiteral("5>"));
        QCOMPARE(r.host.groupsOpened, 3);
        r.type(QStringLiteral("3p"));
        QCOMPARE(r.host.groupsOpened, 4);
        // moves, yanks of nothing and undo open no group around the undo itself
        const int groups = r.host.groupsOpened;
        r.type(QStringLiteral("5l3u"));
        QCOMPARE(r.host.groupsOpened, groups);
        QCOMPARE(r.host.log.count(QStringLiteral("edit.undo")), 3);
        // a redo and a repeat
        r.type(QStringLiteral("."));
        QVERIFY(r.host.groupDepth == 0);
    }

    // ---- Marks ----

    void marksRememberFrameAndTrack()
    {
        ModelRig r;
        r.host.frame = 5;
        r.host.track = 1;
        r.type(QStringLiteral("ma"));
        r.host.frame = 60;
        r.host.track = 3;
        r.type(QStringLiteral("'a"));
        QCOMPARE(
            QStringLiteral("%1/%2").arg(r.host.frame).arg(r.host.track), QStringLiteral("5/1"));
        r.type(QStringLiteral("''"));
        QCOMPARE(
            QStringLiteral("%1/%2").arg(r.host.frame).arg(r.host.track), QStringLiteral("60/3"));
        r.type(QStringLiteral("`a"));
        QCOMPARE(
            QStringLiteral("%1/%2").arg(r.host.frame).arg(r.host.track), QStringLiteral("5/1"));
        // a mark never set does nothing; an operator with a mark covers the frames between
        r.type(QStringLiteral("'z"));
        QCOMPARE(r.host.frame, int64_t(5));
        r.host.frame = 60;
        r.host.track = 1;
        r.host.ops.clear();
        r.type(QStringLiteral("d'a"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 5-60 t1"));
        QVERIFY(r.m.mark(QLatin1Char('a')).has_value());
        QVERIFY(!r.m.mark(QLatin1Char('z')).has_value());
    }

    // ---- VISUAL ----

    void visualCharAcrossFrames()
    {
        ModelRig r;
        r.host.frame = 5;
        r.type(QStringLiteral("v"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Visual);
        QCOMPARE(r.m.displayName(), QStringLiteral("VISUAL"));
        QCOMPARE(r.host.visual.kind, VimVisual::Kind::Char);
        QCOMPARE(r.host.visual.anchor.frame, int64_t(5));
        r.type(QStringLiteral("3l"));
        QCOMPARE(r.host.visual.cursor.frame, int64_t(8));
        r.host.ops.clear();
        r.type(QStringLiteral("d"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 5-9 t1"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
        QCOMPARE(r.host.visual.kind, VimVisual::Kind::None);
        // backwards, the other end, y and c
        r.host.frame = 20;
        r.type(QStringLiteral("v5h"));
        r.host.ops.clear();
        r.type(QStringLiteral("y"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("y F 15-21 t1"));
        r.host.frame = 20;
        r.type(QStringLiteral("v5ho"));
        QCOMPARE(r.host.frame, int64_t(20));
        r.type(QStringLiteral("l"));
        r.host.ops.clear();
        r.type(QStringLiteral("c"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("c F 15-22 t1"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Insert);
    }

    void visualLineSelectsClips()
    {
        ModelRig r;
        r.host.frame = 5;
        r.type(QStringLiteral("V"));
        QCOMPARE(r.m.displayName(), QStringLiteral("V-LINE"));
        QCOMPARE(r.host.visual.kind, VimVisual::Kind::Line);
        r.type(QStringLiteral("jd"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d C 11,31"));
        r.host.ops.clear();
        r.host.frame = 30;
        r.host.track = 1;
        r.type(QStringLiteral("V20ly"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("y C 11,12"));
    }

    void visualBlockAcrossTracks()
    {
        ModelRig r;
        r.host.frame = 5;
        r.key(Qt::Key_V, Qt::ControlModifier, QStringLiteral("\x16"));
        QCOMPARE(r.m.displayName(), QStringLiteral("V-BLOCK"));
        r.type(QStringLiteral("jl"));
        r.type(QStringLiteral("d"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 5-7 t1-2"));
        // a charwise selection stays on the cursor track
        r.host.ops.clear();
        r.host.frame = 5;
        r.host.track = 1;
        r.type(QStringLiteral("vjld"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 5-7 t2"));
    }

    void visualSwitchesAndLeaves()
    {
        ModelRig r;
        r.type(QStringLiteral("v"));
        r.type(QStringLiteral("V"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Visual);
        QCOMPARE(r.m.visualKind(), VimVisual::Kind::Line);
        r.type(QStringLiteral("V")); // the same key leaves
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
        r.type(QStringLiteral("vv"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
        r.type(QStringLiteral("v"));
        r.host.log.clear();
        r.key(Qt::Key_Escape);
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
        QVERIFY(r.host.log.contains(QStringLiteral("clear")));
        // gv selects the last selection again
        r.host.frame = 10;
        r.type(QStringLiteral("v4l"));
        r.key(Qt::Key_Escape);
        r.host.frame = 90;
        r.type(QStringLiteral("gv"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Visual);
        QCOMPARE(r.host.visual.anchor.frame, int64_t(10));
        QCOMPARE(r.host.frame, int64_t(14));
    }

    void visualTextObjectsSelect()
    {
        ModelRig r;
        r.host.frame = 5;
        r.type(QStringLiteral("viw"));
        QCOMPARE(r.host.visual.anchor.frame, int64_t(0));
        QCOMPARE(r.host.frame, int64_t(39));
        r.type(QStringLiteral("d"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 0-40 t1"));
        r.host.ops.clear();
        r.host.frame = 25;
        r.type(QStringLiteral("vim"));
        QCOMPARE(r.host.visual.anchor.frame, int64_t(20));
        QCOMPARE(r.host.frame, int64_t(59));
        QCOMPARE(r.host.visual.anchor.track, 0);
        QCOMPARE(r.host.track, 3);
    }

    void visualOperationsRepeatWithDot()
    {
        ModelRig r;
        r.host.frame = 5;
        r.type(QStringLiteral("v3ld"));
        r.host.ops.clear();
        r.host.frame = 50;
        r.type(QStringLiteral("."));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d F 50-54 t1"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
    }

    void visualPasteReplacesTheSelection()
    {
        ModelRig r;
        r.host.frame = 5;
        r.host.log.clear();
        r.type(QStringLiteral("v3l\"ap"));
        QCOMPARE(r.host.ops.join(QLatin1Char('|')), QStringLiteral("d[_] F 5-9 t1"));
        QCOMPARE(r.host.log.last(), QStringLiteral("paste[a] 1 insert"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
    }

    // ---- Modes and other keys ----

    void insertAndCommandKeys()
    {
        ModelRig r;
        r.type(QStringLiteral("i"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Insert);
        r.key(Qt::Key_Escape);
        r.host.log.clear();
        r.type(QStringLiteral(":/"));
        QCOMPARE(r.host.log, (QStringList {QStringLiteral("colon"), QStringLiteral("search")}));
        r.host.log.clear();
        r.type(QStringLiteral("2nN"));
        QCOMPARE(r.host.log,
            (QStringList {QStringLiteral("search-step 1 2"), QStringLiteral("search-step 0 1")}));
        r.host.log.clear();
        r.type(QStringLiteral("HL"));
        QCOMPARE(r.host.log,
            (QStringList {
                QStringLiteral("timeline.prev_tab"), QStringLiteral("timeline.next_tab")}));
        r.host.log.clear();
        r.type(QStringLiteral(" si so sa sr sf"));
        QCOMPARE(r.host.log,
            (QStringList {QStringLiteral("edit.insert"), QStringLiteral("edit.overwrite"),
                QStringLiteral("edit.append_at_end"), QStringLiteral("edit.replace"),
                QStringLiteral("edit.fit_to_fill")}));
    }

    void temporaryKeysAreGoneAndTheTableIsValid()
    {
        // gi, go and ga were stop-gaps for the source edits: they are gone.
        VimKeymap map;
        map.loadDefaults();
        for (const VimBinding& b : map.bindings()) {
            QVERIFY2(b.keys != QLatin1String("gi") && b.keys != QLatin1String("go")
                    && b.keys != QLatin1String("ga"),
                qPrintable(b.keys));
        }
        // The same keys of the table: every builtin and action in it exists.
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        for (const VimKeymap::Issue& issue : map.issues(&actions))
            QVERIFY2(issue.severity != VimKeymap::Issue::Severity::Error,
                qPrintable(issue.keys + QLatin1Char(' ') + issue.text));
    }

    // ---- The grammar functions on their own ----

    void grammarFunctions()
    {
        ModelHost host;
        QVERIFY(vimgrammar::isMotion(QStringLiteral("clip_next")));
        QVERIFY(!vimgrammar::isMotion(QStringLiteral("obj_iw")));
        QVERIFY(vimgrammar::isObject(QStringLiteral("obj_iw")));
        QCOMPARE(vimgrammar::clipAt(host, 1, 45).id, quint64(12));
        QCOMPARE(vimgrammar::clipAt(host, 1, 90).id, quint64(0));
        QCOMPARE(
            vimgrammar::editPoints(host), (QVector<int64_t> {0, 10, 30, 40, 50, 70, 80, 100, 120}));
        QCOMPARE(
            vimgrammar::clipsTouching(host, 1, 2, 38, 42), (QVector<quint64> {11, 12, 31, 32}));
        QVERIFY(!vimgrammar::motion(host, QStringLiteral("nonsense"), {}, 1, false));
    }

    void rowMotionsOfAListAreLinewise()
    {
        // A list is one track of one-frame rows (VimListHost): the rows motions are inclusive,
        // so `dj` takes this row and the next.
        struct Rows final : VimHost
        {
            bool trigger(const QString&) override { return false; }
            int64_t playhead() const override { return 0; }
            void setPlayhead(int64_t) override { }
            void clearSelection() override { }
            void openCommandLine() override { }
            int64_t timelineEnd() const override { return 30; }
            int pageRows() const override { return 10; }
        } host;
        using vimgrammar::Motion;
        const auto go = [&](const char* name, int64_t from, int count = 1, bool has = false) {
            return vimgrammar::motion(host, QLatin1String(name), {from, 0}, count, has);
        };
        QVERIFY(go("row_down", 4));
        QCOMPARE(go("row_down", 4)->to.frame, int64_t(5));
        QCOMPARE(go("row_down", 4)->type, Motion::Type::Inclusive);
        QCOMPARE(go("row_down", 4, 3, true)->to.frame, int64_t(7));
        QCOMPARE(go("row_down", 28, 9, true)->to.frame, int64_t(29)); // the last row
        QCOMPARE(go("row_up", 4, 2, true)->to.frame, int64_t(2));
        QCOMPARE(go("row_up", 1, 5, true)->to.frame, int64_t(0));
        QCOMPARE(go("row_first", 12)->to.frame, int64_t(0));
        QCOMPARE(go("row_last", 12)->to.frame, int64_t(29));
        QCOMPARE(go("row_last", 12, 5, true)->to.frame, int64_t(4)); // 5G is row 5
        QCOMPARE(go("row_last", 12, 99, true)->to.frame, int64_t(29));
        QCOMPARE(go("row_half_down", 2)->to.frame, int64_t(7)); // half of 10
        QCOMPARE(go("row_half_up", 2)->to.frame, int64_t(0));
        QCOMPARE(go("row_half_down", 2, 2, true)->to.frame, int64_t(12));
        // The range `dj` covers: two rows.
        const auto motion = *go("row_down", 4);
        const VimRange range = vimgrammar::rangeOfMotion(host, {4, 0}, motion);
        QCOMPARE(range.start, int64_t(4));
        QCOMPARE(range.end, int64_t(6));
        QVERIFY(vimgrammar::isMotion(QStringLiteral("row_down")));
    }

    // ---- The real host ----

    void cursorTrackFollowsJAndK()
    {
        Rig r;
        QCOMPARE(r.host.trackCount(), 4);
        QCOMPARE(r.host.cursorTrack(), 1); // V1, the target video track
        QCOMPARE(r.host.indexOf(Rig::kV2), 0);
        QCOMPARE(r.host.indexOf(Rig::kA1), 2);
        QCOMPARE(r.host.trackAt(3).type, TrackType::Audio);
        r.type(QStringLiteral("j"));
        QCOMPARE(r.host.cursorTrack(), 2);
        QCOMPARE(r.controller.targetAudioTrack(), 0); // source edits go to the cursor track
        r.type(QStringLiteral("jj"));
        QCOMPARE(r.host.cursorTrack(), 3);
        r.type(QStringLiteral("gg"));
        QCOMPARE(r.host.cursorTrack(), 0);
        QCOMPARE(r.controller.targetVideoTrack(), 1);
        r.type(QStringLiteral("G"));
        QCOMPARE(r.host.cursorTrack(), 3);
        QVERIFY(r.controller.vimOverlay().cursor);
        QCOMPARE(r.controller.vimOverlay().cursorTrack.type, TrackType::Audio);
        QCOMPARE(r.steps(), 0); // moving is not an edit
    }

    void ddRipplesAndIsOneUndoStep()
    {
        Rig r;
        r.at(50);
        r.type(QStringLiteral("dd"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:40 40:40")); // linked selection
        QCOMPARE(r.steps(), 1);
        r.undo();
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:40 40:40 80:40"));
        // counts: one step for 2dd
        r.at(0);
        r.type(QStringLiteral("2dd"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40"));
        QCOMPARE(r.steps(), 1);
        r.undo();
        QCOMPARE(r.steps(), 0);
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
    }

    void unlinkedSelectionLeavesThePartnerAlone()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(50);
        r.type(QStringLiteral("dd"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:40 40:40 80:40"));
    }

    void charwiseDeleteRipplesTheTrackAndItsPartner()
    {
        Rig r;
        r.at(10);
        r.type(QStringLiteral("dw")); // frames 10..40 of V1
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:10 10:40 50:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:10 10:40 50:40")); // linked: same frames
        QCOMPARE(r.layout(Rig::kV2), QStringLiteral("20:40")); // not touched
        QCOMPARE(r.steps(), 1);
        QCOMPARE(r.f.project.timeline()->playhead(), int64_t(10));
        r.undo();
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:40 40:40 80:40"));
        // without linked selection only the cursor track changes
        r.controller.setLinkedSelection(false);
        r.at(10);
        r.type(QStringLiteral("dw"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:10 10:40 50:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:40 40:40 80:40"));
    }

    void changeLeavesAGapAndEntersInsert()
    {
        Rig r;
        r.at(10);
        r.type(QStringLiteral("cw"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:10 40:40 80:40"));
        QCOMPARE(r.m.mode(), VimMachine::Mode::Insert);
        QCOMPARE(r.steps(), 1);
        r.key(Qt::Key_Escape);
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
        // X lifts a clip: a gap stays; d closes it
        r.undo();
        r.controller.setLinkedSelection(false);
        r.at(50);
        r.type(QStringLiteral("X"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 80:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:40 40:40 80:40"));
        r.undo();
        r.type(QStringLiteral("x"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
    }

    void yankAndPasteWithRegisters()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(0);
        r.type(QStringLiteral("\"ayy"));
        r.at(40);
        r.type(QStringLiteral("yy"));
        r.at(120);
        // the unnamed register holds the second yank, "a the first
        r.type(QStringLiteral("p"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40 120:40"));
        QCOMPARE(r.steps(), 1);
        r.undo();
        r.at(120);
        r.type(QStringLiteral("\"ap"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40 120:40"));
        // the pasted clip is the first clip (a copy of clip 0): same duration, new id
        QVERIFY(r.f.view().track(Rig::kV1)->clips.back().id != r.v1Ids.first());
        // an empty register pastes nothing and says so
        r.messages.clear();
        r.type(QStringLiteral("\"zp"));
        QVERIFY(!r.messages.isEmpty());
        // the unnamed register is the editor's clipboard
        QVERIFY(!r.controller.clipboard().isEmpty());
    }

    void appendRegisterAddsClips()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(0);
        r.type(QStringLiteral("\"ayy"));
        r.at(40);
        r.type(QStringLiteral("\"Ayy"));
        QCOMPARE(r.host.registerContent(QLatin1Char('a')).clips.size(), 2);
        QCOMPARE(r.host.registerContent(QLatin1Char('A')).clips.size(), 2); // A reads a
        QCOMPARE(r.host.registerContent(QLatin1Char('a')).duration, int64_t(80));
        r.at(120);
        r.type(QStringLiteral("\"ap"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40 120:40 160:40"));
    }

    void numberedRegistersShiftOnDelete()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(0);
        r.type(QStringLiteral("yy")); // a yank fills "0 only
        QCOMPARE(r.host.registerContent(QLatin1Char('0')).clips.size(), 1);
        QVERIFY(r.host.registerContent(QLatin1Char('1')).isEmpty());
        const quint64 first = r.v1Ids[0];
        r.at(0);
        r.type(QStringLiteral("dd")); // whole clips: "1
        QCOMPARE(r.host.registerContent(QLatin1Char('1')).clips.first().clip.id, first);
        r.at(0);
        r.type(QStringLiteral("\"add")); // a named register does not keep "1 out
        QCOMPARE(r.host.registerContent(QLatin1Char('1')).clips.first().clip.id, r.v1Ids[1]);
        QCOMPARE(r.host.registerContent(QLatin1Char('2')).clips.first().clip.id, first);
        QCOMPARE(r.host.registerContent(QLatin1Char('a')).clips.first().clip.id, r.v1Ids[1]);
        r.at(0);
        r.type(QStringLiteral("\"_dd")); // the black hole keeps all of them
        QCOMPARE(r.host.registerContent(QLatin1Char('1')).clips.first().clip.id, r.v1Ids[1]);
        // "0 still holds the yank
        QCOMPARE(r.host.registerContent(QLatin1Char('0')).clips.first().clip.id, first);
    }

    void smallDeleteGoesToTheDashRegister()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(10);
        r.type(QStringLiteral("dl")); // five frames inside one track: a small delete
        QVERIFY(!r.host.registerContent(QLatin1Char('-')).isEmpty());
        QVERIFY(r.host.registerContent(QLatin1Char('1')).isEmpty());
        r.at(10);
        r.type(QStringLiteral("\"adl")); // with a named register "- stays as it was
        const auto dash = r.host.registerContent(QLatin1Char('-')).duration;
        QCOMPARE(dash, int64_t(1));
        QVERIFY(!r.host.registerContent(QLatin1Char('a')).isEmpty());
    }

    void countedPasteIsOneStep()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(0);
        r.type(QStringLiteral("yy"));
        r.at(120);
        const int before = r.steps();
        r.type(QStringLiteral("3p"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40 120:40 160:40 200:40"));
        QCOMPARE(r.steps(), before + 1);
        r.undo();
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
    }

    void blackHoleRegisterKeepsTheClipboard()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(0);
        r.type(QStringLiteral("yy"));
        r.at(50);
        r.type(QStringLiteral("\"_dd"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
        r.at(80);
        r.type(QStringLiteral("p"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
        QCOMPARE(r.f.view().track(Rig::kV1)->clips.size(), size_t(3));
    }

    void dotRepeatInTheProject()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(0);
        r.type(QStringLiteral("dd"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
        r.type(QStringLiteral("."));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40"));
        QCOMPARE(r.steps(), 2); // one step each, the dot is one
        r.undo();
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
    }

    void textObjectsInTheProject()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(50);
        r.type(QStringLiteral("diw"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
        r.undo();
        r.at(50);
        r.type(QStringLiteral("diW")); // the clip and its audio, though linked selection is off
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:40 40:40"));
        r.undo();
        r.at(10);
        r.type(QStringLiteral("dip")); // the three clips touch
        QCOMPARE(r.layout(Rig::kV1), QString());
        r.undo();
        r.at(10);
        r.type(QStringLiteral("dit")); // the stack at frame 10: V1 and A1 (V2 starts at 20)
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
        QCOMPARE(r.layout(Rig::kV2), QStringLiteral("20:40"));
        r.undo();
    }

    void rangeObjectsTakeEveryAutoSelectTrack()
    {
        Rig r;
        r.f.data().markers = {Marker {20, QStringLiteral("a")}, Marker {60, QStringLiteral("b")}};
        r.at(30);
        r.type(QStringLiteral("dim")); // frames 20..60 on every track
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:20 20:20 40:40"));
        QCOMPARE(r.layout(Rig::kV2), QString());
        QCOMPARE(r.steps(), 1);
        r.undo();
        // an auto-select off track is left alone
        r.f.data().videoTracks[1].autoSelect = false; // V2
        r.at(30);
        r.type(QStringLiteral("dim"));
        QCOMPARE(r.layout(Rig::kV2), QStringLiteral("20:40"));
        r.undo();
        // the In / Out range
        r.f.data().videoTracks[1].autoSelect = true;
        r.f.data().markIn = 10;
        r.f.data().markOut = 30;
        r.at(0);
        r.type(QStringLiteral("cir"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:10 30:10 40:40 80:40"));
    }

    void lockedTracksAreRespected()
    {
        Rig r;
        r.f.data().videoTracks[0].locked = true; // V1
        r.at(50);
        r.type(QStringLiteral("dd"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
        QCOMPARE(r.steps(), 0);
        QVERIFY(!r.messages.isEmpty());
        r.type(QStringLiteral("dw"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
        QCOMPARE(r.steps(), 0);
        // a block over a locked and an open track changes only the open one
        r.controller.setLinkedSelection(false);
        r.at(10);
        r.key(Qt::Key_V, Qt::ControlModifier, QStringLiteral("\x16"));
        r.type(QStringLiteral("jlld"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:10 10:27 37:40 77:40"));
    }

    void visualModesInTheProject()
    {
        Rig r;
        r.at(10);
        r.type(QStringLiteral("v3ld"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:10 10:26 36:40 76:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:10 10:26 36:40 76:40")); // linked
        QCOMPARE(r.steps(), 1);
        QCOMPARE(r.m.mode(), VimMachine::Mode::Normal);
        QVERIFY(r.controller.vimOverlay().visual == 0);
        r.undo();
        // V selects the clips and shows them selected
        r.at(10);
        r.type(QStringLiteral("V"));
        QCOMPARE(r.controller.vimOverlay().visual, 2);
        QVERIFY(!r.f.selection.clips().isEmpty());
        r.type(QStringLiteral("jd"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:40 40:40"));
        r.undo();
        // v shows an overlay while it lasts
        r.type(QStringLiteral("v"));
        QCOMPARE(r.controller.vimOverlay().visual, 1);
        r.key(Qt::Key_Escape);
        QCOMPARE(r.controller.vimOverlay().visual, 0);
        QVERIFY(r.f.selection.clips().isEmpty());
    }

    void blockAcrossTracksInTheProject()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(10);
        r.key(Qt::Key_V, Qt::ControlModifier, QStringLiteral("\x16"));
        QCOMPARE(r.controller.vimOverlay().visual, 3);
        r.type(QStringLiteral("jl")); // V1 and A1, frames 10..11
        r.type(QStringLiteral("d"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:10 10:28 38:40 78:40"));
        QCOMPARE(r.layout(Rig::kA1), QStringLiteral("0:10 10:28 38:40 78:40"));
        QCOMPARE(r.layout(Rig::kV2), QStringLiteral("20:40"));
        QCOMPARE(r.steps(), 1);
    }

    void visualPasteInTheProject()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(40);
        r.type(QStringLiteral("Vp")); // nothing was copied: the unnamed register is empty
        QCOMPARE(r.steps(), 0);
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
        r.at(0);
        r.type(QStringLiteral("\"ayy")); // clip 0 into a (and the unnamed register, as in Vim)
        r.at(40);
        const int before = r.steps();
        r.type(QStringLiteral("V\"ap"));
        QCOMPARE(r.steps(), before + 1); // delete and paste are one step
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
        r.undo();
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:40 80:40"));
    }

    void deleteToEndOfClip()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(50);
        r.type(QStringLiteral("D"));
        QCOMPARE(r.layout(Rig::kV1), QStringLiteral("0:40 40:10 50:40"));
    }

    void searchFindsMarkersAndClipNames()
    {
        Rig r;
        r.f.data().markers
            = {Marker {20, QStringLiteral("Intro")}, Marker {90, QStringLiteral("Outro")}};
        QVERIFY(r.host.search(QStringLiteral("outro")));
        QCOMPARE(r.f.project.timeline()->playhead(), int64_t(90));
        r.at(0);
        r.type(QStringLiteral("n"));
        QCOMPARE(r.f.project.timeline()->playhead(), int64_t(90));
        r.type(QStringLiteral("N"));
        QCOMPARE(r.f.project.timeline()->playhead(), int64_t(90)); // wraps round to the only match
        QVERIFY(!r.host.search(QStringLiteral("nothing like this")));
        // clip names
        QVERIFY(r.host.search(QStringLiteral("solid")));
    }

    void selectionCommandsActOnTheCursorClip()
    {
        Rig r;
        r.controller.setLinkedSelection(false);
        r.at(50);
        QVERIFY(r.f.selection.clips().isEmpty());
        r.type(QStringLiteral("~"));
        QCOMPARE(r.f.view().track(Rig::kV1)->clips[1].enabled, false);
        QVERIFY(r.f.selection.clips().isEmpty()); // the selection made for it is gone again
        QCOMPARE(r.steps(), 1);
        r.type(QStringLiteral("s")); // split at the cursor
        QCOMPARE(r.f.view().track(Rig::kV1)->clips.size(), size_t(4));
        QCOMPARE(r.steps(), 2);
    }

    void cursorOverlayFollowsVimMode()
    {
        Rig r;
        QVERIFY(r.controller.vimOverlay().cursor);
        r.host.showCursor(false);
        QVERIFY(!r.controller.vimOverlay().cursor);
        r.host.showCursor(true);
        QVERIFY(r.controller.vimOverlay().cursor);
    }
};

QTEST_MAIN(TestVimGrammar)
#include "test_vimgrammar.moc"
