#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QRCodeScanner"));
    QApplication::setOrganizationName(QStringLiteral("Local Industrial Vision"));

    MainWindow window;
    window.show();
    return application.exec();
}

