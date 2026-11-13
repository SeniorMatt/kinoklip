// SPDX-License-Identifier: GPL-3.0-or-later
// ClipAttributesDialog — Clip > Clip Attributes (media pool items and timeline clips): how the
// file is interpreted. Video: pixel aspect, data levels, field order / deinterlacing,
// rotation. Audio: channel format and routing. Timecode: start timecode override.
#pragma once

#include "core/ClipAttributes.h"
#include "core/MediaItem.h"

#include <QDialog>
#include <QStringList>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;

namespace kk {

struct AppContext;
class Project;

/// Modal dialog filled from one media pool item (the first of the edited ones); result()
/// gives the attributes as entered. Controls that do not apply (no video / no audio stream)
/// are disabled with a tooltip saying why.
/// Thread-safety: GUI thread only.
class ClipAttributesDialog : public QDialog
{
    Q_OBJECT
public:
    /// @p item gives the current values and the file's facts (channels, rate, size);
    /// @p count is the number of items the result is applied to (shown in the title).
    ClipAttributesDialog(const MediaItem& item, int count, QWidget* parent = nullptr);

    /// The attributes as entered.
    [[nodiscard]] ClipAttributes result() const;

    // ---- Controls (tests, screenshots) ----
    [[nodiscard]] QTabWidget* tabs() const { return m_tabs; }
    [[nodiscard]] QComboBox* pixelAspectBox() const { return m_par; }
    [[nodiscard]] QButtonGroup* levelsGroup() const { return m_levels; }
    [[nodiscard]] QComboBox* fieldOrderBox() const { return m_fieldOrder; }
    [[nodiscard]] QCheckBox* deinterlaceBox() const { return m_deinterlace; }
    [[nodiscard]] QComboBox* rotationBox() const { return m_rotation; }
    [[nodiscard]] QComboBox* frameRateBox() const { return m_frameRate; }
    [[nodiscard]] QComboBox* alphaModeBox() const { return m_alphaMode; }
    [[nodiscard]] QComboBox* channelFormatBox() const { return m_format; }
    [[nodiscard]] QComboBox* leftBox() const { return m_left; }
    [[nodiscard]] QComboBox* rightBox() const { return m_right; }
    [[nodiscard]] QPushButton* swapButton() const { return m_swap; }
    [[nodiscard]] QCheckBox* timecodeBox() const { return m_overrideTc; }
    [[nodiscard]] QLineEdit* timecodeEdit() const { return m_timecode; }

private:
    QWidget* buildVideoTab(const MediaItem& item);
    QWidget* buildAudioTab(const MediaItem& item);
    QWidget* buildTimecodeTab(const MediaItem& item);
    void updateAudioEnabled();
    /// Fills a channel combo: Channel 1..N, (Mono: All channels), Mute.
    void fillChannels(QComboBox* box, bool withMix);

    Rational m_tcRate;
    int m_channels = 0;
    ClipAttributes m_initial;
    QTabWidget* m_tabs = nullptr;
    QComboBox* m_par = nullptr;
    QButtonGroup* m_levels = nullptr;
    QComboBox* m_fieldOrder = nullptr;
    QCheckBox* m_deinterlace = nullptr;
    QComboBox* m_rotation = nullptr;
    QComboBox* m_frameRate = nullptr;
    QComboBox* m_alphaMode = nullptr;
    QComboBox* m_format = nullptr;
    QLabel* m_leftLabel = nullptr;
    QComboBox* m_left = nullptr;
    QLabel* m_rightLabel = nullptr;
    QComboBox* m_right = nullptr;
    QPushButton* m_swap = nullptr;
    QCheckBox* m_overrideTc = nullptr;
    QLineEdit* m_timecode = nullptr;
};

namespace clipattributes {

/// Media pool file items of @p ids (timelines, compound clips and subtitles are skipped).
[[nodiscard]] QStringList editableItems(const Project& project, const QStringList& ids);
/// Media items of the clips selected in the timeline (with their linked partners' items,
/// unique, selection order).
[[nodiscard]] QStringList timelineTargets(const AppContext& ctx);
/// Applies @p attributes to the items @p ids as one undo step ("Clip Attributes"); their
/// clips follow (Project::syncDerived()). Returns whether anything changed.
bool apply(Project& project, const QStringList& ids, const ClipAttributes& attributes);
/// Opens the dialog for @p ids (first editable item's values) and applies it on OK.
/// Returns whether anything changed. Does nothing when no id is editable.
bool edit(Project& project, const QStringList& ids, QWidget* parent);

} // namespace clipattributes

} // namespace kk
