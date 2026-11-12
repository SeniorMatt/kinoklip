// SPDX-License-Identifier: GPL-3.0-or-later
// The Title tab's layer list (spec 68): add, duplicate, delete, move, show / hide, rename, the
// selected layer's controls, one undo step each.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/ParamKeys.h"
#include "core/TitleLayers.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"

#include <QAction>
#include <QListWidget>
#include <QMenu>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

namespace {

QListWidget* layerList(InspectorPanel& panel)
{
    return panel.findChild<QListWidget*>(QStringLiteral("title-layer-list"));
}

QToolButton* button(InspectorPanel& panel, const char* name)
{
    return panel.findChild<QToolButton*>(QString::fromLatin1(name));
}

} // namespace

class TestTitleLayersUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    // Spec 68.10: the Title tab lists the layers above the controls; an old title is one layer.
    void spec68_10_listShowsTheLayers()
    {
        PanelFixture f;
        const quint64 id
            = effectactions::addGeneratorClip(f.project, QStringLiteral("text_title"), 0);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 900);
        f.selection.selectClip(id);
        panel.tabs()->setCurrentIndex(InspectorPanel::TitleTab);
        QListWidget* list = layerList(panel);
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->text(), f.clip(id)->title.staticValue(keys::Text).toString());
        QCOMPARE(panel.selectedTitleLayer(), quint64(0));
        // The controls below belong to the first layer, as before.
        bool sawText = false;
        for (ParamGroupBox* g : panel.groups()) {
            for (const ParamRow* row : g->rows()) {
                if (row->ref().key == keys::Text) {
                    sawText = true;
                    QCOMPARE(row->ref().scope, ParamScope::Title);
                }
            }
        }
        QVERIFY(sawText);
        QVERIFY(!button(panel, "title-layer-delete")->isEnabled());
    }

    // Spec 68.11: every layer operation is one undo step that undo takes back.
    void spec68_11_everyOperationUndoes()
    {
        PanelFixture f;
        const quint64 id
            = effectactions::addGeneratorClip(f.project, QStringLiteral("text_title"), 0);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 900);
        f.selection.selectClip(id);
        panel.tabs()->setCurrentIndex(InspectorPanel::TitleTab);
        QUndoStack* undo = f.project.undoStack();

        // Runs @p act, expects one new undo step and a changed clip, then undoes and redoes.
        auto step = [&](const char* what, const std::function<void()>& act) {
            const ClipData before = *f.clip(id);
            const int count = undo->count();
            act();
            QVERIFY2(undo->count() == count + 1, what);
            const ClipData after = *f.clip(id);
            QVERIFY2(after != before, what);
            undo->undo();
            QVERIFY2(*f.clip(id) == before, what);
            undo->redo();
            QVERIFY2(*f.clip(id) == after, what);
        };

        step("add text", [&] { button(panel, "title-layer-add-text")->click(); });
        QCOMPARE(f.clip(id)->titleLayers.size(), 1);
        const quint64 text = f.clip(id)->titleLayers[0].id;
        QCOMPARE(panel.selectedTitleLayer(), text);
        QCOMPARE(layerList(panel)->count(), 2);

        step("add shape", [&] {
            QMenu* menu = button(panel, "title-layer-add-shape")->menu();
            QVERIFY(menu);
            QCOMPARE(menu->actions().size(), 4);
            menu->actions().at(2)->trigger(); // ellipse
        });
        const quint64 shape = f.clip(id)->titleLayers[1].id;
        QCOMPARE(f.clip(id)->titleLayers[1].kind, TitleLayerKind::Shape);
        QCOMPARE(f.clip(id)->titleLayers[1].params.staticValue(keys::ShapeType).toString(),
            QStringLiteral("ellipse"));
        // The top layer is first in the list.
        QCOMPARE(layerList(panel)->item(0)->data(Qt::UserRole).toULongLong(), shape);

        step("duplicate", [&] { button(panel, "title-layer-duplicate")->click(); });
        QCOMPARE(f.clip(id)->titleLayers.size(), 3);
        panel.selectTitleLayer(shape);
        step("move down", [&] { button(panel, "title-layer-down")->click(); });
        QCOMPARE(titlelayers::order(*f.clip(id)).indexOf(shape), 1);
        step("move up", [&] { button(panel, "title-layer-up")->click(); });
        auto itemOf = [&](quint64 layer) -> QListWidgetItem* {
            for (int i = 0; i < layerList(panel)->count(); ++i)
                if (layerList(panel)->item(i)->data(Qt::UserRole).toULongLong() == layer)
                    return layerList(panel)->item(i);
            return nullptr;
        };
        step("hide", [&] { itemOf(shape)->setCheckState(Qt::Unchecked); });
        step("rename", [&] { itemOf(shape)->setText(QStringLiteral("Panel")); });
        QCOMPARE(titlelayers::displayName(*f.clip(id), shape), QStringLiteral("Panel"));
        panel.selectTitleLayer(text);
        step("delete", [&] { button(panel, "title-layer-delete")->click(); });
        QCOMPARE(f.clip(id)->titleLayers.size(), 2);
        // The first layer cannot be deleted but can be moved and hidden.
        panel.selectTitleLayer(0);
        QVERIFY(!button(panel, "title-layer-delete")->isEnabled());
        step("move first layer", [&] { button(panel, "title-layer-up")->click(); });
        step("hide first layer", [&] {
            layerList(panel)->item(layerList(panel)->count() - 2)->setCheckState(Qt::Unchecked);
        });
    }

    // Spec 68.12: the controls edit the selected layer.
    void spec68_12_controlsEditTheSelectedLayer()
    {
        PanelFixture f;
        const quint64 id
            = effectactions::addGeneratorClip(f.project, QStringLiteral("text_title"), 0);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 900);
        f.selection.selectClip(id);
        panel.tabs()->setCurrentIndex(InspectorPanel::TitleTab);
        button(panel, "title-layer-add-shape")->menu()->actions().at(0)->trigger();
        const quint64 shape = f.clip(id)->titleLayers[0].id;
        QCOMPARE(panel.selectedTitleLayer(), shape);
        int rows = 0;
        bool sawWidth = false;
        for (ParamGroupBox* g : panel.groups()) {
            for (const ParamRow* row : g->rows()) {
                if (row->ref().scope == ParamScope::Transform)
                    continue; // the Transform and Cropping rows
                ++rows;
                QCOMPARE(row->ref().scope, ParamScope::TitleLayer);
                QCOMPARE(row->ref().effectId, shape);
                sawWidth = sawWidth || row->ref().key == keys::ShapeWidth;
            }
        }
        QVERIFY(rows > 5 && sawWidth);
        // Back to the first layer: its own scope again.
        panel.selectTitleLayer(0);
        for (ParamGroupBox* g : panel.groups()) {
            for (const ParamRow* row : g->rows()) {
                if (row->ref().scope != ParamScope::Transform)
                    QCOMPARE(row->ref().scope, ParamScope::Title);
            }
        }
    }
    // Typing in the Text row renames the layer in the list without rebuilding the tab: a
    // rebuild per key showed a frame of squeezed, overlapping rows while typing fast.
    void typingKeepsTheTabAndRenamesTheLayer()
    {
        PanelFixture f;
        const quint64 id
            = effectactions::addGeneratorClip(f.project, QStringLiteral("text_title"), 0);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 900);
        f.selection.selectClip(id);
        panel.tabs()->setCurrentIndex(InspectorPanel::TitleTab);
        const QList<ParamGroupBox*> before = panel.groups();
        QListWidget* list = layerList(panel);
        QVERIFY(list && !before.isEmpty());
        for (const QString& text :
            {QStringLiteral("a"), QStringLiteral("ab"), QStringLiteral("abc")}) {
            f.project.beginEdit(QStringLiteral("Edit Text"));
            editops::clipById(f.project.timeline()->mutableData(), id)
                ->title.setStatic(keys::Text, text);
            f.project.timeline()->notifyChanged();
            f.project.endEdit();
            QCOMPARE(panel.groups(), before); // the same widgets: no rebuild
            QCOMPARE(layerList(panel), list);
            QCOMPARE(list->item(0)->text(), text);
        }
    }
};

QTEST_MAIN(TestTitleLayersUi)
#include "test_titlelayersui.moc"
