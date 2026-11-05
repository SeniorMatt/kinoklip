// SPDX-License-Identifier: GPL-3.0-or-later
// Text in parts the user cannot resize never runs off its widget: it is elided with "..." and
// shown in full as the tooltip, or the widget grows to fit it (spec 23.21 - 23.23).
#include "app/Theme.h"
#include "ui/common/Metrics.h"
#include "ui/common/TextFit.h"
#include "ui/deliver/RenderPresetRow.h"
#include "ui/inspector/ValueField.h"

#include <QApplication>
#include <QHelpEvent>
#include <QLabel>
#include <QListWidget>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

using namespace kk;

namespace {

const QChar kEllipsis(0x2026);
constexpr int kStripWidth = 92; ///< like a mixer strip

/// Each line of @p text fits @p width with @p metrics.
bool linesFit(const QString& text, const QFontMetrics& metrics, int width)
{
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (metrics.horizontalAdvance(line) > width)
            return false;
    }
    return true;
}

} // namespace

class TestTextFit : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_textfit"));
        theme::apply(*qApp);
    }

    void spec23_21_cappedListElidesWithFullTextTooltip()
    {
        QListWidget list;
        list.setFixedSize(150, 200);
        textfit::elideListItems(&list);
        const QString longName = QStringLiteral("Gradient 4 (inverted, with a long translation)");
        auto* item = new QListWidgetItem(longName, &list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        new QListWidgetItem(QStringLiteral("Circle 1"), &list);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        QCoreApplication::processEvents();
        QVERIFY(!list.horizontalScrollBar()->isVisible());
        QCOMPARE(list.horizontalScrollBar()->maximum(), 0);
        const QRect r = list.visualItemRect(item);
        QVERIFY(r.left() >= 0 && r.right() < list.viewport()->width());
        // The full name shows as the tooltip of the elided item, not of a short one.
        QHelpEvent help(QEvent::ToolTip, r.center(), list.viewport()->mapToGlobal(r.center()));
        QApplication::sendEvent(list.viewport(), &help);
        QTRY_COMPARE(QToolTip::text(), longName);
        QToolTip::hideText();
        QTRY_VERIFY(!QToolTip::isVisible());
        const QRect shortRect = list.visualItemRect(list.item(1));
        QHelpEvent shortHelp(
            QEvent::ToolTip, shortRect.center(), list.viewport()->mapToGlobal(shortRect.center()));
        QApplication::sendEvent(list.viewport(), &shortHelp);
        QVERIFY(!QToolTip::isVisible());
    }

    void spec23_22_fixedWidthLabelsAndButtonsElide()
    {
        QWidget strip;
        strip.setFixedWidth(kStripWidth);
        auto* layout = new QVBoxLayout(&strip);
        auto* label = new QLabel;
        label->setAlignment(Qt::AlignCenter);
        auto* button = new QToolButton;
        button->setPopupMode(QToolButton::InstantPopup);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout->addWidget(label);
        layout->addWidget(button);
        const QString name = QStringLiteral("Dialogue Microphone Left\nStereo");
        const QString bus = QStringLiteral("Music and Effects Submix");
        textfit::setElidedText(label, name, QStringLiteral("Right-click for the format"));
        textfit::setElidedText(button, bus, QStringLiteral("Bus assignment"));
        strip.show();
        QVERIFY(QTest::qWaitForWindowExposed(&strip));
        QCoreApplication::processEvents();

        auto verifyFits = [&] {
            QVERIFY(label->text().contains(kEllipsis));
            QVERIFY(label->text().endsWith(QStringLiteral("\nStereo"))); // short line kept
            QVERIFY(linesFit(label->text(), label->fontMetrics(), label->contentsRect().width()));
            QCOMPARE(label->toolTip(), name + QStringLiteral("\nRight-click for the format"));
            QVERIFY(button->text().contains(kEllipsis));
            QVERIFY(button->sizeHint().width() <= button->width());
            QCOMPARE(button->toolTip(), bus + QStringLiteral("\nBus assignment"));
        };
        verifyFits();

        // A larger font elides again to the same room.
        QFont larger = strip.font();
        larger.setPointSizeF(larger.pointSizeF() * 1.5);
        strip.setFont(larger);
        QCoreApplication::processEvents();
        verifyFits();

        // Short texts stay whole, with the plain tooltip.
        textfit::setElidedText(label, QStringLiteral("A1\nMono"), QStringLiteral("Tip"));
        textfit::setElidedText(button, QStringLiteral("Bus 1"), QStringLiteral("Bus assignment"));
        QCOMPARE(label->text(), QStringLiteral("A1\nMono"));
        QCOMPARE(label->toolTip(), QStringLiteral("Tip"));
        QCOMPARE(button->text(), QStringLiteral("Bus 1"));
        QCOMPARE(button->toolTip(), QStringLiteral("Bus assignment"));
    }

    void spec23_23_valueFieldsFitTheirWidestValue()
    {
        ValueField field;
        field.setRange(-100000.0, 10.0); // the minimum is the longer text
        field.setDecimals(2);
        field.setSuffix(QStringLiteral(" px"));
        const QFontMetrics metrics(field.font());
        QVERIFY(field.sizeHint().width()
            > metrics.horizontalAdvance(field.textForValue(field.minimum())));
    }

    void spec23_23_presetTileNamesElidePerLine()
    {
        RenderPresetRow row;
        const QString name = QStringLiteral("Hochgeschwindigkeitsvoreinstellung Archivierung");
        row.addPreset(QStringLiteral("long"), name, QIcon(), QString());
        QToolButton* tile = row.tile(QStringLiteral("long"));
        QVERIFY(tile);
        QVERIFY(tile->text().contains(kEllipsis));
        QVERIFY(linesFit(tile->text(), tile->fontMetrics(), tile->width() - 2 * metrics::kSpaceS));
        QVERIFY(tile->toolTip().startsWith(name));
    }
};

QTEST_MAIN(TestTextFit)
#include "test_textfit.moc"
