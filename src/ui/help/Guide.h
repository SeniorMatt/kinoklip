// SPDX-License-Identifier: GPL-3.0-or-later
// guide:: — the app's link to the user guide: the bundled index, opening articles, the help
// context of widgets, and the small "?" buttons / "How to use" entries.
#pragma once

#include "ui/help/GuideIndex.h"

#include <QAbstractButton>
#include <QStringList>

#include <functional>

class QAction;
class QObject;
class QUrl;
class QWidget;

namespace kk::guide {

/// The guide index bundled as `:/guide/index.json`, loaded on first use. GUI thread only.
[[nodiscard]] const GuideIndex& index();
/// Replaces the index (tests; widgets built afterwards see it).
void setIndexForTesting(const GuideIndex& index);
/// Reads `:/guide/index.json` again (undoes setIndexForTesting()).
void reloadBundledIndex();

/// Opens @p url in the user's browser (platform::openUrl: the browser gets the user's
/// environment, not the bundle's). Returns whether the opener succeeded.
bool openUrl(const QUrl& url);
/// Opens @p article (or the start page for nullptr).
bool openArticle(const GuideArticle* article);
/// Test hook: replaces the URL opener (an empty function restores platform::openUrl).
void setUrlOpenerForTesting(std::function<bool(const QUrl&)> opener);

/// Help context of a widget: a key of the guide contract ("effect:chroma_key", ...). The
/// innermost widget that has one is the most specific; see contextsAt().
void setHelpContext(QWidget* widget, const QString& key);
[[nodiscard]] QString helpContext(const QWidget* widget);

/// The context keys from @p widget up to its window, innermost first: each widget's own help
/// context, `panel:<key>` of a PanelFrame with a panel key, `page:<id>` of a Page.
[[nodiscard]] QStringList contextsAt(const QWidget* widget);

/// The small "?" button: a dim question mark in a circle (scheme's dim text colour, about
/// 16 px), brighter on hover. Clicking it opens its article.
class HelpButton : public QAbstractButton
{
    Q_OBJECT
public:
    HelpButton(const GuideArticle& article, QWidget* parent = nullptr);
    /// The id of the article it opens.
    [[nodiscard]] QString articleId() const { return m_articleId; }
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString m_articleId;
    bool m_hover = false;
};

/// A "?" button for context @p key, or nullptr when the guide has no article for it.
[[nodiscard]] HelpButton* createHelpButton(const QString& key, QWidget* parent = nullptr);

/// "How to use <name>…" opening the article of context @p key, or nullptr without one.
[[nodiscard]] QAction* createHowToAction(
    const QString& key, const QString& name, QObject* parent = nullptr);

} // namespace kk::guide
