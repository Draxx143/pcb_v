// main.cpp — GUI entry point.
#include <QApplication>
#include <QIcon>
#include <QStringList>

#include "Icons.h"
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pcb-viewer"));  // settings / config path
    app.setApplicationDisplayName(QStringLiteral("PCB Viewer"));
    app.setOrganizationName(QStringLiteral("pcbv"));
    app.setApplicationVersion(QStringLiteral("0.3.0"));
    // Wayland/X11 dock icon: matches the .desktop file written by setup_linux.sh.
    app.setDesktopFileName(QStringLiteral("pcb-viewer"));
    app.setWindowIcon(pcbv::gui::appIcon());

    MainWindow window;
    window.show();

    const QStringList args = app.arguments();
    if (args.size() > 1) {
        const QString path = args.at(1);
        if (!path.isEmpty() && !path.startsWith('-')) window.openDocument(path);
    }
    return app.exec();
}
