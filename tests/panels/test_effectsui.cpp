// SPDX-License-Identifier: GPL-3.0-or-later
// Edit page effects UI (docs/behavior-spec.md section 38): the Effects library folders, applying
// a keyer from the library, the Inspector's eyedropper button and the timeline viewer's
// ColorPickOverlay (click and drag picks, one undo step, Esc, the picture before the key).
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/viewer/ColorPickOverlay.h"
#include "ui/viewer/ViewerSurface.h"

#include <QApplication>
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
            = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1}, 320, 180);
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

    // Spec 38.1: video effects sit in folders in the library and the Add Effect menu.
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
                QStringLiteral("Transform")}));
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
        QCOMPARE(menus.size(), 7);
        QCOMPARE(menus[3], QStringLiteral("Key"));
    }

    // Spec 38.3: the eyedropper button of the key colour drives SelectionModel::colorPicker().
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

    // Spec 38.3: click and drag picks, one undo step, the picture before the key, Esc.
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
};

QTEST_MAIN(TestEffectsUi)

#include "test_effectsui.moc"
