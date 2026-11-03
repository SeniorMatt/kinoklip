// SPDX-License-Identifier: GPL-3.0-or-later
// CompInspector implementation: the stand-in clip, the header and the edit protocol.
#include "ui/comp/CompInspector.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "platform/FileDialogs.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompSession.h"
#include "ui/inspector/InspectorGlyphs.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

CompInspector::CompInspector(AppContext& ctx, CompSession* session, QWidget* parent)
    : PanelFrame(tr("Inspector"), parent)
    , m_ctx(ctx)
    , m_session(session)
{
    setObjectName(QStringLiteral("CompInspector"));
    m_proxy.id = kProxyClipId;
    m_proxy.kind = ClipKind::Generator;

    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header: name | enable | reset.
    m_header = new QWidget;
    m_header->setObjectName(QStringLiteral("CompInspectorHeader"));
    auto* header = new QHBoxLayout(m_header);
    header->setContentsMargins(metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM,
        metrics::kSpaceS);
    m_enable = new QCheckBox;
    m_enable->setObjectName(QStringLiteral("CompNodeEnable"));
    m_enable->setToolTip(tr("Enabled (off: pass-through, the main input goes straight through)"));
    m_name = new QLabel;
    m_name->setObjectName(QStringLiteral("CompNodeName"));
    QFont bold = m_name->font();
    bold.setBold(true);
    m_name->setFont(bold);
    m_reset = new QToolButton;
    m_reset->setObjectName(QStringLiteral("CompNodeReset"));
    m_reset->setIcon(inspectorGlyph(InspectorGlyph::Reset));
    m_reset->setToolTip(tr("Reset all parameters of the node"));
    m_reset->setAutoRaise(true);
    header->addWidget(m_enable);
    header->addWidget(m_name, 1);
    header->addWidget(m_reset);
    layout->addWidget(m_header);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("InspectorScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(m_scroll, 1);
    m_empty = createPlaceholderContent(tr("Select a node to see its parameters"));
    layout->addWidget(m_empty, 1);
    setContentWidget(body);

    connect(m_enable, &QCheckBox::clicked, this, [this](bool on) {
        if (m_session && m_node)
            m_session->setNodeEnabled(m_node, on);
    });
    connect(m_reset, &QToolButton::clicked, this, [this] {
        if (!m_session || !m_node)
            return;
        const quint64 id = m_node;
        const CompNode* n = m_session->composition().node(id);
        m_session->edit(tr("Reset %1").arg(n ? n->name : QString()), [id](Composition& c) {
            CompNode* node = c.node(id);
            const compadapter::NodeTypeInfo* info
                = node ? compadapter::nodeType(node->type) : nullptr;
            if (!info || node->locked)
                return false;
            node->params = {};
            for (const ParamDescriptor& p : info->params) {
                if (p.isScalar())
                    node->params.setBase(p.key, p.def);
                else
                    node->params.setStatic(p.key, p.defStatic);
            }
            return true;
        });
    });
    connect(session, &CompSession::selectionChanged, this, &CompInspector::rebuild);
    connect(session, &CompSession::clipChanged, this, &CompInspector::rebuild);
    connect(session, &CompSession::compositionChanged, this, &CompInspector::rebuild);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, &CompInspector::refreshRows);
    rebuild();
}

void CompInspector::syncProxy()
{
    const CompNode* n = m_session ? m_session->composition().node(m_node) : nullptr;
    m_proxy.duration = std::max<int64_t>(1, m_session ? m_session->compFrameCount() : 1);
    m_proxy.effects.clear();
    if (!n)
        return;
    EffectInstance fx;
    fx.id = n->id;
    fx.type = n->type;
    fx.enabled = n->enabled;
    fx.params = n->params;
    m_proxy.effects.append(fx);
}

void CompInspector::rebuild()
{
    const quint64 node = m_session ? m_session->currentNode() : 0;
    const CompNode* n = m_session ? m_session->composition().node(node) : nullptr;
    const QString type = n ? n->type : QString();
    syncProxy();
    if (node != m_node || type != m_nodeType || (n && !m_group)) {
        m_node = n ? node : 0;
        m_nodeType = type;
        m_proxy.effects.clear();
        syncProxy();
        delete m_scroll->takeWidget();
        m_group = nullptr;
        if (n) {
            const compadapter::NodeTypeInfo* info = compadapter::nodeType(n->type);
            m_group = new ParamGroupBox(info ? info->name : n->type);
            m_group->setObjectName(QStringLiteral("CompNodeParams"));
            if (info) {
                for (const ParamDescriptor& desc : info->params) {
                    ParamRef ref;
                    ref.clipId = kProxyClipId;
                    ref.scope = ParamScope::Effect;
                    ref.effectId = n->id;
                    ref.key = desc.key;
                    m_group->addParam(ref, desc, *this);
                }
            }
            auto* holder = new QWidget;
            auto* l = new QVBoxLayout(holder);
            l->setContentsMargins(0, 0, 0, 0);
            l->addWidget(m_group);
            l->addStretch(1);
            m_scroll->setWidget(holder);
        }
    }
    m_header->setVisible(n);
    m_scroll->setVisible(n);
    m_empty->setVisible(!n);
    if (n) {
        m_name->setText(n->name);
        const QSignalBlocker block(m_enable);
        m_enable->setChecked(n->enabled);
        m_enable->setEnabled(n->id != m_session->composition().mediaOutId);
        m_reset->setEnabled(!n->locked);
        if (m_group)
            m_group->setEnabled(!n->locked);
    }
    refreshRows();
}

void CompInspector::refreshRows()
{
    if (!m_group || !m_session)
        return;
    syncProxy();
    m_group->refresh(m_proxy, m_session->compFrame(), true);
}

// ---- ParamEditHost ----

const ClipData* CompInspector::clip(quint64 clipId) const
{
    return clipId == kProxyClipId && !m_proxy.effects.isEmpty() ? &m_proxy : nullptr;
}

int64_t CompInspector::localFrame(quint64) const
{
    return m_session ? m_session->compFrame() : 0;
}

void CompInspector::beginParamEdit(const QString& label)
{
    ++m_openEdits;
    if (m_session)
        m_session->beginGesture(label);
}

void CompInspector::endParamEdit()
{
    if (m_openEdits <= 0)
        return;
    --m_openEdits;
    if (m_session)
        m_session->endGesture();
}

void CompInspector::modifyClip(quint64 clipId, const std::function<void(ClipData&)>& mutate)
{
    if (clipId != kProxyClipId || m_proxy.effects.isEmpty() || !m_session)
        return;
    mutate(m_proxy);
    if (m_proxy.effects.isEmpty())
        return;
    const ParamSet params = m_proxy.effects.first().params;
    const quint64 id = m_node;
    m_session->modifyLive([&](Composition& c) {
        if (CompNode* n = c.node(id); n && !n->locked)
            n->params = params;
    });
}

void CompInspector::editClip(
    const QString& label, quint64 clipId, const std::function<void(ClipData&)>& mutate)
{
    beginParamEdit(label);
    modifyClip(clipId, mutate);
    endParamEdit();
}

void CompInspector::seekLocalFrame(quint64, int64_t frame)
{
    if (m_session)
        m_session->seekCompFrame(frame);
}

void CompInspector::setActiveParam(const ParamRef&, const ParamDescriptor&) { }

QColor CompInspector::pickColor(const QColor& initial, const QString& title)
{
    return QColorDialog::getColor(initial, this, title);
}

QString CompInspector::pickFile(const QString& current, const QString& title, const QString& filter)
{
    return platform::getOpenFileName(this, title, filter, QStringLiteral("compositing/paramFile"),
        current.isEmpty() ? QString() : QFileInfo(current).absolutePath());
}

} // namespace kk
