// SPDX-License-Identifier: GPL-3.0-or-later
// Bug hunt for 1.7 (spec 89): layout probes on a 1366 x 768 window, and the fixes of the round.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/AudioPage.h"
#include "app/pages/ColorPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "export/RenderQueue.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/color/ColorPalettes.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Page.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/deliver/RenderSettingsPanel.h"
#include "ui/dialogs/NormalizeAudioDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/editindex/EditIndexPanel.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/projects/NewProjectDialog.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTreeView>
#include <QTreeWidget>
#include <QUndoStack>

#include <functional>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.name = QStringLiteral("Plate");
        clip.start = 10;
        clip.duration = 50;
        project.beginEdit(QStringLiteral("Add Clip"));
        const quint64 id = editops::overwriteClip(
            project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        project.endEdit();
        selection.setClips({id});
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }
    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
};

/// Visible sibling buttons / combos that intersect, and button texts wider than the button.
QStringList layoutProblems(QWidget* root)
{
    QStringList out;
    const auto isLeaf = [](QWidget* w) {
        return qobject_cast<QAbstractButton*>(w) || qobject_cast<QComboBox*>(w);
    };
    const auto all = root->findChildren<QWidget*>();
    for (QWidget* w : all) {
        if (!w->isVisible() || !isLeaf(w))
            continue;
        if (auto* b = qobject_cast<QAbstractButton*>(w); b && !b->text().isEmpty()) {
            const QString t = QString(b->text()).remove(QLatin1Char('&'));
            const int need = b->fontMetrics().horizontalAdvance(t);
            if (need > b->width() && b->width() > 0)
                out << QStringLiteral("text cut: '%1' %2 > %3 (%4)")
                           .arg(t)
                           .arg(need)
                           .arg(b->width())
                           .arg(b->objectName());
        }
        QWidget* p = w->parentWidget();
        if (!p)
            continue;
        for (QWidget* o : p->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (o == w || !o->isVisible() || !isLeaf(o) || o < w)
                continue;
            const QRect i = w->geometry().intersected(o->geometry());
            if (i.width() > 2 && i.height() > 2)
                out << QStringLiteral("overlap: %1/%2 and %3/%4 in %5")
                           .arg(w->metaObject()->className(), w->objectName(),
                               o->metaObject()->className(), o->objectName(), p->objectName());
        }
    }
    return out;
}

/// Where the Tab key goes next jumps back up the screen by more than @p slack px (inside one
/// top-level widget): the chain was built in a different order than the layout.
QStringList tabOrderProblems(QWidget* root, int slack = 24)
{
    QStringList out;
    QWidget* w = root->nextInFocusChain();
    QWidget* previous = nullptr;
    int guard = 0;
    for (; w && w != root && guard < 2000; w = w->nextInFocusChain(), ++guard) {
        if (!w->isVisible() || !(w->focusPolicy() & Qt::TabFocus) || !w->isEnabled()
            || w->window() != root->window())
            continue;
        if (qobject_cast<QLineEdit*>(w->parentWidget()))
            continue; // the editor of a spin box or combo box
        if (previous) {
            const QPoint a = previous->mapTo(root->window(), QPoint(0, 0));
            const QPoint b = w->mapTo(root->window(), QPoint(0, 0));
            if (b.y() + slack < a.y())
                out << QStringLiteral("tab goes up: %1/%2 (y %3) -> %4/%5 (y %6)")
                           .arg(previous->metaObject()->className(), previous->objectName())
                           .arg(a.y())
                           .arg(w->metaObject()->className(), w->objectName())
                           .arg(b.y());
        }
        previous = w;
    }
    return out;
}

/// Labels and item-view rows whose text is wider than the room they have.
QStringList textProblems(QWidget* root)
{
    QStringList out;
    for (auto* label : root->findChildren<QLabel*>()) {
        if (!label->isVisible() || label->wordWrap() || label->text().isEmpty()
            || label->text().contains(QLatin1Char('<')))
            continue;
        const int need = label->fontMetrics().horizontalAdvance(label->text());
        if (need > label->width() + 1 && label->width() > 0)
            out << QStringLiteral("label cut: '%1' %2 > %3 (%4)")
                       .arg(label->text().left(40))
                       .arg(need)
                       .arg(label->width())
                       .arg(label->objectName());
    }
    for (auto* view : root->findChildren<QAbstractItemView*>()) {
        if (!view->isVisible() || !view->model())
            continue;
        int checked = 0;
        const std::function<void(const QModelIndex&)> visit = [&](const QModelIndex& parent) {
            for (int r = 0; r < view->model()->rowCount(parent) && checked < 60; ++r) {
                const QModelIndex idx = view->model()->index(r, 0, parent);
                const QString text = idx.data(Qt::DisplayRole).toString();
                const QRect rect = view->visualRect(idx);
                if (!text.isEmpty() && rect.isValid() && !text.contains(QLatin1Char('\n'))) {
                    ++checked;
                    const int need = view->fontMetrics().horizontalAdvance(text);
                    if (need > rect.width() && rect.width() > 0 && rect.width() < 400)
                        out << QStringLiteral("row cut: '%1' %2 > %3 (%4 %5)")
                                   .arg(text.left(40))
                                   .arg(need)
                                   .arg(rect.width())
                                   .arg(view->metaObject()->className(), view->objectName());
                }
                auto* tree = qobject_cast<QTreeView*>(view);
                if (tree && tree->isExpanded(idx))
                    visit(idx);
            }
        };
        visit({});
    }
    return out;
}

} // namespace

class TestBugHunt12 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt12"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    /// Spec 89.1: the mouse back / forward buttons step through the timelines on every page that
    /// shows a timeline and over any of its widgets, and a quick second press (a double-click
    /// event) steps again.
    void spec89_1_backForwardWorkOnEveryTimelinePage()
    {
        Fixture f;
        f.window->resize(1366, 768);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.action("view.page_edit")->trigger();
        TimelineController* controller = f.window->editPage()->timeline()->controller();
        controller->setNamePrompt({});
        controller->setNewTimelinePrompt({}); // no dialogs
        const quint64 a = f.project.activeTimelineUid();
        f.action("timeline.new")->trigger();
        const quint64 b = f.project.activeTimelineUid();
        QVERIFY(a != b);
        f.action("timeline.new")->trigger();
        const quint64 c = f.project.activeTimelineUid();
        QVERIFY(b != c);
        const auto press = [](QWidget* over, QEvent::Type type, Qt::MouseButton button) {
            QMouseEvent e(type, QPointF(5, 5), over->mapToGlobal(QPointF(5, 5)), button, button,
                Qt::NoModifier);
            QCoreApplication::sendEvent(over, &e);
        };
        // Over the timeline viewer of the Edit page (not the timeline panel itself).
        press(f.window->editPage()->timelineViewer(), QEvent::MouseButtonPress, Qt::BackButton);
        QCOMPARE(f.project.activeTimelineUid(), b);
        // Quick repeated presses arrive as double-click events.
        press(f.window->editPage()->timelineViewer(), QEvent::MouseButtonDblClick, Qt::BackButton);
        QCOMPARE(f.project.activeTimelineUid(), a);
        // The Cut, Color and Compositing pages step too.
        for (const char* page : {"view.page_cut", "view.page_color", "view.page_compositing"}) {
            f.action(page)->trigger();
            QCoreApplication::processEvents();
            QWidget* current = f.window->page(f.window->currentPageId());
            QVERIFY(current);
            const quint64 before = f.project.activeTimelineUid();
            press(current, QEvent::MouseButtonPress, Qt::ForwardButton);
            QVERIFY2(f.project.activeTimelineUid() != before, page);
            press(current, QEvent::MouseButtonPress, Qt::BackButton);
            QCOMPARE(f.project.activeTimelineUid(), before);
        }
        // The Media Pool keeps the buttons for its bins: no timeline change there.
        f.action("view.page_edit")->trigger();
        const quint64 now = f.project.activeTimelineUid();
        press(f.window->editPage()->mediaPool(), QEvent::MouseButtonPress, Qt::ForwardButton);
        QCOMPARE(f.project.activeTimelineUid(), now);
    }

    /// Spec 89.2: in single viewer mode a click in the Edit Index brings the timeline into the
    /// one viewer (the click moves the timeline's playhead).
    void spec89_2_editIndexClickShowsTheTimelineInTheSingleViewer()
    {
        Fixture f;
        f.window->resize(1600, 900);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        EditPage* edit = f.window->editPage();
        edit->setPanelVisible(PanelId::EditIndex, true);
        edit->setSingleViewer(true);
        MediaItem item;
        item.path = QStringLiteral("/nonexistent/clip.mp4");
        item.name = QStringLiteral("clip.mp4");
        item.info.valid = true;
        item.info.hasVideo = true;
        item.info.durationUs = 10'000'000;
        item.info.width = 640;
        item.info.height = 360;
        const QString id = f.project.mediaPool()->addItem(item);
        emit edit->mediaPool()->openInSourceViewer(id);
        QVERIFY(edit->singleViewerShowsSource());
        QTest::mouseClick(edit->editIndex(), Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QVERIFY(!edit->singleViewerShowsSource());
        QVERIFY(!edit->timelineViewer()->isHidden());
    }

    /// Spec 89.3: right-click in the Effects library offers the double-click command in words.
    void spec89_3_libraryContextMenuHasAddToSelection()
    {
        Fixture f;
        EffectsLibraryPanel panel(f.ctx);
        for (const auto& [id, text] : {std::pair {"gaussian_blur", "Add to Selected Clips"},
                 std::pair {"wipe", "Add to Selected Edit Point"}}) {
            std::unique_ptr<QMenu> menu(panel.createContextMenu(QLatin1String(id)));
            QVERIFY(menu);
            QAction* apply = menu->findChild<QAction*>(QStringLiteral("ApplyLibraryEntry"));
            QVERIFY(apply);
            QCOMPARE(apply->text(), QLatin1String(text));
        }
    }

    /// Spec 89.4: a double-click on a render job card loads the job for editing (the pencil),
    /// and a right-click offers the card's commands.
    void spec89_4_renderJobCardDoubleClickEdits()
    {
        Fixture f;
        f.window->setCurrentPage(QStringLiteral("deliver"));
        DeliverPage* deliver = f.window->deliverPage();
        QVERIFY(deliver);
        QTemporaryDir dir;
        ExportSettings s = deliver->settingsPanel()->settings();
        s.outputPath = dir.filePath(QStringLiteral("first.mp4"));
        deliver->settingsPanel()->setSettings(s);
        const quint64 id = deliver->addCurrentToQueue(false);
        QVERIFY(id != 0);
        auto* card = deliver->findChild<QFrame*>(QStringLiteral("RenderJobCard"));
        QVERIFY(card);
        QCOMPARE(deliver->editedJob(), quint64(0));
        QMouseEvent e(QEvent::MouseButtonDblClick, QPointF(5, 5), card->mapToGlobal(QPointF(5, 5)),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(card, &e);
        QCOMPARE(deliver->editedJob(), id);
    }

    /// Spec 89.5: the LUT browser of the Color page has a search field; folders with no match
    /// give way.
    void spec89_5_lutBrowserSearch()
    {
        Fixture f;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const char* name : {"warm_film.cube", "cool_teal.cube", "warm_sunset.cube"}) {
            QFile cube(dir.filePath(QLatin1String(name)));
            QVERIFY(cube.open(QIODevice::WriteOnly));
            cube.write("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
        }
        f.window->setCurrentPage(QStringLiteral("color"));
        LutBrowser* luts = f.window->colorPage()->luts();
        luts->addFolder(dir.path());
        auto* search = luts->findChild<QLineEdit*>(QStringLiteral("LutSearch"));
        QVERIFY(search);
        QTreeWidget* tree = luts->tree();
        const auto visibleLuts = [tree] {
            int n = 0;
            for (int r = 0; r < tree->topLevelItemCount(); ++r) {
                QTreeWidgetItem* root = tree->topLevelItem(r);
                if (root->isHidden())
                    continue;
                for (int c = 0; c < root->childCount(); ++c) {
                    if (!root->child(c)->isHidden()
                        && root->child(c)->text(0).endsWith(QStringLiteral(".cube")))
                        ++n;
                }
            }
            return n;
        };
        const int all = visibleLuts();
        QVERIFY(all >= 3);
        search->setText(QStringLiteral("warm"));
        QCOMPARE(visibleLuts(), 2);
        search->setText(QStringLiteral("warm sun"));
        QCOMPARE(visibleLuts(), 1);
        search->setText(QStringLiteral("nothing like this"));
        QCOMPARE(visibleLuts(), 0);
        search->clear();
        QCOMPARE(visibleLuts(), all);
    }

    /// Spec 89.6: the duration / marks text in the program viewer's transport bar is shown whole
    /// or not at all (on 1366 px it was squeezed to "Du").
    void spec89_6_viewerMarksLabelNeverCut()
    {
        Fixture f;
        f.action("view.page_edit")->trigger();
        for (const QSize size : {QSize(1366, 768), QSize(1920, 1080), QSize(1100, 700)}) {
            f.window->resize(size);
            f.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
            QTest::qWait(100);
            auto* label = f.window->editPage()->timelineViewer()->findChild<QLabel*>(
                QStringLiteral("ViewerMarks"));
            QVERIFY(label);
            QVERIFY(!label->text().isEmpty());
            if (label->isVisible()) {
                QVERIFY2(label->width() >= label->sizeHint().width(),
                    qPrintable(QStringLiteral("%1: %2 < %3")
                            .arg(size.width())
                            .arg(label->width())
                            .arg(label->sizeHint().width())));
            }
        }
        // One wide viewer has room for it.
        f.window->resize(1920, 1080);
        f.window->editPage()->setSingleViewer(true);
        QTest::qWait(150);
        auto* label = f.window->editPage()->timelineViewer()->findChild<QLabel*>(
            QStringLiteral("ViewerMarks"));
        QVERIFY(label->isVisible());
        QVERIFY(label->width() >= label->sizeHint().width());
    }

    /// Spec 89.7: a render job with a very long name does not widen its card (or the queue).
    void spec89_7_longJobNameIsElided()
    {
        Fixture f;
        f.window->setCurrentPage(QStringLiteral("deliver"));
        DeliverPage* deliver = f.window->deliverPage();
        QTemporaryDir dir;
        ExportSettings s = deliver->settingsPanel()->settings();
        s.outputPath = dir.filePath(QString(240, QLatin1Char('x')) + QStringLiteral(".mp4"));
        deliver->settingsPanel()->setSettings(s);
        QVERIFY(deliver->addCurrentToQueue(false) != 0);
        auto* card = deliver->findChild<QFrame*>(QStringLiteral("RenderJobCard"));
        QVERIFY(card);
        QVERIFY2(card->minimumSizeHint().width() < 450,
            qPrintable(QString::number(card->minimumSizeHint().width())));
    }

    /// Spec 89.9: a double-click on a mixer strip's name renames its track (buses already had
    /// Rename in the menu); one undo step "Rename Track".
    void spec89_9_mixerStripNameDoubleClickRenames()
    {
        Fixture f;
        f.window->resize(1800, 1000);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.action("view.page_audio")->trigger();
        const quint64 a1 = f.project.timeline()->data().audioTracks[0].id;
        MixerStrip* strip = f.window->audioPage()->mixer()->strip(MixTarget::track(a1));
        QVERIFY(strip);
        QString offered;
        strip->setNamePrompt([&offered](const QString& current) -> std::optional<QString> {
            offered = current;
            return QStringLiteral("  Dialogue ");
        });
        auto* name = strip->findChildren<QLabel*>().last();
        // The name plate is the last label of the strip.
        const int steps = f.project.undoStack()->count();
        QMouseEvent e(QEvent::MouseButtonDblClick, QPointF(5, 5), name->mapToGlobal(QPointF(5, 5)),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(name, &e);
        QCOMPARE(offered, QStringLiteral("A1"));
        QCOMPARE(f.project.timeline()->data().audioTracks[0].name, QStringLiteral("Dialogue"));
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Rename Track"));
        // Cancelling changes nothing.
        strip->setNamePrompt([](const QString&) -> std::optional<QString> { return std::nullopt; });
        QCoreApplication::sendEvent(name, &e);
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
    }

    void probeTabOrder()
    {
        Fixture f;
        QList<QWidget*> dialogs;
        QTemporaryDir dir;
        auto* prefs = new PreferencesDialog(nullptr);
        dialogs << prefs;
        dialogs << new ProjectSettingsDialog(&f.project);
        dialogs << new NewProjectDialog(QStringLiteral("My project"));
        dialogs << new NormalizeAudioDialog(&f.project, {});
        for (QWidget* d : dialogs) {
            d->show();
            QVERIFY(QTest::qWaitForWindowExposed(d));
            for (const QString& p : tabOrderProblems(d))
                qWarning("TABPROBE %s: %s", d->metaObject()->className(), qPrintable(p));
            if (auto* list = d->findChild<QListWidget*>()) {
                for (int row = 0; row < list->count(); ++row) {
                    list->setCurrentRow(row);
                    QTest::qWait(30);
                    for (const QString& p : tabOrderProblems(d))
                        qWarning("TABPROBE %s page %s: %s", d->metaObject()->className(),
                            qPrintable(list->item(row)->text()), qPrintable(p));
                }
            }
        }
        qDeleteAll(dialogs);
        f.window->resize(1600, 900);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        for (const char* page : {"view.page_edit", "view.page_color", "view.page_compositing",
                 "view.page_deliver", "view.page_audio"}) {
            f.action(page)->trigger();
            QTest::qWait(100);
            for (const QString& p : tabOrderProblems(f.window.get(), 60))
                qWarning("TABPROBE %s: %s", page, qPrintable(p));
        }
    }

    void probeKeyStealing()
    {
        Fixture f;
        f.window->resize(1600, 900);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        using K = QKeyCombination;
        const QList<QKeyCombination> keys {K(Qt::ControlModifier, Qt::Key_Z),
            K(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Z),
            K(Qt::ControlModifier, Qt::Key_S), K(Qt::ControlModifier, Qt::Key_C),
            K(Qt::ControlModifier, Qt::Key_V), K(Qt::ControlModifier, Qt::Key_X),
            K(Qt::ControlModifier, Qt::Key_A), K(Qt::Key_Escape),
            K(Qt::ShiftModifier, Qt::Key_Delete), K(Qt::ShiftModifier, Qt::Key_Left),
            K(Qt::ControlModifier, Qt::Key_B), K(Qt::Key_Up), K(Qt::Key_Down), K(Qt::Key_Return),
            K(Qt::Key_F2), K(Qt::Key_Q), K(Qt::Key_W), K(Qt::Key_E), K(Qt::Key_N)};
        for (const char* page :
            {"view.page_media", "view.page_cut", "view.page_edit", "view.page_compositing",
                "view.page_color", "view.page_audio", "view.page_deliver"}) {
            f.action(page)->trigger();
            QTest::qWait(100);
            for (QWidget* w : f.window->findChildren<QWidget*>()) {
                if (!w->isVisible() || w->focusPolicy() == Qt::NoFocus || !w->isEnabled())
                    continue;
                if (qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w)
                    || qobject_cast<QTextEdit*>(w) || qobject_cast<QPlainTextEdit*>(w)
                    || qobject_cast<QComboBox*>(w))
                    continue;
                QStringList stolen;
                for (const QKeyCombination& kc : keys) {
                    QKeyEvent e(QEvent::ShortcutOverride, kc.key(), kc.keyboardModifiers());
                    e.ignore();
                    w->setFocus();
                    QCoreApplication::sendEvent(w, &e);
                    if (e.isAccepted())
                        stolen << QKeySequence(kc).toString();
                }
                if (!stolen.isEmpty())
                    qWarning("STEALPROBE %s: %s '%s' takes %s", page, w->metaObject()->className(),
                        qPrintable(w->objectName()), qPrintable(stolen.join(' ')));
            }
        }
    }

    void probeKeyConflicts()
    {
        Fixture f;
        QHash<QString, QStringList> byKey;
        for (const QString& id : f.actions.ids())
            for (const QKeySequence& k : f.actions.shortcuts(id))
                byKey[k.toString()]
                    << id + QLatin1Char('[') + f.actions.shortcutScope(id) + QLatin1Char(']');
        for (auto it = byKey.cbegin(); it != byKey.cend(); ++it)
            if (it.value().size() > 1)
                qWarning("KEYPROBE %s: %s", qPrintable(it.key()), qPrintable(it.value().join(' ')));
    }

    void probeLayouts()
    {
        Fixture f;
        f.window->resize(1366, 768);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        for (const char* page :
            {"view.page_media", "view.page_cut", "view.page_edit", "view.page_compositing",
                "view.page_color", "view.page_audio", "view.page_deliver"}) {
            f.action(page)->trigger();
            QTest::qWait(150);
            QCoreApplication::processEvents();
            if (Page* pg = qEnvironmentVariableIsSet("KK_PROBE_ALL")
                    ? f.window->page(f.window->currentPageId())
                    : nullptr) {
                for (PanelId id :
                    {PanelId::MediaPool, PanelId::Effects, PanelId::EditIndex, PanelId::Mixer,
                        PanelId::Metadata, PanelId::Inspector, PanelId::Keyframes, PanelId::Nodes,
                        PanelId::Spline, PanelId::Gallery, PanelId::Clips, PanelId::ColorTimeline})
                    if (pg->hasPanel(id) && !pg->isPanelVisible(id)
                        && !(id == PanelId::Keyframes && qstrcmp(page, "view.page_edit") == 0))
                        pg->setPanelVisible(id, true);
            }
            QTest::qWait(150);
            for (const QString& p : textProblems(f.window.get()))
                qWarning("TEXTPROBE %s: %s", page, qPrintable(p));
            for (const QString& p : layoutProblems(f.window.get()))
                qWarning("PROBE %s: %s", page, qPrintable(p));
        }
    }
};

QTEST_MAIN(TestBugHunt12)
#include "test_bughunt12.moc"
