// SPDX-License-Identifier: GPL-3.0-or-later
// CompInspector — the Compositing page Inspector: the selected node's parameters with the
// Edit page Inspector's rows and keyframe buttons (keyframes in comp frames).
#pragma once

#include "core/TimelineData.h"
#include "ui/common/PanelFrame.h"
#include "ui/inspector/ParamEditHost.h"

#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QScrollArea;
class QToolButton;

namespace kk {

struct AppContext;
class CompSession;
struct CompNode;
class ParamGroupBox;
class TrackerPanel;

/// Inspector panel of the Compositing page.
///
/// Header: the node's name, an enable toggle (off = pass-through) and Reset (every parameter
/// back to its default). Body: one ParamGroupBox with a ParamRow per descriptor parameter,
/// the same widgets as the Edit page Inspector.
///
/// **How the rows edit a node.** ParamRow reads and writes a ClipData through its
/// ParamEditHost. This panel is that host: it keeps a stand-in clip whose single effect
/// instance carries the node's ParamSet (ParamScope::Effect, effect id = node id, clip-local
/// frames = comp frames). Every row edit lands in the stand-in and is copied into the node
/// through a CompSession gesture, so a drag or a typed value is exactly one undo step and the
/// rows need no knowledge of compositions.
///
/// Thread-safety: GUI thread only.
class CompInspector : public PanelFrame, public ParamEditHost
{
    Q_OBJECT
public:
    /// Creates the panel for @p session.
    CompInspector(AppContext& ctx, CompSession* session, QWidget* parent = nullptr);

    /// The node shown (0 = none).
    [[nodiscard]] quint64 shownNode() const { return m_node; }
    /// The parameter group (nullptr without a node).
    [[nodiscard]] ParamGroupBox* group() const { return m_group; }
    /// MediaIn nodes: the Source picker (the clip's own picture or a media pool item), else
    /// null (spec 42b.30).
    [[nodiscard]] QComboBox* sourcePicker() const { return m_source; }
    /// The Tracker controls (a Tracker node must be shown to see them).
    [[nodiscard]] TrackerPanel* trackerPanel() const { return m_tracker; }
    /// The header's enable toggle (tests).
    [[nodiscard]] QCheckBox* enableToggle() const { return m_enable; }

    // ---- ParamEditHost ----
    [[nodiscard]] const ClipData* clip(quint64 clipId) const override;
    [[nodiscard]] int64_t localFrame(quint64 clipId) const override;
    void beginParamEdit(const QString& label) override;
    void endParamEdit() override;
    void modifyClip(quint64 clipId, const std::function<void(ClipData&)>& mutate) override;
    void editClip(const QString& label, quint64 clipId,
        const std::function<void(ClipData&)>& mutate) override;
    void seekLocalFrame(quint64 clipId, int64_t localFrame) override;
    void setActiveParam(const ParamRef& ref, const ParamDescriptor& desc) override;
    [[nodiscard]] QColor pickColor(const QColor& initial, const QString& title) override;
    [[nodiscard]] QString pickFile(
        const QString& current, const QString& title, const QString& filter) override;

    /// Id of the stand-in clip the rows address.
    static constexpr quint64 kProxyClipId = 1;

private:
    void rebuild();
    void refreshRows();
    /// Copies the node's state into the stand-in clip.
    void syncProxy();

    AppContext& m_ctx;
    QPointer<CompSession> m_session;
    quint64 m_node = 0;
    QString m_nodeType;
    ClipData m_proxy;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_header = nullptr;
    QLabel* m_name = nullptr;
    QCheckBox* m_enable = nullptr;
    QToolButton* m_reset = nullptr;
    ParamGroupBox* m_group = nullptr;
    QComboBox* m_source = nullptr; ///< sourcePicker()
    void fillSourcePicker(const CompNode& node);
    QWidget* m_empty = nullptr;
    TrackerPanel* m_tracker = nullptr; ///< owned by the inspector, shown inside the scroll area
    int m_openEdits = 0;
};

} // namespace kk
