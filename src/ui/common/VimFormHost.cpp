// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimFormHost.h"

#include "ui/common/PanelFrame.h"
#include "ui/common/VimRowOps.h"
#include "ui/common/VimWindowNav.h"
#include "ui/inspector/ValueField.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMetaObject>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace kk {

// ---- The frame round the row of the cursor ----

class VimRowCursor final : public QWidget
{
public:
    explicit VimRowCursor(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setObjectName(QStringLiteral("VimRowCursor"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QColor accent = palette().color(QPalette::Highlight);
        QColor fill = accent;
        fill.setAlpha(28);
        p.fillRect(rect(), fill);
        p.setPen(QPen(accent, 2));
        p.drawRect(rect().adjusted(1, 1, -1, -1));
    }
};

namespace {

/// True for the controls that are rows of their own (when not part of a VimRowOps row).
bool isControl(const QWidget* w)
{
    if (qobject_cast<const QAbstractSlider*>(w))
        return !qobject_cast<const QScrollBar*>(w) && w->focusPolicy() != Qt::NoFocus;
    return qobject_cast<const QAbstractButton*>(w) || qobject_cast<const QComboBox*>(w)
        || qobject_cast<const QAbstractSpinBox*>(w) || qobject_cast<const QLineEdit*>(w)
        || qobject_cast<const QPlainTextEdit*>(w) || qobject_cast<const QTextEdit*>(w)
        || w->inherits("kk::ValueField");
}

/// A control that contains other widgets which are not rows (the editor of a spin box).
bool isCompound(const QWidget* w)
{
    return qobject_cast<const QAbstractSpinBox*>(w) || qobject_cast<const QComboBox*>(w)
        || qobject_cast<const QAbstractScrollArea*>(w) || w->inherits("kk::ValueField")
        || w->inherits("QTabBar");
}

VimRowOps* opsOf(const QWidget* w)
{
    return dynamic_cast<VimRowOps*>(w->property(VimRowOps::kRowProperty).value<QObject*>());
}

VimFormHost::Item makeItem(QWidget* anchor, VimRowOps* ops, const QRect& rect)
{
    VimFormHost::Item item;
    item.anchor = anchor;
    item.ops = ops;
    item.rect = rect;
    return item;
}

QRect rectIn(const QWidget* root, const QWidget* w)
{
    return QRect(w->mapTo(root, QPoint(0, 0)), w->size());
}

} // namespace

VimFormHost::VimFormHost(ActionRegistry* actions)
    : VimPanelHost(actions)
{ }

VimFormHost::~VimFormHost()
{
    delete m_frame;
}

void VimFormHost::setRoot(QWidget* root)
{
    if (root == m_root)
        return;
    m_root = root;
    m_cursor = nullptr;
    m_line = m_item = 0;
    if (m_frame)
        m_frame->hide();
}

// ---- Rows ----

QList<QList<VimFormHost::Item>> VimFormHost::lines() const
{
    QList<QList<Item>> out;
    if (!m_root)
        return out;
    QList<Item> items;
    QSet<const QWidget*> owned;
    const QList<QWidget*> all = m_root->findChildren<QWidget*>();
    for (QWidget* w : all) {
        VimRowOps* ops = opsOf(w);
        if (!ops)
            continue;
        QRect box;
        for (QWidget* part : ops->vimWidgets()) {
            if (!part)
                continue;
            owned.insert(part);
            if (part->isVisibleTo(m_root))
                box |= rectIn(m_root, part);
        }
        if (!box.isEmpty() && w->isVisibleTo(m_root))
            items.append(makeItem(w, ops, box));
    }
    for (QWidget* w : all) {
        if (!isControl(w) || owned.contains(w) || !w->isVisibleTo(m_root) || !w->isEnabled())
            continue;
        bool skip = false;
        for (const QWidget* a = w->parentWidget(); a && a != m_root; a = a->parentWidget()) {
            if (owned.contains(a) || isCompound(a)) {
                skip = true;
                break;
            }
        }
        // The buttons of the panel's title bar are not rows.
        if (const PanelFrame* frame = vimwin::frameOf(w);
            frame && frame->contentWidget() && !frame->contentWidget()->isAncestorOf(w))
            skip = true;
        // The buttons of a tab bar and of a combo's popup are not rows.
        if (skip || (w->parentWidget() && w->parentWidget()->inherits("QTabBar")))
            continue;
        items.append(makeItem(w, nullptr, rectIn(m_root, w)));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.rect.top() != b.rect.top() ? a.rect.top() < b.rect.top()
                                            : a.rect.left() < b.rect.left();
    });
    int bottom = -1;
    for (const Item& item : std::as_const(items)) {
        if (out.isEmpty() || item.rect.top() >= bottom - 3) {
            out.emplace_back();
            bottom = item.rect.bottom();
        } else {
            bottom = std::max(bottom, item.rect.bottom());
        }
        out.last().append(item);
    }
    for (auto& line : out)
        std::sort(line.begin(), line.end(),
            [](const Item& a, const Item& b) { return a.rect.left() < b.rect.left(); });
    return out;
}

VimFormHost::Item VimFormHost::current() const
{
    const QList<QList<Item>> all = lines();
    if (all.isEmpty())
        return {};
    for (const auto& line : all) {
        for (const Item& item : line) {
            if (m_cursor && item.anchor == m_cursor)
                return item;
        }
    }
    const int l = std::clamp(m_line, 0, int(all.size()) - 1);
    return all[l][std::clamp(m_item, 0, int(all[l].size()) - 1)];
}

int VimFormHost::cursorLine() const
{
    const QList<QList<Item>> all = lines();
    if (all.isEmpty())
        return -1;
    for (int l = 0; l < all.size(); ++l) {
        for (const Item& item : all[l]) {
            if (m_cursor && item.anchor == m_cursor)
                return l;
        }
    }
    return std::clamp(m_line, 0, int(all.size()) - 1);
}

int VimFormHost::cursorItem() const
{
    const QList<QList<Item>> all = lines();
    if (all.isEmpty())
        return -1;
    for (const auto& line : all) {
        for (int i = 0; i < line.size(); ++i) {
            if (m_cursor && line[i].anchor == m_cursor)
                return i;
        }
    }
    const int l = std::clamp(m_line, 0, int(all.size()) - 1);
    return std::clamp(m_item, 0, int(all[l].size()) - 1);
}

QString VimFormHost::rowName(const Item& item) const
{
    if (item.ops && !item.ops->vimName().isEmpty())
        return item.ops->vimName();
    const QWidget* w = item.anchor;
    if (!w)
        return {};
    if (const auto* b = qobject_cast<const QAbstractButton*>(w); b && !b->text().isEmpty())
        return b->text().remove(QLatin1Char('&'));
    return w->accessibleName().isEmpty() ? w->toolTip() : w->accessibleName();
}

void VimFormHost::moveTo(int line, int item)
{
    const QList<QList<Item>> all = lines();
    if (all.isEmpty())
        return;
    m_line = std::clamp(line, 0, int(all.size()) - 1);
    m_item = std::clamp(item, 0, int(all[m_line].size()) - 1);
    m_cursor = all[m_line][m_item].anchor;
    // The focus goes back to the panel: a control that had it would catch the next key.
    QWidget* focus = QApplication::focusWidget();
    if (m_root && focus && focus != m_root && m_root->isAncestorOf(focus)
        && m_root->focusPolicy() != Qt::NoFocus)
        m_root->setFocus(Qt::OtherFocusReason);
    show();
}

void VimFormHost::show()
{
    const Item item = current();
    if (!m_visible || !item.anchor || !m_root) {
        if (m_frame)
            m_frame->hide();
        return;
    }
    QWidget* parent = item.anchor->parentWidget();
    if (!parent)
        return;
    if (!m_frame)
        m_frame = new VimRowCursor(parent);
    if (m_frame->parentWidget() != parent)
        m_frame->setParent(parent);
    const QPoint topLeft = parent->mapFrom(m_root, item.rect.topLeft());
    m_frame->setGeometry(QRect(topLeft, item.rect.size()).adjusted(-2, -1, 2, 1));
    m_frame->show();
    m_frame->raise();
    for (QWidget* w = item.anchor; w && w != m_root; w = w->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(w)) {
            scroll->ensureWidgetVisible(item.anchor, 0, 24);
            break;
        }
    }
}

void VimFormHost::setCursorVisible(bool visible)
{
    m_visible = visible;
    if (!visible && m_frame)
        m_frame->hide();
    else if (visible)
        show();
}

void VimFormHost::focusChanged(QWidget* focus)
{
    if (!focus)
        return;
    QWidget* root = nullptr;
    for (QWidget* w = focus; w; w = w->parentWidget()) {
        if (w->property("kkVimContext").toString() == QLatin1String("form")) {
            root = w;
            break;
        }
    }
    if (!root)
        return;
    setRoot(root);
    if (focus == root) {
        if (m_cursor || cursorLine() < 0)
            return show();
        moveTo(0);
        return;
    }
    sync(focus);
}

void VimFormHost::sync(QWidget* focus)
{
    // A click (or Tab) put the focus on a control: the cursor goes to its row.
    if (m_lastFocus == focus)
        return;
    m_lastFocus = focus;
    const QList<QList<Item>> all = lines();
    for (int l = 0; l < all.size(); ++l) {
        for (int i = 0; i < all[l].size(); ++i) {
            const Item& item = all[l][i];
            bool inside = item.anchor == focus || item.anchor->isAncestorOf(focus);
            if (item.ops) {
                for (QWidget* part : item.ops->vimWidgets())
                    inside = inside || (part && (part == focus || part->isAncestorOf(focus)));
            }
            if (inside) {
                m_line = l;
                m_item = i;
                m_cursor = item.anchor;
                show();
                return;
            }
        }
    }
}

// ---- Operations on a row ----

bool VimFormHost::step(const Item& item, int steps, double scale)
{
    if (item.ops)
        return item.ops->vimStep(steps, scale);
    QWidget* w = item.anchor;
    if (!w || steps == 0)
        return false;
    if (auto* field = qobject_cast<ValueField*>(w)) {
        const double before = field->value();
        field->setValue(before + steps * field->step() * scale);
        if (field->value() != before)
            emit field->valueCommitted(field->value());
        return true;
    }
    if (auto* spin = qobject_cast<QDoubleSpinBox*>(w)) {
        spin->setValue(spin->value() + steps * spin->singleStep() * scale);
        return true;
    }
    if (auto* spin = qobject_cast<QSpinBox*>(w)) {
        spin->setValue(
            spin->value() + steps * std::max(1, int(std::lround(spin->singleStep() * scale))));
        return true;
    }
    if (auto* slider = qobject_cast<QAbstractSlider*>(w)) {
        slider->setValue(
            slider->value() + steps * std::max(1, int(std::lround(slider->singleStep() * scale))));
        return true;
    }
    if (auto* combo = qobject_cast<QComboBox*>(w)) {
        const int n = combo->count();
        if (n == 0)
            return false;
        const int index = ((combo->currentIndex() + steps) % n + n) % n;
        combo->setCurrentIndex(index);
        QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index));
        return true;
    }
    if (auto* box = qobject_cast<QCheckBox*>(w)) {
        box->click();
        return true;
    }
    return false;
}

bool VimFormHost::edit(const Item& item)
{
    if (item.ops)
        return item.ops->vimEdit();
    QWidget* w = item.anchor;
    if (!w)
        return false;
    if (auto* field = qobject_cast<ValueField*>(w)) {
        field->setFocus(Qt::OtherFocusReason);
        field->startTextEdit();
        return true;
    }
    if (qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w)
        || qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QTextEdit*>(w)) {
        w->setFocus(Qt::OtherFocusReason);
        return true;
    }
    return activate(item);
}

bool VimFormHost::activate(const Item& item)
{
    if (item.ops)
        return item.ops->vimActivate();
    QWidget* w = item.anchor;
    if (!w)
        return false;
    if (auto* button = qobject_cast<QAbstractButton*>(w)) {
        button->click();
        return true;
    }
    if (auto* combo = qobject_cast<QComboBox*>(w)) {
        combo->setFocus(Qt::OtherFocusReason);
        combo->showPopup();
        return true;
    }
    return edit(item);
}

bool VimFormHost::reset(const Item& item)
{
    if (item.ops)
        return item.ops->vimRemove() || item.ops->vimReset();
    return false;
}

bool VimFormHost::yank(const Item& item)
{
    QVariant value;
    QString name = rowName(item);
    if (item.ops) {
        value = item.ops->vimValue();
    } else if (QWidget* w = item.anchor) {
        QVariant inner;
        if (auto* field = qobject_cast<ValueField*>(w))
            inner = field->value();
        else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w))
            inner = dspin->value();
        else if (auto* spin = qobject_cast<QSpinBox*>(w))
            inner = spin->value();
        else if (auto* slider = qobject_cast<QAbstractSlider*>(w))
            inner = slider->value();
        else if (auto* combo = qobject_cast<QComboBox*>(w))
            inner = combo->currentIndex();
        else if (auto* box = qobject_cast<QCheckBox*>(w))
            inner = box->isChecked();
        else if (auto* line = qobject_cast<QLineEdit*>(w))
            inner = line->text();
        else if (auto* text = qobject_cast<QPlainTextEdit*>(w))
            inner = text->toPlainText();
        if (inner.isValid())
            value = QVariantList {QString::fromLatin1(w->metaObject()->className()), inner};
    }
    if (!value.isValid())
        return false;
    m_register = value;
    m_registerName = name;
    showMessage(QObject::tr("Yanked the value of %1").arg(name));
    return true;
}

bool VimFormHost::paste(const Item& item)
{
    if (!m_register.isValid())
        return false;
    if (item.ops)
        return item.ops->vimSetValue(m_register);
    QWidget* w = item.anchor;
    const QVariantList list = m_register.toList();
    if (!w || list.size() != 2 || list[0].toString() != QLatin1String(w->metaObject()->className()))
        return false;
    const QVariant v = list[1];
    if (auto* field = qobject_cast<ValueField*>(w)) {
        field->setValue(v.toDouble());
        emit field->valueCommitted(field->value());
    } else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w)) {
        dspin->setValue(v.toDouble());
    } else if (auto* spin = qobject_cast<QSpinBox*>(w)) {
        spin->setValue(v.toInt());
    } else if (auto* slider = qobject_cast<QAbstractSlider*>(w)) {
        slider->setValue(v.toInt());
    } else if (auto* combo = qobject_cast<QComboBox*>(w)) {
        combo->setCurrentIndex(v.toInt());
        QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, v.toInt()));
    } else if (auto* box = qobject_cast<QCheckBox*>(w)) {
        if (box->isChecked() != v.toBool())
            box->click();
    } else if (auto* line = qobject_cast<QLineEdit*>(w)) {
        line->setText(v.toString());
        emit line->editingFinished();
    } else if (auto* text = qobject_cast<QPlainTextEdit*>(w)) {
        text->setPlainText(v.toString());
    } else {
        return false;
    }
    return true;
}

bool VimFormHost::fold(int mode)
{
    const QList<QList<Item>> all = lines();
    const int at = cursorLine();
    if (at < 0)
        return false;
    // The section of the row: its own header, else the nearest one above.
    for (int l = at; l >= 0; --l) {
        for (const Item& item : all[l]) {
            if (item.ops && item.ops->vimFold(mode)) {
                m_cursor = item.anchor;
                m_line = l;
                m_item = 0;
                QWidget* anchor = item.anchor;
                const QPointer<QWidget> keep = anchor;
                // The layout changes: the frame is drawn when it has settled.
                QMetaObject::invokeMethod(m_root, [this] { show(); }, Qt::QueuedConnection);
                show();
                return true;
            }
        }
    }
    return false;
}

bool VimFormHost::foldAll(int mode)
{
    bool any = false;
    for (const auto& line : lines()) {
        for (const Item& item : line)
            any = (item.ops && item.ops->vimFold(mode)) || any;
    }
    QMetaObject::invokeMethod(
        m_root, [this] { moveTo(std::max(0, cursorLine())); }, Qt::QueuedConnection);
    return any;
}

bool VimFormHost::nextTab(int delta)
{
    if (!m_root)
        return false;
    for (auto* tabs : m_root->findChildren<QTabWidget*>()) {
        if (!tabs->isVisibleTo(m_root) || tabs->count() < 2)
            continue;
        const int n = tabs->count();
        tabs->setCurrentIndex(((tabs->currentIndex() + delta) % n + n) % n);
        m_cursor = nullptr;
        m_line = m_item = 0;
        showMessage(tabs->tabText(tabs->currentIndex()).remove(QLatin1Char('&')));
        QMetaObject::invokeMethod(m_root, [this] { moveTo(0); }, Qt::QueuedConnection);
        return true;
    }
    return false;
}

bool VimFormHost::keyframe(const QString& what, const Item& item)
{
    VimRowOps* ops = item.ops;
    using I = VimRowOps::Ease;
    if (ops) {
        if (what == QLatin1String("toggle") && ops->vimToggleKeyframe())
            return true;
        if (what == QLatin1String("next") && ops->vimJumpKeyframe(true))
            return true;
        if (what == QLatin1String("prev") && ops->vimJumpKeyframe(false))
            return true;
        if (what == QLatin1String("delete") && ops->vimDeleteKeyframe())
            return true;
        static const QList<std::pair<QString, I>> interps {{QStringLiteral("linear"), I::Linear},
            {QStringLiteral("ease_in"), I::EaseIn}, {QStringLiteral("ease_out"), I::EaseOut},
            {QStringLiteral("ease_in_out"), I::EaseInOut}, {QStringLiteral("smooth"), I::Smooth},
            {QStringLiteral("hold"), I::Hold}};
        for (const auto& [name, interp] : interps) {
            if (what == name && ops->vimSetInterp(interp))
                return true;
        }
    }
    // A row without keyframes of its own: the clip's actions (the last parameter touched).
    static const QHash<QString, QString> actions {
        {QStringLiteral("toggle"), QStringLiteral("clip.add_keyframe")},
        {QStringLiteral("next"), QStringLiteral("clip.next_keyframe")},
        {QStringLiteral("prev"), QStringLiteral("clip.prev_keyframe")},
        {QStringLiteral("delete"), QStringLiteral("clip.delete_keyframe")},
        {QStringLiteral("linear"), QStringLiteral("clip.keyframe_linear")},
        {QStringLiteral("ease_in"), QStringLiteral("clip.keyframe_ease_in")},
        {QStringLiteral("ease_out"), QStringLiteral("clip.keyframe_ease_out")},
        {QStringLiteral("ease_in_out"), QStringLiteral("clip.keyframe_ease_in_out")}};
    return trigger(actions.value(what));
}

// ---- Commands ----

bool VimFormHost::move(int delta)
{
    const QList<QList<Item>> all = lines();
    if (all.isEmpty())
        return false;
    const int at = cursorLine();
    const int to = std::clamp(at + delta, 0, int(all.size()) - 1);
    // Keep the column: the item of the new line nearest to the old item's place.
    int item = 0;
    if (const Item now = current(); now.anchor) {
        int best = 1 << 30;
        for (int i = 0; i < all[to].size(); ++i) {
            const int d = std::abs(all[to][i].rect.left() - now.rect.left());
            if (d < best) {
                best = d;
                item = i;
            }
        }
    }
    moveTo(to, item);
    return true;
}

bool VimFormHost::hostCommand(const QString& name, int count, bool hasCount)
{
    if (!name.startsWith(QLatin1String("form_")))
        return false;
    if (!m_root)
        return true;
    const QString cmd = name.mid(5);
    const int n = std::max(1, count);
    if (cursorLine() >= 0 && !m_cursor)
        moveTo(cursorLine(), std::max(0, cursorItem()));
    const Item item = current();
    bool ok = true;
    if (cmd == QLatin1String("down")) {
        ok = move(n);
    } else if (cmd == QLatin1String("up")) {
        ok = move(-n);
    } else if (cmd == QLatin1String("first")) {
        moveTo(hasCount ? count - 1 : 0);
    } else if (cmd == QLatin1String("last")) {
        moveTo(hasCount ? count - 1 : 1 << 20);
    } else if (cmd == QLatin1String("left") || cmd == QLatin1String("right")) {
        const int sign = cmd == QLatin1String("right") ? 1 : -1;
        const QList<QList<Item>> all = lines();
        const int line = cursorLine();
        if (line >= 0 && all[line].size() > 1)
            moveTo(line, cursorItem() + sign * n);
        else
            ok = step(item, sign * n, 1.0);
    } else if (cmd == QLatin1String("inc") || cmd == QLatin1String("dec")) {
        ok = step(item, cmd == QLatin1String("inc") ? n : -n, 1.0);
    } else if (cmd == QLatin1String("inc_coarse") || cmd == QLatin1String("dec_coarse")) {
        ok = step(item, cmd == QLatin1String("inc_coarse") ? n : -n, 10.0);
    } else if (cmd == QLatin1String("inc_fine") || cmd == QLatin1String("dec_fine")) {
        ok = step(item, cmd == QLatin1String("inc_fine") ? n : -n, 0.1);
    } else if (cmd == QLatin1String("edit")) {
        ok = edit(item);
    } else if (cmd == QLatin1String("activate")) {
        ok = activate(item);
    } else if (cmd == QLatin1String("switch")) {
        ok = item.ops && item.ops->vimSwitch();
        if (!ok)
            ok = activate(item);
    } else if (cmd == QLatin1String("delete")) {
        ok = reset(item);
    } else if (cmd == QLatin1String("yank")) {
        ok = yank(item);
    } else if (cmd == QLatin1String("paste")) {
        ok = paste(item);
    } else if (cmd == QLatin1String("fold_toggle")) {
        ok = fold(0);
    } else if (cmd == QLatin1String("fold_open")) {
        ok = fold(1);
    } else if (cmd == QLatin1String("fold_close")) {
        ok = fold(-1);
    } else if (cmd == QLatin1String("fold_open_all")) {
        ok = foldAll(1);
    } else if (cmd == QLatin1String("fold_close_all")) {
        ok = foldAll(-1);
    } else if (cmd == QLatin1String("move_down") || cmd == QLatin1String("move_up")) {
        ok = item.ops && item.ops->vimMove(cmd == QLatin1String("move_down") ? n : -n);
    } else if (cmd == QLatin1String("tab_next") || cmd == QLatin1String("tab_prev")) {
        ok = nextTab(cmd == QLatin1String("tab_next") ? n : -n);
    } else if (cmd == QLatin1String("help")) {
        if (item.anchor) {
            const QString tip = item.anchor->toolTip();
            QToolTip::showText(item.anchor->mapToGlobal(QPoint(0, item.anchor->height())),
                tip.isEmpty() ? rowName(item) : tip, item.anchor);
        }
    } else if (cmd.startsWith(QLatin1String("key_"))) {
        ok = keyframe(cmd.mid(4), item);
    } else {
        return false;
    }
    if (!ok)
        showMessage(QObject::tr("Cannot do that to %1").arg(rowName(item)));
    return true;
}

} // namespace kk
