// SPDX-License-Identifier: GPL-3.0-or-later
// Placeholder entry point — replaced by the foundation agent.
#include <QApplication>
#include <QLabel>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QLabel l(QStringLiteral(CW_APP_NAME " " CW_APP_VERSION " — skeleton"));
    l.resize(480, 120);
    l.show();
    return app.exec();
}
