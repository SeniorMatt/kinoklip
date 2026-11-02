// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeParamTree — the left side of the Keyframes panel: the property rows grouped like the
// Inspector, each with a curve visibility check box, keyframe navigation (previous / add-remove
// / next, the Inspector's three diamond states) and the current value, editable inline.
#pragma once

#include "ui/keyframes/KeyframeItemModel.h"

#include <QScrollArea>
#include <QSet>

#include <functional>

class QCheckBox;
class QLabel;
class QToolButton;

namespace kk {

struct AppContext;
class KeyframeArrowButton;
class KeyframeButton;
class ValueField;

/// Parameter tree. One fixed-height row per visible KeyframeEntry, so row i lines up with row i
/// of the panel's dope sheet (same row height, same vertical scroll value).
///
/// Rows: master ("Keyframes": previous / next key of everything shown), item (multi-item
/// selections), group (section diamond keying every parameter of the group at the playhead, like
/// the Inspector's section diamonds) and parameter rows (diamond, arrows, value field). Edits are
/// one undo step each (a value-field drag from press to release) through Project, at the
/// playhead's clip-local frame; the diamonds are disabled while the playhead is outside the clip
/// (spec 15.5). A click on a row label selects every key of the row.
/// Thread-safety: GUI thread only.
class KeyframeParamTree : public QScrollArea
{
    Q_OBJECT
public:
    explicit KeyframeParamTree(AppContext& ctx, QWidget* parent = nullptr);
    ~KeyframeParamTree() override;

    /// Row height in px (the dope sheet uses the same).
    static constexpr int kRowHeight = 24;

    /// Shows @p rows (indices into @p entries); rebuilds the row widgets only when the rows
    /// changed (another clip, parameters added, a group collapsed).
    void setRows(const QVector<KeyframeEntry>& entries, const QVector<int>& rows,
        const QSet<QString>& collapsed);
    /// Re-reads values and diamond states (playhead moved, keys edited): O(rows).
    void refresh();
    /// Number of rows shown.
    [[nodiscard]] int rowCount() const { return int(m_rows.size()); }
    /// The entry of row @p row.
    [[nodiscard]] const KeyframeEntry& entryAt(int row) const;
    /// The value field of parameter row @p row (tests), or nullptr.
    [[nodiscard]] ValueField* valueField(int row) const;
    /// The diamond of row @p row (tests), or nullptr.
    [[nodiscard]] KeyframeButton* diamond(int row) const;
    /// The curve check box of row @p row (tests), or nullptr.
    [[nodiscard]] QCheckBox* curveCheck(int row) const;

    /// Asks whether a curve is shown (curve check boxes).
    void setCurveShownProvider(std::function<bool(const ParamId&)> provider)
    {
        m_curveShown = std::move(provider);
    }

signals:
    /// The user expanded / collapsed the item or group @p key.
    void collapseToggled(const QString& key);
    /// The user switched curves on or off.
    void curvesToggled(const QVector<ParamId>& ids, bool shown);
    /// The user clicked the label of a row (its keys were selected; @p ids are its params).
    void rowActivated(const QVector<ParamId>& ids);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Row;
    void buildRow(Row& row, int index);
    void refreshRow(Row& row);
    [[nodiscard]] int64_t localFrame(const ClipData& clip) const;
    [[nodiscard]] bool playheadInside(const ClipData& clip) const;
    void toggleKeys(const Row& row);
    void jump(const Row& row, bool forward);
    void selectRowKeys(const Row& row);
    void editValue(const KeyframeEntry& e, double value);

    AppContext& m_ctx;
    QWidget* m_content = nullptr;
    QVector<KeyframeEntry> m_entries; ///< every entry (summary rows look up their params)
    QHash<QString, int> m_paramIndex; ///< "param:<clip>/<path>" -> index into m_entries
    QVector<Row*> m_rows;
    QSet<QString> m_collapsed;
    std::function<bool(const ParamId&)> m_curveShown;
    bool m_scrubOpen = false; ///< a value-field drag holds an open edit
};

} // namespace kk
