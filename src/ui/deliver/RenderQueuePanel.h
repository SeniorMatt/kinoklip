// SPDX-License-Identifier: GPL-3.0-or-later
// RenderQueuePanel — the Deliver page's right panel: one card per RenderQueue job (name, output,
// progress bar, speed/ETA, cancel / reveal / remove) and Render All / Stop / Clear Finished.
#pragma once

#include "ui/common/PanelFrame.h"

#include <QHash>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace kk {

class RenderQueue;
class RenderJobCard;

/// View of a RenderQueue (does not own it).
class RenderQueuePanel : public PanelFrame
{
    Q_OBJECT
public:
    /// @p queue must outlive the panel.
    explicit RenderQueuePanel(RenderQueue* queue, QWidget* parent = nullptr);

private:
    void addCard(quint64 id);
    void removeCard(quint64 id);
    void updateCard(quint64 id);
    void updateButtons();

    RenderQueue* m_queue;
    QHash<quint64, RenderJobCard*> m_cards;
    QVBoxLayout* m_list = nullptr;
    QLabel* m_empty = nullptr;
    QPushButton* m_renderAll = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_clear = nullptr;
};

} // namespace kk
