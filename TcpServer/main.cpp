#include "tcpserver.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    TcpServer window;
    window.show();
    return application.exec();
}