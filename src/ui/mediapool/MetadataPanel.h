// SPDX-License-Identifier: GPL-3.0-or-later
// MetadataPanel — clip details (probe result, read-only) and editable user metadata of the
// media items selected in the media pool, with batch editing of several items.
#pragma once

#include "core/MediaItem.h"
#include "ui/common/PanelFrame.h"

#include <QStringList>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedWidget;

namespace kk {

struct AppContext;
class MediaPool;
class MediaPoolPanel;

/// The Metadata panel (Resolve: Edit / Media page, right column).
///
/// Groups:
/// - **Clip Details** (read-only, from probing): name, file, folder, size or offline state,
///   container, codec, resolution, frame rate, duration, audio channels / rate, start
///   timecode, clip colour, flags, usage.
/// - **Shot and Scene** (editable): scene, shot, take, camera, angle, good take.
/// - **Notes** (editable): description, comments, keywords (comma separated), rating.
///
/// With several items selected the fields show the shared value, or stay empty with a
/// "multiple values" hint; editing a field writes that field (only) to every selected item,
/// as one undo step. Power bin items are edited directly (saved at once, not undoable).
/// Follows renames, undo and removal. Thread-safety: GUI thread only.
class MetadataPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// One editable metadata field.
    enum class Field {
        Scene,
        Shot,
        Take,
        Camera,
        Angle,
        GoodTake,
        Description,
        Comments,
        Keywords,
        Rating
    };

    /// Creates an empty panel.
    MetadataPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Follows the selection of @p pool (single item and batch); also finds its power bin
    /// items. Replaces the plain showMedia() wiring.
    void attach(MediaPoolPanel* pool);

    /// Shows the metadata of media item @p mediaId (empty = none).
    void showMedia(const QString& mediaId);
    /// Shows the items @p ids (several = batch editing; empty = none).
    void showMediaItems(const QStringList& ids);
    /// Items shown.
    [[nodiscard]] const QStringList& shownItems() const { return m_ids; }

    /// Writes @p value into field @p field of every shown item (one undo step). Text fields
    /// take a QString (keywords: comma separated), GoodTake a bool, Rating an int 0 - 5.
    void applyField(Field field, const QVariant& value);
    /// The editor widget of @p field (tests): QLineEdit, QPlainTextEdit, QCheckBox or QComboBox.
    [[nodiscard]] QWidget* editor(Field field) const;

protected:
    /// Commits the comments editor when it loses the focus.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildEditors();
    void refresh();
    void fillDetails(const QVector<const MediaItem*>& items);
    void fillEditors(const QVector<const MediaItem*>& items);
    [[nodiscard]] const MediaItem* lookup(const QString& id, MediaPool** pool = nullptr) const;

    AppContext& m_ctx;
    MediaPoolPanel* m_pool = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_heading = nullptr;
    QFormLayout* m_details = nullptr;
    QLineEdit* m_scene = nullptr;
    QLineEdit* m_shot = nullptr;
    QLineEdit* m_take = nullptr;
    QLineEdit* m_camera = nullptr;
    QLineEdit* m_angle = nullptr;
    QCheckBox* m_goodTake = nullptr;
    QLineEdit* m_description = nullptr;
    QPlainTextEdit* m_comments = nullptr;
    QLineEdit* m_keywords = nullptr;
    QComboBox* m_rating = nullptr;
    QStringList m_ids; ///< items shown (refreshed when a pool changes)
    bool m_filling = false; ///< editors are being filled: ignore their change signals
};

} // namespace kk
