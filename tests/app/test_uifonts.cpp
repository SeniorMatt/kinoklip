// SPDX-License-Identifier: GPL-3.0-or-later
// Interface fonts by role (spec 142): the bundled Open Sans, the Resolve and System choices,
// switching back restores the original application font exactly, the stored choice, the
// style sheet rules and tabular digits for the timecodes.
#include "core/Preferences.h"
#include "ui/common/Resources.h"
#include "ui/common/UiFonts.h"

#include <QApplication>
#include <QFontDatabase>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QLabel>
#include <QTreeWidget>
#include <QSettings>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTest>

using namespace kk;
using namespace kk::uifonts;

class UiFontsTest : public QObject
{
    Q_OBJECT
    QFont m_original;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        initResources();
        m_original = QApplication::font();
        QCOMPARE(systemFont(), m_original); // captured before anything changes it
        loadBundledFonts();
    }

    void desktopFontParsing()
    {
        const QFont k = parseKdeFont(QStringLiteral("Noto Sans,11,-1,5,400,0,0,0,0,0,0,0,0,0,0,1"));
        QCOMPARE(k.family(), QStringLiteral("Noto Sans"));
        QCOMPARE(k.pointSizeF(), 11.0);
        QVERIFY(parseKdeFont(QString()).family().isEmpty());
        const QFont g = parseGnomeFont(QStringLiteral("'Inter Variable Semi-Bold 12'\n"));
        QCOMPARE(g.family(), QStringLiteral("Inter Variable"));
        QCOMPARE(g.pointSizeF(), 12.0);
        QCOMPARE(g.weight(), QFont::DemiBold);
        QCOMPARE(parseGnomeFont(QStringLiteral("'Cantarell 11'")).family(), QStringLiteral("Cantarell"));
        QVERIFY(parseGnomeFont(QStringLiteral("garbage")).family().isEmpty());
        QVERIFY(!desktopFont().family().isEmpty());
    }

    void systemChoiceKeepsTheBaseFont()
    {
        const QFont base(QStringLiteral("DejaVu Sans"), 11);
        const QFont f = font(Role::Body, Choice::System, base);
        QCOMPARE(f.family(), base.family());
        QCOMPARE(f.pointSizeF(), 11.0);
    }

    void bundledOpenSansIsRegistered()
    {
        QVERIFY(QFontDatabase::families().contains(QLatin1String(kFamily)));
        const QStringList styles = QFontDatabase::styles(QLatin1String(kFamily));
        QVERIFY2(styles.contains(QStringLiteral("Regular")), qPrintable(styles.join(',')));
        QVERIFY(styles.contains(QStringLiteral("Italic")));
        QVERIFY(styles.contains(QStringLiteral("Bold")));
        QVERIFY(styles.contains(QStringLiteral("Medium")));
        QVERIFY(styles.contains(QStringLiteral("SemiBold")));
    }

    void resolveChoiceGivesOpenSansPerRole()
    {
        const QFont base;
        for (Role role : {Role::Body, Role::PanelTitle, Role::SectionHeader, Role::Small,
                 Role::Timecode, Role::ViewerTimecode}) {
            const QFont f = font(role, Choice::Resolve, base);
            QCOMPARE(f.family(), QString::fromLatin1(kFamily));
            QCOMPARE(QFontInfo(f).family(), QString::fromLatin1(kFamily)); // really found
        }
        QCOMPARE(font(Role::Body, Choice::Resolve, base).pointSizeF(), 9.0);
        QCOMPARE(font(Role::Body, Choice::Resolve, base).weight(), QFont::Medium);
        QCOMPARE(font(Role::PanelTitle, Choice::Resolve, base).weight(), QFont::Medium);
        QCOMPARE(font(Role::PanelTitle, Choice::Resolve, base).pointSizeF(), 9.75);
        QCOMPARE(font(Role::Small, Choice::Resolve, base).pointSizeF(), 8.25);
        QCOMPARE(font(Role::Ruler, Choice::Resolve, base).pointSizeF(), 7.5); // 10 px
        QCOMPARE(font(Role::Body, Choice::Resolve, base).hintingPreference(), QFont::PreferDefaultHinting);
        QCOMPARE(font(Role::Ruler, Choice::System, QFont()).pointSizeF(),
            std::max(6.0, QFont().pointSizeF() - 1.5)); // as the small font before
        QCOMPARE(font(Role::Timecode, Choice::Resolve, base).weight(), QFont::Medium);
        QCOMPARE(font(Role::Timecode, Choice::Resolve, base).pointSizeF(), 15.75);
        // Mono is a real monospace in both choices.
        QCOMPARE(font(Role::Mono, Choice::Resolve, base), font(Role::Mono, Choice::System, base));
    }

    void systemChoiceIsTheOldFonts()
    {
        QFont base(QStringLiteral("Sans Serif"));
        base.setPointSizeF(10);
        QCOMPARE(font(Role::Body, Choice::System, base), base);
        QCOMPARE(font(Role::SectionHeader, Choice::System, base), base);
        QFont title = base;
        title.setWeight(QFont::DemiBold);
        QCOMPARE(font(Role::PanelTitle, Choice::System, base), title);
        QCOMPARE(font(Role::Small, Choice::System, base).pointSizeF(), 8.5); // font - 1.5
        const QFont tc = font(Role::Timecode, Choice::System, base);
        QVERIFY(tc.bold());
        QCOMPARE(tc.pointSizeF(), 12.0); // font + 2
        QCOMPARE(tc.family(), base.family());
        const QFont viewer = font(Role::ViewerTimecode, Choice::System, base);
        QCOMPARE(viewer.family(), QStringLiteral("monospace"));
        QCOMPARE(viewer.weight(), QFont::DemiBold);
    }

    void timecodeDigitsDoNotJump()
    {
        for (Role role : {Role::Timecode, Role::ViewerTimecode, Role::Body}) {
            const QFontMetricsF fm(font(role, Choice::Resolve, QFont()));
            const qreal zero = fm.horizontalAdvance(QLatin1Char('0'));
            for (QChar d : QStringLiteral("123456789"))
                QCOMPARE(fm.horizontalAdvance(d), zero);
        }
    }

    void choiceIsStoredAndApplied()
    {
        QCOMPARE(choice(), Choice::Resolve); // default
        int changed = 0;
        setChangedHandler([&changed] { ++changed; });

        setChoice(Choice::System);
        QCOMPARE(QSettings().value(QLatin1String(prefs::kInterfaceFont)).toString(),
            QStringLiteral("system"));
        QCOMPARE(choice(), Choice::System);
        QCOMPARE(changed, 1);
        setChoice(Choice::System); // no change, no call
        QCOMPARE(changed, 1);
        QVERIFY(styleSheetRules().contains(QLatin1String("font-family: monospace")));

        setChoice(Choice::Resolve);
        QCOMPARE(changed, 2);
        QVERIFY(!styleSheetRules().contains(QLatin1String("monospace")));
        QVERIFY(styleSheetRules().contains(QLatin1String("#TimecodeLabel")));
        setChangedHandler({});
    }

    void switchingBackRestoresTheApplicationFont()
    {
        setChoice(Choice::Resolve);
        apply(*qApp);
        QCOMPARE(QApplication::font().family(), QString::fromLatin1(kFamily));
        QCOMPARE(QApplication::font().pointSizeF(), 9.0);
        setChoice(Choice::System);
        apply(*qApp);
        QCOMPARE(QApplication::font(), m_original);
        setChoice(Choice::Resolve);
    }

    void sameRoleSameFontInEveryWidget()
    {
        // Widgets of different pages start from different fonts; a role gives one font.
        QLabel label;
        QTreeWidget tree;
        QFont odd(QStringLiteral("Courier"));
        odd.setPointSizeF(13);
        for (Role role : {Role::Body, Role::Emphasis, Role::PanelTitle, Role::Heading, Role::Title,
                 Role::Ruler, Role::Small, Role::SmallEmphasis, Role::Tiny, Role::Timecode, Role::ViewerTimecode}) {
            const QFont a = font(role, Choice::Resolve, label.font());
            QCOMPARE(font(role, Choice::Resolve, tree.font()), a);
            QCOMPARE(font(role, Choice::Resolve, odd), a);
            QCOMPARE(QFontInfo(a).family(), QString::fromLatin1(kFamily));
        }
        // Emphasis is bold, PanelTitle semi-bold, Body regular: one weight each.
        QCOMPARE(font(Role::Emphasis, Choice::Resolve, odd).weight(), QFont::Bold);
        QVERIFY(font(Role::Heading, Choice::Resolve, odd).pointSizeF()
            > font(Role::Body, Choice::Resolve, odd).pointSizeF());
        QVERIFY(font(Role::Body, Choice::Resolve, odd).pointSizeF()
            > font(Role::Small, Choice::Resolve, odd).pointSizeF());
        // A zoomed painter scales the role's size, not its family.
        QCOMPARE(font(Role::Body, odd, 2.0).family() == font(Role::Body, odd).family(), true);
    }

    void noWidgetCodeSetsItsOwnFont()
    {
        // Source scan: family, size, weight and bold are set in UiFonts only (title and caption
        // text drawn into the picture, the font box and the adaptive welcome strip excepted).
        const QDir src(QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath(QStringLiteral("../../src")));
        if (!src.exists())
            QSKIP("sources not available");
        const QRegularExpression bad(QStringLiteral(
            "setPointSize|setPixelSize|setBold\\(|setWeight\\(|font-family|font-size|font-weight|FixedFont"));
        QStringList offenders;
        QDirIterator it(src.absolutePath(), {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
            QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString rel = src.relativeFilePath(path);
            if (rel.startsWith(QLatin1String("media/")) || rel.startsWith(QLatin1String("playback/"))
                || rel.startsWith(QLatin1String("render/")) || rel.endsWith(QLatin1String("UiFonts.cpp"))
                || rel.endsWith(QLatin1String("WelcomeHeroes.cpp")))
                continue;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            int line = 0;
            while (!f.atEnd()) {
                const QString text = QString::fromUtf8(f.readLine());
                ++line;
                if (text.trimmed().startsWith(QLatin1String("//")))
                    continue;
                if (bad.match(text).hasMatch())
                    offenders << QStringLiteral("%1:%2").arg(rel).arg(line);
            }
        }
        QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QLatin1Char(' '))));
    }

    void sessionOverrideWinsAndIsNotStored()
    {
        setChoice(Choice::Resolve);
        setSessionOverride(QStringLiteral("System"));
        QCOMPARE(choice(), Choice::System);
        QCOMPARE(QSettings().value(QLatin1String(prefs::kInterfaceFont)).toString(),
            QStringLiteral("resolve"));
        setSessionOverride(QStringLiteral("nonsense"));
        QCOMPARE(choice(), Choice::Resolve);
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_uifonts"));
    QApplication app(argc, argv);
    UiFontsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_uifonts.moc"
