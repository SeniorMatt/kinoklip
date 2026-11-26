// SPDX-License-Identifier: GPL-3.0-or-later
// AudioBounce — Timeline > Bounce Mix to Track and Bounce Selected Tracks (spec 176): renders
// the mix of the timeline (the output bus, as heard and exported), or each chosen audio track
// after its effects and fader, into a 24-bit 48 kHz stereo WAV file with the real mixer, puts
// the file into the media pool (bin "Bounced Audio") and places it as one clip on a new audio
// track. The tracks it came from stay as they are. The range is the In / Out marks, else the
// whole timeline. One undo step. Used by the menu actions, the command line and the MCP server.
#pragma once

#include "core/MixerOps.h"
#include "core/TimelineData.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

class QWidget;

namespace kk {

struct AppContext;
class Project;
class SelectionModel;

namespace audiobounce {

struct Options
{
    QString folder; ///< where the WAV files are written (created when missing)
    bool mix = false; ///< bounce the main mix; else every track of @c trackIds
    QVector<quint64> trackIds; ///< audio tracks (TrackData::id) to bounce, one new track each
    /// Called with every written file before it is added to the project: may copy it into a
    /// single-file project and return the path to use ("" = failed). Empty = use the file.
    std::function<QString(const QString& wavPath)> store;
};

struct Result
{
    bool ok = false;
    bool cancelled = false;
    QString error;
    QStringList files; ///< files written
    QVector<quint64> newTrackIds; ///< the audio tracks that were added, in order
};

/// Bounces as @p options says. @p progress (may be empty) gets the fraction done of the whole
/// job; @p cancel (may be null) is polled between blocks (nothing is changed then and the
/// files written are removed). Runs on the GUI thread's project but renders from a snapshot.
[[nodiscard]] Result bounce(Project& project, const Options& options,
    const std::atomic<bool>* cancel = nullptr, const std::function<void(double)>& progress = {});

/// Ids of the audio tracks "Bounce Selected Tracks" works on: the audio tracks that hold a
/// selected clip, or, with no clip selected, the audio tracks whose track selector (Auto Select)
/// is on. Muted tracks are included (they are bounced as the mixer plays them: silent).
[[nodiscard]] QVector<quint64> selectedAudioTracks(
    const TimelineData& timeline, const SelectionModel& selection);

/// Folder for the bounced files of @p project: "<project folder>/Bounced Audio", the
/// application's data folder when the project was never saved, a temporary folder when the
/// project is a single-file project (@p stored).
[[nodiscard]] QString bounceFolder(const Project& project, bool stored);

/// The menu action: bounces the mix (@p mix) or the selected audio tracks of the open project
/// behind a progress dialog with Cancel, into the project's own storage (a single-file project
/// keeps the files inside). Tells the user what went wrong. Returns the number of tracks added.
int runInteractive(AppContext& ctx, QWidget* parent, bool mix);

} // namespace audiobounce
} // namespace kk
