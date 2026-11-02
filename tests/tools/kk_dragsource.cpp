// SPDX-License-Identifier: GPL-3.0-or-later
// kk_dragsource -- a tiny stand-in for Dolphin in the nested Wayland session tests
// (scripts/wayland_session.sh, scripts/wayland_dnd_check.sh): a window that starts a real
// QDrag of the files given on the command line when it is dragged with the left button.
//
//   kk_dragsource [--portal-only] file...
//
// The payload is what Dolphin offers (KUrlMimeData): text/uri-list and
// application/x-kde4-urilist; Qt Wayland adds the document-portal key
// application/vnd.portal.filetransfer itself. With --portal-only only a (fake) portal key is
// offered, like a sandboxed source. Prints "drop-result <action>" when the drag ends, then
// exits.
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTextStream>
#include <QUrl>
#include <QWidget>

namespace {

class Source final : public QWidget
{
public:
    QList<QUrl> urls;
    bool portalOnly = false;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x30, 0x60, 0xa0));
        p.setPen(Qt::white);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("drag source"));
    }
    void mousePressEvent(QMouseEvent* event) override { m_press = event->position().toPoint(); }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || m_started
            || (event->position().toPoint() - m_press).manhattanLength() < 4)
            return;
        m_started = true;
        auto* data = new QMimeData;
        if (portalOnly) {
            // The key is meaningless without a real document portal; the receiver must not
            // crash and must ignore it gracefully.
            data->setData(QStringLiteral("application/vnd.portal.filetransfer"), "kk-test-key");
        } else {
            data->setUrls(urls);
            QByteArray list;
            for (const QUrl& url : urls)
                list += url.toEncoded() + "\r\n";
            data->setData(QStringLiteral("application/x-kde4-urilist"), list);
        }
        auto* drag = new QDrag(this);
        drag->setMimeData(data);
        const Qt::DropAction action = drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction);
        QTextStream(stdout) << "drop-result " << int(action) << Qt::endl;
        QApplication::quit();
    }

private:
    QPoint m_press;
    bool m_started = false;
};

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Source source;
    QStringList args = app.arguments().mid(1);
    for (const QString& arg : args) {
        if (arg == QLatin1String("--portal-only"))
            source.portalOnly = true;
        else
            source.urls.append(QUrl::fromLocalFile(arg));
    }
    source.setWindowTitle(QStringLiteral("kk-dragsource"));
    source.resize(240, 160);
    source.show();
    return app.exec();
}
