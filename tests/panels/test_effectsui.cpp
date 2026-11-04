// SPDX-License-Identifier: GPL-3.0-or-later
// Edit page effects UI (docs/behavior-spec.md section 40): the Effects library folders, applying
// a keyer from the library, the Inspector's eyedropper button and the timeline viewer's
// ColorPickOverlay (click and drag picks, one undo step, Esc, the picture before the key), the
// HSL Key's hue eyedropper, the Curves graph rows and the File rows.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/CurveParam.h"
#include "core/EffectRegistry.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/CurveEditor.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/viewer/ColorPickOverlay.h"
#include "ui/viewer/ViewerSurface.h"

#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <memory>

using namespace kk;
using namespace kk::test;

namespace {

const TrackRef kV1 {TrackType::Video, 0};

/// A 320 x 180 timeline with a solid clip carrying a chroma key, a raster viewer surface of
/// 656 x 376 px (picture 640 x 360 in Fit: 2 widget px per frame px) and the eyedropper.
/// The fake frame source paints the left half of the frame (0, 200, 80) and the right half
/// (40, 100, 220), and remembers the timeline it was given.
struct PickFixture
{
    PanelFixture f;
    std::unique_ptr<QWidget> host;
    ViewerSurface* surface = nullptr;
    ColorPickOverlay* overlay = nullptr;
    quint64 clipId = 0;
    quint64 keyId = 0;
    std::shared_ptr<const TimelineData> rendered;

    PickFixture()
    {
        f.project.timeline()->mutableData()
            = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1}, 320, 180, 2, 2);
        clipId = f.addClip(kV1, 0, 100, ClipKind::Solid);
        QVERIFY(effectactions::applyEffect(f.project, {clipId}, QLatin1String(fxid::ChromaKey)));
        QVERIFY(effectactions::applyEffect(f.project, {clipId}, QLatin1String(fxid::Invert)));
        keyId = f.clip(clipId)->effects[0].id;
        f.selection.selectClip(clipId);
        host = std::make_unique<QWidget>();
        surface = ViewerSurface::create(host.get());
        surface->setAspect(QSize(320, 180));
        surface->widget()->setGeometry(0, 0, 656, 376);
        overlay = new ColorPickOverlay(f.ctx, surface);
        overlay->setFrameSource([this](std::shared_ptr<const TimelineData> tl, int64_t) {
            rendered = tl;
            QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor(40, 100, 220));
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 160; ++x)
                    image.setPixel(x, y, qRgb(0, 200, 80));
            return image;
        });
        host->resize(656, 376);
        host->show();
        QCoreApplication::processEvents();
    }
    /// Widget position of frame pixel (x, y).
    QPoint at(int x, int y) const
    {
        const QRect pic = surface->pictureRect();
        return QPoint(pic.x() + x * pic.width() / 320 + 1, pic.y() + y * pic.height() / 180 + 1);
    }
    const EffectInstance& key() const { return f.clip(clipId)->effects[0]; }
    int undoCount() const { return f.project.undoStack()->count(); }
};

} // namespace

class TestEffectsUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    // Spec 40.1: video effects sit in folders in the library and the Add Effect menu.
    void libraryFolders()
    {
        PanelFixture f;
        EffectsLibraryPanel panel(f.ctx);
        QTreeWidgetItem* video = nullptr;
        for (int i = 0; i < panel.tree()->topLevelItemCount(); ++i) {
            if (panel.tree()->topLevelItem(i)->text(0) == QLatin1String("Video Effects"))
                video = panel.tree()->topLevelItem(i);
        }
        QVERIFY(video);
        QStringList folders;
        for (int i = 0; i < video->childCount(); ++i)
            folders.append(video->child(i)->text(0));
        QCOMPARE(folders,
            QStringList({QStringLiteral("Blur"), QStringLiteral("Color"), QStringLiteral("Distort"),
                QStringLiteral("Key"), QStringLiteral("Light"), QStringLiteral("Stylize"),
                QStringLiteral("Time"), QStringLiteral("Transform")}));
        QTreeWidgetItem* key = video->child(3);
        QStringList keyers;
        for (int i = 0; i < key->childCount(); ++i)
            keyers.append(key->child(i)->text(0));
        QCOMPARE(keyers,
            QStringList({QStringLiteral("Chroma Key"), QStringLiteral("Luma Key"),
                QStringLiteral("HSL Key")}));
        // Every video effect is listed exactly once; searching hides empty folders.
        const QStringList all = panel.visibleDescriptorIds();
        for (const EffectDescriptor* d :
            EffectRegistry::instance().byCategory(EffectCategory::VideoEffect))
            QCOMPARE(all.count(d->id), 1);
        panel.setFilterText(QStringLiteral("chroma"));
        QCOMPARE(panel.visibleDescriptorIds(),
            QStringList(
                {QLatin1String(fxid::ChromaKey), QLatin1String(fxid::ChromaticAberration)}));
        QVERIFY(!key->isHidden());
        QVERIFY(video->child(0)->isHidden()); // Blur: nothing matches
        saveScreenshot(panel, QStringLiteral("effects_library_folders"));

        // Double-click semantics put the chroma key on the selected clip in one undo step.
        const quint64 clip = f.addClip(kV1, 0, 50);
        f.selection.selectClip(clip);
        const int commands = f.project.undoStack()->count();
        QVERIFY(effectactions::activate(f.ctx, QLatin1String(fxid::ChromaKey)));
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        QCOMPARE(f.clip(clip)->effects.size(), 1);
        QCOMPARE(f.clip(clip)->effects[0].type, QLatin1String(fxid::ChromaKey));

        // The Inspector's Add Effect menu has one submenu per folder.
        InspectorPanel inspector(f.ctx);
        f.selection.clear(); // the inspector follows selection changes
        f.selection.selectClip(clip);
        QToolButton* add = nullptr;
        for (QToolButton* b : inspector.findChildren<QToolButton*>()) {
            if (b->text() == QLatin1String("Add Effect"))
                add = b;
        }
        QVERIFY(add && add->menu());
        QStringList menus;
        for (QAction* a : add->menu()->actions())
            menus.append(a->text());
        QCOMPARE(menus.size(), 8);
        QCOMPARE(menus[3], QStringLiteral("Key"));
    }

    // Spec 40.3: the eyedropper button of the key colour drives SelectionModel::colorPicker().
    void inspectorPickerButton()
    {
        PickFixture p;
        InspectorPanel inspector(p.f.ctx);
        p.f.selection.clear(); // the inspector follows selection changes
        p.f.selection.selectClip(p.clipId);
        auto* button = inspector.findChild<QToolButton*>(QStringLiteral("ColorPickerButton"));
        QVERIFY(button);
        QVERIFY(!button->isChecked());
        QVERIFY(!p.overlay->isPicking());
        button->click();
        const ColorPickTarget& t = p.f.selection.colorPicker();
        QCOMPARE(t.clipId, p.clipId);
        QCOMPARE(t.effectId, p.keyId);
        QCOMPARE(t.key, fxkeys::KeyColor);
        QVERIFY(p.overlay->isPicking());
        QVERIFY(p.overlay->isVisible());
        button->click(); // off again
        QVERIFY(!p.f.selection.colorPicker().isActive());
        QVERIFY(!p.overlay->isPicking());
        // Only colour parameters marked for the viewer have the button (one per chroma key).
        QCOMPARE(
            inspector.findChildren<QToolButton*>(QStringLiteral("ColorPickerButton")).size(), 1);
        // A pick unchecks the button.
        button->click();
        QTest::mouseClick(p.overlay, Qt::LeftButton, {}, p.at(40, 90));
        QVERIFY(!button->isChecked());
    }

    // Spec 40.3: click and drag picks, one undo step, the picture before the key, Esc.
    void overlayPicks()
    {
        PickFixture p;
        p.f.selection.setColorPicker({p.clipId, p.keyId, fxkeys::KeyColor});
        QVERIFY(p.overlay->isPicking());
        const int before = p.undoCount();
        QTest::mouseClick(p.overlay, Qt::LeftButton, {}, p.at(40, 90));
        QCOMPARE(p.undoCount(), before + 1);
        QCOMPARE(p.f.project.undoStack()->text(p.undoCount() - 1), QStringLiteral("Pick Color"));
        QCOMPARE(
            p.key().params.statics.value(fxkeys::KeyColor).value<QColor>(), QColor(0, 200, 80));
        QCOMPARE(p.key().params.statics.value(fxkeys::Screen).toString(), fxchoice::Custom);
        QVERIFY(!p.f.selection.colorPicker().isActive()); // one pick per button press
        QVERIFY(!p.overlay->isPicking());
        // The frame was rendered with the key and the effects after it switched off.
        QVERIFY(p.rendered);
        const ClipData* shown = editops::clipById(*p.rendered, p.clipId);
        QVERIFY(shown && shown->effects.size() == 2);
        QVERIFY(!shown->effects[0].enabled && !shown->effects[1].enabled);
        QVERIFY(p.f.clip(p.clipId)->effects[0].enabled); // the project itself is unchanged

        // A drag across both halves averages the area (equal parts of each colour).
        p.f.selection.setColorPicker({p.clipId, p.keyId, fxkeys::KeyColor});
        QTest::mousePress(p.overlay, Qt::LeftButton, {}, p.at(120, 40));
        QMouseEvent move(QEvent::MouseMove, QPointF(p.at(200, 140)), QPointF(p.at(200, 140)),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(p.overlay, &move);
        QTest::mouseRelease(p.overlay, Qt::LeftButton, {}, p.at(199, 140));
        const QColor mean = p.key().params.statics.value(fxkeys::KeyColor).value<QColor>();
        QVERIFY2(std::abs(mean.red() - 20) <= 2 && std::abs(mean.green() - 150) <= 2
                && std::abs(mean.blue() - 150) <= 2,
            qPrintable(mean.name()));
        // Undo restores the previous colour.
        p.f.project.undoStack()->undo();
        QCOMPARE(
            p.key().params.statics.value(fxkeys::KeyColor).value<QColor>(), QColor(0, 200, 80));

        // Esc cancels without an edit; outside the clip the eyedropper is off.
        p.f.selection.setColorPicker({p.clipId, p.keyId, fxkeys::KeyColor});
        const int count = p.undoCount();
        p.overlay->setFocus();
        QTest::keyClick(p.overlay, Qt::Key_Escape);
        QVERIFY(!p.f.selection.colorPicker().isActive());
        QCOMPARE(p.undoCount(), count);
        p.f.selection.setColorPicker({p.clipId, p.keyId, fxkeys::KeyColor});
        p.f.project.timeline()->setPlayhead(150);
        QVERIFY(!p.overlay->isPicking());
    }

    void averageColorIgnoresTransparency()
    {
        QImage image(4, 1, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        image.setPixel(0, 0, qRgba(100, 50, 0, 255));
        image.setPixel(1, 0, qRgba(50, 25, 0, 128)); // the same colour, half covered
        QCOMPARE(ColorPickOverlay::averageColor(image, QRect(0, 0, 4, 1)), QColor(100, 50, 0));
        QVERIFY(!ColorPickOverlay::averageColor(image, QRect(2, 0, 2, 1)).isValid());
        QVERIFY(!ColorPickOverlay::averageColor(image, QRect(10, 10, 2, 2)).isValid());
    }
    // Spec 40.21: the HSL Key's hue has an eyedropper too; a pick writes the colour's hue.
    void hslKeyHuePick()
    {
        PickFixture p;
        QVERIFY(effectactions::applyEffect(p.f.project, {p.clipId}, QLatin1String(fxid::HslKey)));
        const quint64 hslId = p.f.clip(p.clipId)->effects.last().id;
        InspectorPanel inspector(p.f.ctx);
        p.f.selection.clear();
        p.f.selection.selectClip(p.clipId);
        // Chroma Key's colour and HSL Key's hue.
        QCOMPARE(
            inspector.findChildren<QToolButton*>(QStringLiteral("ColorPickerButton")).size(), 2);
        p.f.selection.setColorPicker({p.clipId, hslId, fxkeys::Hue});
        QVERIFY(p.overlay->isPicking());
        const int before = p.undoCount();
        QTest::mouseClick(p.overlay, Qt::LeftButton, {}, p.at(40, 90)); // (0, 200, 80)
        QCOMPARE(p.undoCount(), before + 1);
        const double hue = p.f.clip(p.clipId)->effects.last().params.value(fxkeys::Hue, 0, -1);
        QVERIFY2(
            std::abs(hue - QColor(0, 200, 80).hslHueF() * 360.0) < 0.5, QByteArray::number(hue));
        QVERIFY(!p.f.selection.colorPicker().isActive());
        // An animated hue gets a key at the playhead.
        AnimatedValue& v = p.f.project.timeline()
                               ->mutableData()
                               .videoTracks[0]
                               .clips[0]
                               .effects.last()
                               .params.values[fxkeys::Hue];
        v.setKey(0, 10.0);
        v.setKey(50, 20.0);
        p.f.project.timeline()->setPlayhead(30);
        p.f.selection.setColorPicker({p.clipId, hslId, fxkeys::Hue});
        QTest::mouseClick(p.overlay, Qt::LeftButton, {}, p.at(250, 90)); // (40, 100, 220)
        const AnimatedValue& after
            = p.f.clip(p.clipId)->effects.last().params.values.value(fxkeys::Hue);
        QVERIFY(after.hasKeyAt(30));
        QVERIFY(std::abs(after.valueAt(30) - QColor(40, 100, 220).hslHueF() * 360.0) < 0.5);
    }

    // Spec 40.21: Curves rows show a curve graph; dragging bends the curve in one undo step.
    void curveRowEdits()
    {
        PickFixture p;
        QVERIFY(effectactions::applyEffect(p.f.project, {p.clipId}, QLatin1String(fxid::Curves)));
        InspectorPanel inspector(p.f.ctx);
        inspector.resize(420, 1600);
        inspector.show();
        p.f.selection.clear();
        p.f.selection.selectClip(p.clipId);
        QCoreApplication::processEvents();
        auto* editor = inspector.findChild<CurveEditor*>(QStringLiteral("curve:masterCurve"));
        QVERIFY(editor);
        QVERIFY(inspector.findChild<CurveEditor*>(QStringLiteral("curve:blueCurve")));
        QCOMPARE(editor->points(), curve::identity());
        const int before = p.undoCount();
        const QPoint start = editor->toWidget({0.5, 0.5}).toPoint();
        const QPoint end = editor->toWidget({0.5, 0.75}).toPoint();
        QTest::mousePress(editor, Qt::LeftButton, {}, start);
        QMouseEvent move(QEvent::MouseMove, QPointF(end), editor->mapToGlobal(QPointF(end)),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor, &move);
        QTest::mouseRelease(editor, Qt::LeftButton, {}, end);
        QCOMPARE(p.undoCount(), before + 1);
        const curve::Points edited = curve::parse(
            p.f.clip(p.clipId)->effects.last().params.staticValue(fxkeys::MasterCurve).toString());
        QCOMPARE(edited.size(), 3);
        QVERIFY(std::abs(edited[1].y - 0.75) < 0.03);
        // Right-click removes the point (one more step); undo brings it back.
        QTest::mouseClick(editor, Qt::RightButton, {}, editor->toWidget(edited[1]).toPoint());
        QCOMPARE(p.undoCount(), before + 2);
        QVERIFY(curve::isIdentity(curve::parse(p.f.clip(p.clipId)
                                                   ->effects.last()
                                                   .params.staticValue(fxkeys::MasterCurve)
                                                   .toString())));
        p.f.project.undoStack()->undo();
        QCOMPARE(curve::parse(p.f.clip(p.clipId)
                                  ->effects.last()
                                  .params.staticValue(fxkeys::MasterCurve)
                                  .toString())
                     .size(),
            3);
    }

    // Spec 40.21: a File row chooses the LUT file through the file dialog, in one undo step.
    void fileRowPicks()
    {
        PickFixture p;
        QVERIFY(effectactions::applyEffect(p.f.project, {p.clipId}, QLatin1String(fxid::ApplyLut)));
        InspectorPanel inspector(p.f.ctx);
        QString askedFilter;
        inspector.setFilePicker([&](const QString&, const QString&, const QString& filter) {
            askedFilter = filter;
            return QStringLiteral("/luts/look.cube");
        });
        p.f.selection.clear();
        p.f.selection.selectClip(p.clipId);
        auto* browse = inspector.findChild<QToolButton*>(QStringLiteral("browse:lutFile"));
        auto* name = inspector.findChild<QLineEdit*>(QStringLiteral("file:lutFile"));
        QVERIFY(browse && name);
        QVERIFY(name->text().isEmpty());
        const int before = p.undoCount();
        browse->click();
        QVERIFY(askedFilter.contains(QLatin1String("*.cube")));
        QCOMPARE(p.undoCount(), before + 1);
        QCOMPARE(p.f.clip(p.clipId)->effects.last().params.staticValue(fxkeys::LutFile).toString(),
            QStringLiteral("/luts/look.cube"));
        QCoreApplication::processEvents();
        QCOMPARE(name->text(), QStringLiteral("look.cube"));
        QVERIFY(name->font().italic()); // the file does not exist
        // Undo restores the empty path: the edit did not write into the undo snapshot (the
        // effect list is shared with it until the edit detaches it).
        p.f.project.undoStack()->undo();
        QVERIFY(p.f.clip(p.clipId)
                    ->effects.last()
                    .params.staticValue(fxkeys::LutFile)
                    .toString()
                    .isEmpty());
    }
};

QTEST_MAIN(TestEffectsUi)

#include "test_effectsui.moc"
