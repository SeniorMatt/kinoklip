// SPDX-License-Identifier: GPL-3.0-or-later
// VimTimelineHost — connects the vim key layer (ui/common/VimInput.h) to a timeline: it only
// triggers registry actions, moves the playhead and sets the selection or the target track.
// All edits are the timeline's own actions, one undo step each.
#pragma once

#include "ui/common/VimInput.h"

#include <functional>

namespace kk {

struct AppContext;
class TimelineController;

class VimTimelineHost final : public VimHost
{
public:
    VimTimelineHost(AppContext& ctx, TimelineController* controller);

    bool trigger(const QString& actionId) override;
    [[nodiscard]] int64_t playhead() const override;
    void setPlayhead(int64_t frame) override;
    bool selectClipsAtPlayhead(int count) override;
    void selectRange(int64_t a, int64_t b) override;
    void clearSelection() override;
    void moveTarget(int delta) override;
    void openCommandLine() override;
    void openSearch() override;
    void runEx(const QString& line) override;

    /// Called for `:` and `/` (set by the window; empty = ignored).
    std::function<void()> onCommandLine;
    std::function<void()> onSearch;
    /// Called for a key bound to an ex command (set by the window).
    std::function<void(const QString&)> onEx;

private:
    AppContext& m_ctx;
    TimelineController* m_controller;
};

} // namespace kk
