// SPDX-License-Identifier: GPL-3.0-or-later
// guide:: implementation: bundled index, URL opener, widget help contexts, "?" buttons.
#include "ui/help/Guide.h"

#include "core/Logging.h"
#include "platform/ChildProcessEnv.h"
#include "ui/common/Page.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/SchemeColors.h"

#include <QAction>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QPainter>
#include <QUrl>

#include <optional>

namespace kk::guide {

namespace {

constexpr char kIndexResource[] = ":/guide/index.json";
constexpr char kContextProperty[] = "kkHelpContext";
constexpr int kButtonSize = 16; ///< px, the "?" buttons

std::optional<GuideIndex>& storedIndex()
{
    static std::optional<GuideIndex> stored;
    return stored;
}

std::function<bool(const QUrl&)>& opener()
{
    static std::function<bool(const QUrl&)> fn;
    return fn;
}

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::guide", text);
}

} // namespace

const GuideIndex& index()
{
    std::optional<GuideIndex>& stored = storedIndex();
    if (!stored)
        reloadBundledIndex();
    return *stored;
}

void setIndexForTesting(const GuideIndex& index)
{
    storedIndex() = index;
}

void reloadBundledIndex()
{
    QString error;
    storedIndex() = GuideIndex::load(QLatin1String(kIndexResource), &error);
    if (!storedIndex()->isValid())
        qCWarning(lcCore).noquote() << "guide index not usable (no \"?\" buttons):" << error;
}

bool openUrl(const QUrl& url)
{
    if (!url.isValid())
        return false;
    if (opener())
        return opener()(url);
    return platform::openUrl(url);
}

bool openArticle(const GuideArticle* article)
{
    const GuideIndex& guideIndex = index();
    return openUrl(article ? guideIndex.url(*article) : guideIndex.startUrl());
}

void setUrlOpenerForTesting(std::function<bool(const QUrl&)> fn)
{
    opener() = std::move(fn);
}

void setHelpContext(QWidget* widget, const QString& key)
{
    if (widget)
        widget->setProperty(kContextProperty, key.isEmpty() ? QVariant() : QVariant(key));
}

QString helpContext(const QWidget* widget)
{
    return widget ? widget->property(kContextProperty).toString() : QString();
}

QStringList contextsAt(const QWidget* widget)
{
    QStringList keys;
    auto add = [&keys](const QString& key) {
        if (!key.isEmpty() && !keys.contains(key))
            keys.append(key);
    };
    for (const QWidget* w = widget; w; w = w->parentWidget()) {
        add(helpContext(w));
        if (const auto* panel = qobject_cast<const PanelFrame*>(w)) {
            if (!panel->panelKey().isEmpty())
                add(QStringLiteral("panel:") + panel->panelKey());
        }
        if (const auto* page = qobject_cast<const Page*>(w))
            add(QStringLiteral("page:") + page->pageId());
        if (w->isWindow())
            break;
    }
    return keys;
}

HelpButton::HelpButton(const GuideArticle& article, QWidget* parent)
    : QAbstractButton(parent)
    , m_articleId(article.id)
{
    setObjectName(QStringLiteral("HelpButton"));
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(kButtonSize, kButtonSize);
    setToolTip(tr("Guide: %1").arg(article.title));
    setAccessibleName(tr("Open the guide: %1").arg(article.title));
    connect(this, &QAbstractButton::clicked, this,
        [this] { openArticle(index().article(m_articleId)); });
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this,
        qOverload<>(&QWidget::update));
}

QSize HelpButton::sizeHint() const
{
    return {kButtonSize, kButtonSize};
}

void HelpButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const SchemeColors& colors = schemeColors();
    const QColor color = m_hover || isDown() ? colors.text : colors.textDim;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF circle = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    p.setPen(QPen(color, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(circle);
    QFont f = font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    p.drawText(circle, Qt::AlignCenter, QStringLiteral("?"));
}

void HelpButton::enterEvent(QEnterEvent* event)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(event);
}

void HelpButton::leaveEvent(QEvent* event)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(event);
}

HelpButton* createHelpButton(const QString& key, QWidget* parent)
{
    const GuideArticle* article = index().articleFor(key);
    return article ? new HelpButton(*article, parent) : nullptr;
}

QAction* createHowToAction(const QString& key, const QString& name, QObject* parent)
{
    const GuideArticle* article = index().articleFor(key);
    if (!article)
        return nullptr;
    auto* action = new QAction(tr("How to use %1…").arg(name), parent);
    action->setObjectName(QStringLiteral("HowToUse"));
    action->setToolTip(article->title);
    QObject::connect(action, &QAction::triggered, action,
        [id = article->id] { openArticle(index().article(id)); });
    return action;
}

} // namespace kk::guide
