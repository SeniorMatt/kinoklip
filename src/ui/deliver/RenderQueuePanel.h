// SPDX-License-Identifier: GPL-3.0-or-later
// RenderQueuePanel — the Deliver page's right panel: one card per RenderQueue job (name, output,
// progress bar, speed/ETA, edit / cancel / reveal / remove) and Render All / Stop / Clear Finished.
#pragma once

#include "ui/common/PanelFrame.h"

#include <QHash>
#include <QVector>

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
    /// The summary line of the last run (empty / hidden before the first one).
    [[nodiscard]] QString summaryText() const;
    /// Marks the card of job @p id as the one being edited (0: none).
    void setEditingJob(quint64 id);

Q_SIGNALS:
    /// The pencil of job @p id was clicked: load its settings for editing.
    void editRequested(quint64 id);
    /// A render run ended (all jobs done, or stopped); @p summary is the line shown.
    void runFinished(const QString& summary);

private:
    void addCard(quint64 id);
    void removeCard(quint64 id);
    void updateCard(quint64 id);
    void updateButtons();

    RenderQueue* m_queue;
    QHash<quint64, RenderJobCard*> m_cards;
    QVBoxLayout* m_list = nullptr;
    QLabel* m_empty = nullptr;
    QLabel* m_summary = nullptr; ///< result of the last run ("Render complete: 1 job rendered")
    QVector<quint64> m_runStarted; ///< jobs queued when the current run started
    /// The queue started / stopped: remembers the run's jobs, then shows the summary.
    void onRunningChanged(bool running);
    QPushButton* m_renderAll = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_clear = nullptr;
};

} // namespace kk
