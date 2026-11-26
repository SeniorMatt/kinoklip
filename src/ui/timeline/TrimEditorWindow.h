// SPDX-License-Identifier: GPL-3.0-or-later
// TrimEditorWindow — Trim > Trim Editor (Resolve: the Trim Editor): a window of its own that
// shows the frames on both sides of the selected edit point, the outgoing frame on the left
// and the incoming frame on the right, with trim buttons under each side and between them
// (spec 197). The window follows the selected edit point of the timeline and trims it through
// the timeline controller, so every button is one undo step like the nudge keys.
#pragma once

#include "ui/timeline/DragInfo.h"
#include "ui/timeline/TimelineController.h"

#include <QDialog>

class QLabel;
class QPushButton;

namespace kk {

struct AppContext;
class TrimTwoUpView;

/// Non-modal; one per main window, hidden (not deleted) when closed. GUI thread only.
class TrimEditorWindow : public QDialog
{
    Q_OBJECT
public:
    /// The trim steps of the buttons, in frames, shown as -10 -5 -1 +1 +5 +10.
    static constexpr int kSteps[] = {-10, -5, -1, 1, 5, 10};

    /// Which side of the cut a button trims.
    enum class Side {
        Outgoing, ///< the tail of the left clip
        Roll, ///< the cut itself (both clips)
        Incoming ///< the head of the right clip
    };

    TrimEditorWindow(AppContext& ctx, TimelineController* controller, QWidget* parent = nullptr);

    /// The clips on both sides of @p edit on @p data: the tail clip and the clip touching its
    /// end, or the head clip and the clip touching its start (0 = none on that side).
    struct Cut
    {
        quint64 left = 0;
        quint64 right = 0;
    };
    [[nodiscard]] static Cut cutOf(const TimelineData& data, const EditPointSelection& edit);
    /// The two-up pictures for @p edit: the last frame of the left clip, the first frame of
    /// the right clip (empty sides where there is no clip).
    [[nodiscard]] static TrimPreview previewOf(
        const TimelineData& data, const EditPointSelection& edit);

    /// Trims @p side of the selected edit point by @p frames (the buttons). Returns false when
    /// no edit point is selected or that side has no clip.
    bool trim(Side side, int frames);
    /// The edit point before / after the selected one on its track (the Previous / Next Edit
    /// buttons). Returns false when there is none.
    bool step(bool forward);

    [[nodiscard]] TrimTwoUpView* view() const { return m_view; }
    /// The pictures and captions follow the selection and the timeline; called by their signals.
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private:
    AppContext& m_ctx;
    TimelineController* m_controller;
    TrimTwoUpView* m_view = nullptr;
    QLabel* m_hint = nullptr;
    QPushButton* m_buttons[3][6] = {};
    QPushButton* m_previous = nullptr;
    QPushButton* m_next = nullptr;
};

} // namespace kk
