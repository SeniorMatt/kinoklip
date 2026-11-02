// SPDX-License-Identifier: GPL-3.0-or-later
// Colour schemes for custom-painted widgets: Kinoklip is the default scheme (found in every
// build), SchemeColors follows the active scheme's roles, and the timeline actually repaints
// in the new colours after a switch (lanes, ruler, clip fill, playhead), keyframe colours too.
#include "../media/MediaTestUtil.h"
#include "app/ColorSchemeFile.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/keyframes/CurveView.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <memory>

using namespace kk;
using kk::theme::ColorSchemeFile;

namespace {

struct App
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    SettingsPageRegistry settingsPages;
    std::unique_ptr<PlaybackEngine> playback;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    App()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        theme::registerActions(actions);
        registerStandardSettingsPages(settingsPages);
        theme::registerSettingsPage(settingsPages, actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        playback = std::make_unique<PlaybackEngine>(std::move(options));
        playback->setMediaPool(project.mediaPool());
        playback->setTimeline(project.timeline());
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.settingsPages = &settingsPages;
        ctx.playback = playback.get();
        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, wo);
    }
};

/// The .colors file the theme uses for @p id: installed copies win over the compiled-in ones
/// (KColorSchemeManager and the fallback reader agree on that).
ColorSchemeFile schemeFile(const QString& id)
{
    const QString name = id + QStringLiteral(".colors");
    const QString installed = QStandardPaths::locate(
        QStandardPaths::GenericDataLocation, QStringLiteral("color-schemes/") + name);
    ColorSchemeFile file;
    file.load(installed.isEmpty() ? QStringLiteral(":/color-schemes/") + name : installed);
    return file;
}

QColor viewRole(const ColorSchemeFile& file, const char* key)
{
    return file.color(QStringLiteral("View"), QLatin1String(key));
}

/// Pixels of @p image with exactly colour @p c (alpha ignored).
int countPixels(const QImage& image, const QColor& c)
{
    const QRgb want = c.rgb() & 0xffffff;
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            n += (line[x] & 0xffffff) == want ? 1 : 0;
    }
    return n;
}

/// The colours sampled from the helper for one scheme.
struct Sample
{
    QColor lane, ruler, clip, playhead, keyframe, curve0, audio;
};

Sample sample()
{
    const SchemeColors& sc = schemeColors();
    return {sc.lane, sc.ruler, sc.videoClip, sc.playhead, sc.keyframe, CurveView::curveColor(0),
        sc.audioClip};
}

} // namespace

class TestSchemeColors : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_schemecolors"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void defaultSchemeIsKinoklip()
    {
        QCOMPARE(QLatin1String(theme::kDefaultColorScheme), QLatin1String("Kinoklip"));
        // No stored choice: the default scheme is active, in this build mode (KF6 or not).
        QVERIFY(!QSettings().contains(QLatin1String(prefs::kColorScheme)));
        QCOMPARE(theme::currentColorScheme(), QStringLiteral("Kinoklip"));
        int listed = 0;
        for (const theme::ColorSchemeInfo& s : theme::availableColorSchemes())
            listed += s.id == QLatin1String("Kinoklip") ? 1 : 0;
        QCOMPARE(listed, 1);
        const ColorSchemeFile kinoklip = schemeFile(QStringLiteral("Kinoklip"));
        QCOMPARE(QApplication::palette().color(QPalette::Window),
            kinoklip.color(QStringLiteral("Window"), QStringLiteral("BackgroundNormal")));
    }

    void explicitChoiceIsKept()
    {
        QSettings().setValue(QLatin1String(prefs::kColorScheme), QStringLiteral("BreezeLight"));
        theme::apply(*qApp);
        QCOMPARE(theme::currentColorScheme(), QStringLiteral("BreezeLight"));
        QSettings().remove(QLatin1String(prefs::kColorScheme));
        theme::apply(*qApp);
        QCOMPARE(theme::currentColorScheme(), QStringLiteral("Kinoklip"));
    }

    void helperFollowsSchemeRoles()
    {
        QSignalSpy changed(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed);
        for (const QString id : {QStringLiteral("Kinoklip"), QStringLiteral("BreezeLight"),
                 QStringLiteral("BreezeDark")}) {
            theme::setColorScheme(id);
            QCOMPARE(theme::currentColorScheme(), id);
            const ColorSchemeFile file = schemeFile(id);
            const SchemeColors& sc = schemeColors();
            QCOMPARE(sc.view, viewRole(file, "BackgroundNormal"));
            QCOMPARE(sc.link, viewRole(file, "ForegroundLink"));
            QCOMPARE(sc.positive, viewRole(file, "ForegroundPositive"));
            QCOMPARE(sc.neutral, viewRole(file, "ForegroundNeutral"));
            QCOMPARE(sc.negative, viewRole(file, "ForegroundNegative"));
            QCOMPARE(sc.visited, viewRole(file, "ForegroundVisited"));
            QCOMPARE(sc.focus, viewRole(file, "DecorationFocus"));
            QCOMPARE(sc.playhead, sc.negative);
            QCOMPARE(sc.selection, sc.neutral);
            QCOMPARE(sc.dark, id != QLatin1String("BreezeLight"));
            // Clip names stay readable on the clip fills.
            for (const QColor& fill : {sc.videoClip, sc.audioClip, sc.titleClip})
                QVERIFY2(std::abs(sc.textOn(fill).lightness() - fill.lightness()) > 70,
                    qPrintable(id + QLatin1Char(' ') + fill.name()));
        }
        QCOMPARE(changed.count(), 3);
        theme::setColorScheme(QStringLiteral("Kinoklip"));
    }

    void timelineRepaintsOnSwitch()
    {
        App app;
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        const ImportResult r = app.project.importMedia({test::fixture("h264_25fps.mp4")});
        QCOMPARE(r.importedIds.size(), 1);
        app.window->resize(1400, 900);
        app.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(app.window.get()));
        auto* panel = app.window->findChild<TimelinePanel*>();
        QVERIFY(panel);
        app.project.timeline()->setPlayhead(0);
        QVERIFY(!panel->controller()
                     ->insertSource(r.importedIds.first(), 0, 4'000'000, SourceEditMode::Overwrite)
                     .isEmpty());
        app.project.timeline()->setPlayhead(30);
        TimelineWidget* timeline = panel->timelineWidget();
        QCoreApplication::processEvents();

        const auto switchTo = [&](const QString& id) {
            app.actions.action(theme::colorSchemeActionId(id))->trigger();
            QCoreApplication::processEvents();
            QCOMPARE(theme::currentColorScheme(), id);
        };
        switchTo(QStringLiteral("Kinoklip"));
        const Sample kinoklip = sample();
        const QImage before = timeline->grab().toImage();
        switchTo(QStringLiteral("BreezeLight"));
        const Sample light = sample();
        const QImage after = timeline->grab().toImage();

        // Every sampled role differs between the two schemes...
        QVERIFY(kinoklip.lane != light.lane);
        QVERIFY(kinoklip.ruler != light.ruler);
        QVERIFY(kinoklip.clip != light.clip);
        QVERIFY(kinoklip.playhead != light.playhead);
        QVERIFY(kinoklip.keyframe != light.keyframe);
        QVERIFY(kinoklip.curve0 != light.curve0);
        QVERIFY(kinoklip.audio != light.audio);
        // ...and the timeline paints with the active scheme's colours only.
        struct Check
        {
            const char* what;
            QColor old, now;
            int minPixels;
        };
        for (const Check& c : {Check {"lane", kinoklip.lane, light.lane, 500},
                 Check {"ruler", kinoklip.ruler, light.ruler, 500},
                 Check {"video clip", kinoklip.clip, light.clip, 200},
                 Check {"audio clip", kinoklip.audio, light.audio, 200},
                 Check {"playhead", kinoklip.playhead, light.playhead, 20}}) {
            QVERIFY2(countPixels(before, c.old) >= c.minPixels,
                qPrintable(QStringLiteral("Kinoklip %1 %2 not painted (%3 px)")
                               .arg(QLatin1String(c.what), c.old.name())
                               .arg(countPixels(before, c.old))));
            QVERIFY2(countPixels(after, c.now) >= c.minPixels,
                qPrintable(QStringLiteral("BreezeLight %1 %2 not painted (%3 px)")
                               .arg(QLatin1String(c.what), c.now.name())
                               .arg(countPixels(after, c.now))));
            QVERIFY2(countPixels(after, c.old) < c.minPixels / 4,
                qPrintable(QStringLiteral("%1: old colour %2 still painted after the switch")
                               .arg(QLatin1String(c.what), c.old.name())));
        }
        switchTo(QStringLiteral("Kinoklip"));
    }
};

QTEST_MAIN(TestSchemeColors)
#include "test_schemecolors.moc"
