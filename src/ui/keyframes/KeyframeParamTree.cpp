// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeParamTree implementation: row widgets, refresh, keyframe toggles and value edits.
#include "ui/keyframes/KeyframeParamTree.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/ValueField.h"
#include "ui/keyframes/CurveView.h"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace kk {

namespace ko = keyframeops;

namespace {
constexpr int kIndentPx = 14; ///< per tree level
constexpr int kExpandSize = 16;
constexpr int kValueFieldWidth = 84;
constexpr int kSwatchWidth = 3; ///< curve colour bar left of a parameter's check box

/// Thin vertical bar in a curve's colour (identifies the curve in the overlaid view).
class Swatch : public QWidget
{
public:
    Swatch(const QColor& color, QWidget* parent)
        : QWidget(parent)
        , m_color(color)
    {
        setFixedSize(kSwatchWidth, KeyframeParamTree::kRowHeight - 8);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), m_color);
    }

private:
    QColor m_color;
};
} // namespace

struct KeyframeParamTree::Row
{
    KeyframeEntry entry;
    QWidget* widget = nullptr;
    QToolButton* expand = nullptr;
    QCheckBox* check = nullptr;
    QLabel* label = nullptr;
    KeyframeArrowButton* prev = nullptr;
    KeyframeButton* diamond = nullptr;
    KeyframeArrowButton* next = nullptr;
    ValueField* field = nullptr;
};

KeyframeParamTree::KeyframeParamTree(AppContext& ctx, QWidget* parent)
    : QScrollArea(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("KeyframeParamTree"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The dope sheet beside the tree has no scroll bar of its own: the tree's scrolls both.
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_content = new QWidget;
    auto* layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(0, 1, 0, 1); // the dope sheet's 1 px top padding
    layout->setSpacing(0);
    layout->addStretch(1);
    setWidget(m_content);
    m_curveShown = [](const ParamId&) { return true; };
}

KeyframeParamTree::~KeyframeParamTree()
{
    if (m_scrubOpen)
        m_ctx.project->cancelEdit();
    qDeleteAll(m_rows);
}

const KeyframeEntry& KeyframeParamTree::entryAt(int row) const
{
    return m_rows[row]->entry;
}

ValueField* KeyframeParamTree::valueField(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row]->field : nullptr;
}

KeyframeButton* KeyframeParamTree::diamond(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row]->diamond : nullptr;
}

QCheckBox* KeyframeParamTree::curveCheck(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row]->check : nullptr;
}

void KeyframeParamTree::setRows(
    const QVector<KeyframeEntry>& entries, const QVector<int>& rows, const QSet<QString>& collapsed)
{
    m_entries = entries;
    m_paramIndex.clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].kind == KeyRow::Kind::Param)
            m_paramIndex.insert(m_entries[i].key, i);
    }
    // Same rows (and the same collapse states of their headers): keep the widgets, which keeps
    // a value field that is being dragged alive.
    bool same = rows.size() == m_rows.size();
    for (int i = 0; same && i < rows.size(); ++i) {
        const KeyframeEntry& e = m_entries[rows[i]];
        same = e.sameRow(m_rows[i]->entry)
            && collapsed.contains(e.key) == m_collapsed.contains(e.key);
    }
    m_collapsed = collapsed;
    if (same) {
        for (int i = 0; i < rows.size(); ++i)
            m_rows[i]->entry = m_entries[rows[i]];
        refresh();
        return;
    }
    // A value-field drag in progress keeps its edit; the field itself goes away.
    if (m_scrubOpen) {
        m_scrubOpen = false;
        m_ctx.project->endEdit();
    }
    auto* layout = static_cast<QVBoxLayout*>(m_content->layout());
    for (Row* r : std::as_const(m_rows)) {
        r->widget->hide();
        r->widget->deleteLater(); // may be the sender of the signal that caused the rebuild
        delete r;
    }
    m_rows.clear();
    for (int i = 0; i < rows.size(); ++i) {
        auto* row = new Row;
        row->entry = m_entries[rows[i]];
        buildRow(*row, i);
        layout->insertWidget(i, row->widget);
        m_rows.append(row);
    }
    refresh();
}

void KeyframeParamTree::buildRow(Row& row, int index)
{
    const KeyframeEntry& e = row.entry;
    row.widget = new QWidget(m_content);
    row.widget->setFixedHeight(kRowHeight);
    row.widget->setAutoFillBackground(e.kind != KeyRow::Kind::Param);
    if (e.kind != KeyRow::Kind::Param) {
        // Header rows are a shade darker (like Inspector section headers).
        QPalette pal = row.widget->palette();
        pal.setColor(QPalette::Window,
            keyframeMix(pal.color(QPalette::Window), pal.color(QPalette::WindowText),
                e.kind == KeyRow::Kind::Group ? 0.05 : 0.09));
        row.widget->setPalette(pal);
    }
    auto* h = new QHBoxLayout(row.widget);
    h->setContentsMargins(metrics::kSpaceS + e.depth * kIndentPx, 0, metrics::kSpaceS, 0);
    h->setSpacing(metrics::kSpaceS);

    const bool header = e.kind == KeyRow::Kind::Group || e.kind == KeyRow::Kind::Item;
    if (header) {
        row.expand = new QToolButton(row.widget);
        row.expand->setAutoRaise(true);
        row.expand->setFixedSize(kExpandSize, kExpandSize);
        row.expand->setFocusPolicy(Qt::NoFocus);
        row.expand->setArrowType(m_collapsed.contains(e.key) ? Qt::RightArrow : Qt::DownArrow);
        row.expand->setToolTip(m_collapsed.contains(e.key) ? tr("Expand") : tr("Collapse"));
        const QString key = e.key;
        connect(
            row.expand, &QToolButton::clicked, this, [this, key] { emit collapseToggled(key); });
        h->addWidget(row.expand);
    } else if (e.kind == KeyRow::Kind::Param) {
        h->addWidget(new Swatch(CurveView::curveColor(e.colorIndex), row.widget));
    }
    row.check = new QCheckBox(row.widget);
    row.check->setFocusPolicy(Qt::NoFocus);
    row.check->setToolTip(tr("Show the curve in the curve editor"));
    connect(row.check, &QCheckBox::clicked, this, [this, index] {
        const KeyframeEntry& entry = m_rows[index]->entry;
        const bool allShown = std::all_of(entry.params.cbegin(), entry.params.cend(),
            [this](const ParamId& id) { return m_curveShown(id); });
        emit curvesToggled(entry.params, !allShown);
        refresh();
    });
    h->addWidget(row.check);

    row.label = new QLabel(e.label, row.widget);
    row.label->setProperty("kkRow", index);
    row.label->installEventFilter(this);
    row.label->setMinimumWidth(20);
    row.label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (e.kind != KeyRow::Kind::Param) {
        QFont f = row.label->font();
        f.setBold(true);
        row.label->setFont(f);
    }
    h->addWidget(row.label, 1);

    row.prev = new KeyframeArrowButton(false, row.widget);
    row.prev->setToolTip(tr("Previous keyframe"));
    connect(
        row.prev, &QAbstractButton::clicked, this, [this, index] { jump(*m_rows[index], false); });
    h->addWidget(row.prev);
    if (e.kind == KeyRow::Kind::Param || e.kind == KeyRow::Kind::Group) {
        row.diamond = new KeyframeButton(row.widget);
        row.diamond->setToolTip(e.kind == KeyRow::Kind::Group
                ? tr("Add or remove a keyframe on every parameter of the group at the playhead")
                : tr("Add or remove a keyframe at the playhead"));
        connect(row.diamond, &QAbstractButton::clicked, this,
            [this, index] { toggleKeys(*m_rows[index]); });
        h->addWidget(row.diamond);
    }
    row.next = new KeyframeArrowButton(true, row.widget);
    row.next->setToolTip(tr("Next keyframe"));
    connect(
        row.next, &QAbstractButton::clicked, this, [this, index] { jump(*m_rows[index], true); });
    h->addWidget(row.next);

    // A mask shape has keys but no number to type (it is edited in the viewer).
    const bool shapeRow = e.kind == KeyRow::Kind::Param && !e.params.isEmpty()
        && ko::isShapePath(e.params.front().path);
    if (e.kind == KeyRow::Kind::Param && !shapeRow) {
        auto* f = new ValueField(row.widget);
        f->setRange(e.desc.min, e.desc.max);
        f->setStep(e.desc.step);
        const int decimals = e.desc.type == ParamType::Int || e.desc.step >= 1.0
            ? 0
            : std::clamp(int(std::ceil(-std::log10(e.desc.step) - 1e-9)), 0, 4);
        f->setDecimals(decimals);
        f->setSuffix(e.desc.unit);
        // The column width, unless the widest value with its unit needs more (never clipped).
        f->setFixedWidth(std::max(kValueFieldWidth, f->sizeHint().width()));
        row.field = f;
        // One undo step per drag (press to release) and per typed / wheel value.
        connect(f, &ValueField::scrubStarted, this, [this, index] {
            if (m_scrubOpen)
                return;
            m_scrubOpen = true;
            m_ctx.project->beginEdit(tr("Change %1").arg(m_rows[index]->entry.label));
        });
        // The field shows and takes shown values (Position Y +up, spec 24.16).
        connect(f, &ValueField::valueScrubbed, this, [this, index](double v) {
            editValue(m_rows[index]->entry, m_rows[index]->entry.desc.displayed(v));
        });
        connect(f, &ValueField::scrubFinished, this, [this] {
            if (!m_scrubOpen)
                return;
            m_scrubOpen = false;
            m_ctx.project->endEdit();
        });
        connect(f, &ValueField::valueCommitted, this, [this, index](double v) {
            const KeyframeEntry& entry = m_rows[index]->entry;
            m_ctx.project->beginEdit(tr("Change %1").arg(entry.label));
            editValue(entry, entry.desc.displayed(v));
            m_ctx.project->endEdit();
        });
        h->addWidget(f);
    } else {
        h->addSpacing(kValueFieldWidth + metrics::kSpaceS);
    }
}

int64_t KeyframeParamTree::localFrame(const ClipData& clip) const
{
    const int64_t local = m_ctx.project->timeline()->playhead() - clip.start;
    return std::clamp<int64_t>(local, 0, std::max<int64_t>(0, clip.duration - 1));
}

bool KeyframeParamTree::playheadInside(const ClipData& clip) const
{
    const int64_t local = m_ctx.project->timeline()->playhead() - clip.start;
    return local >= 0 && local < clip.duration;
}

void KeyframeParamTree::editValue(const KeyframeEntry& e, double value)
{
    // Inspector semantics (spec 15.3): an animated value keys at the playhead, a plain value
    // changes its constant. Inside an open edit; notifies views live.
    Timeline* timeline = m_ctx.project->timeline();
    if (ClipData* c = editops::clipById(timeline->mutableData(), e.ref.clipId)) {
        paramaccess::setScalar(*c, e.ref, e.desc, localFrame(*c), value);
        timeline->notifyChanged();
    }
}

void KeyframeParamTree::refresh()
{
    for (Row* r : std::as_const(m_rows))
        refreshRow(*r);
}

void KeyframeParamTree::refreshRow(Row& row)
{
    const KeyframeEntry& e = row.entry;
    const TimelineData& data = m_ctx.project->timeline()->data();
    // Curve check box: checked, unchecked or partial over the row's parameters.
    int shown = 0;
    for (const ParamId& id : e.params)
        shown += m_curveShown(id) ? 1 : 0;
    const Qt::CheckState state = shown == 0 ? Qt::Unchecked
        : shown == e.params.size()          ? Qt::Checked
                                            : Qt::PartiallyChecked;
    if (row.check->checkState() != state)
        row.check->setCheckState(state);
    row.check->setEnabled(!e.params.isEmpty());

    // Keyframe state at the playhead.
    bool anyAnimated = false;
    bool allOnKey = !e.params.isEmpty();
    bool inside = false;
    for (const ParamId& id : e.params) {
        const ClipData* c = editops::clipById(data, id.clipId);
        if (!c)
            continue;
        inside = inside || playheadInside(*c);
        const AnimatedValue* v = ko::findValue(*c, id.path);
        const bool animated = v && v->isAnimated();
        anyAnimated = anyAnimated || animated;
        if (!(animated && v->hasKeyAt(m_ctx.project->timeline()->playhead() - c->start)))
            allOnKey = false;
    }
    if (row.diamond) {
        using S = KeyframeButton::State;
        row.diamond->setState(allOnKey ? S::OnKey : anyAnimated ? S::Animated : S::NotAnimated);
        row.diamond->setEnabled(inside); // spec 15.5
    }
    row.prev->setEnabled(anyAnimated);
    row.next->setEnabled(anyAnimated);
    if (row.field && !row.field->isScrubbing()) {
        if (const ClipData* c = editops::clipById(data, e.ref.clipId))
            row.field->setValue(
                e.desc.displayed(paramaccess::scalar(*c, e.ref, e.desc, localFrame(*c))));
    }
}

void KeyframeParamTree::toggleKeys(const Row& row)
{
    // The parameter rows behind the row (a group keys all of them, like the Inspector's
    // section diamond): remove every key at the playhead if all have one, else add the
    // missing ones.
    QVector<const KeyframeEntry*> params;
    for (const ParamId& id : row.entry.params) {
        const auto it
            = m_paramIndex.constFind(QStringLiteral("param:%1/%2").arg(id.clipId).arg(id.path));
        if (it != m_paramIndex.cend())
            params.append(&m_entries[*it]);
    }
    if (params.isEmpty())
        return;
    const TimelineData& data = m_ctx.project->timeline()->data();
    bool all = true;
    for (const KeyframeEntry* e : params) {
        const ClipData* c = editops::clipById(data, e->ref.clipId);
        const AnimatedValue* v = c ? paramaccess::animated(*c, e->ref) : nullptr;
        if (!(c && v && v->hasKeyAt(localFrame(*c))))
            all = false;
    }
    m_ctx.project->beginEdit(all ? tr("Remove Keyframe") : tr("Add Keyframe"));
    TimelineData& mutableData = m_ctx.project->timeline()->mutableData();
    for (const KeyframeEntry* e : params) {
        ClipData* c = editops::clipById(mutableData, e->ref.clipId);
        if (!c || !playheadInside(*c))
            continue;
        const AnimatedValue* v = paramaccess::animated(*c, e->ref);
        const bool has = v && v->hasKeyAt(localFrame(*c));
        if (has == all) // remove existing keys / add missing ones
            paramaccess::toggleKey(*c, e->ref, e->desc, localFrame(*c));
    }
    m_ctx.project->endEdit();
}

void KeyframeParamTree::jump(const Row& row, bool forward)
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    std::optional<int64_t> best;
    for (const ParamId& id : row.entry.params) {
        const ClipData* c = editops::clipById(data, id.clipId);
        const AnimatedValue* v = c ? ko::findValue(*c, id.path) : nullptr;
        if (!v)
            continue;
        const auto f = forward ? v->nextKeyFrame(playhead - c->start)
                               : v->previousKeyFrame(playhead - c->start);
        if (!f)
            continue;
        const int64_t t = c->start + *f;
        if (!best || (forward ? t < *best : t > *best))
            best = t;
    }
    if (best)
        m_ctx.project->timeline()->setPlayhead(std::max<int64_t>(0, *best));
}

void KeyframeParamTree::selectRowKeys(const Row& row)
{
    QVector<KeyframeRef> refs;
    const TimelineData& data = m_ctx.project->timeline()->data();
    for (const ParamId& id : row.entry.params) {
        const ClipData* c = editops::clipById(data, id.clipId);
        const AnimatedValue* v = c ? ko::findValue(*c, id.path) : nullptr;
        if (!v)
            continue;
        for (const Keyframe& k : v->keys)
            refs.append({id.clipId, id.path, k.frame});
    }
    m_ctx.selection->setKeyframes(refs);
    emit rowActivated(row.entry.params);
}

bool KeyframeParamTree::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        const QVariant v = watched->property("kkRow");
        auto* me = static_cast<QMouseEvent*>(event);
        if (v.isValid() && me->button() == Qt::LeftButton) {
            const int index = v.toInt();
            if (index >= 0 && index < m_rows.size())
                selectRowKeys(*m_rows[index]);
            return true;
        }
    }
    return QScrollArea::eventFilter(watched, event);
}

} // namespace kk
