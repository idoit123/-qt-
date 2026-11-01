#include "tcpclient.h"
#include "monitor/monitorwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    if (a.arguments().contains("--monitor-smoke")) return runMonitorSmoke(a);
    if (a.arguments().contains("--monitor")) {
        MonitorWindow monitor;
        monitor.show();
        return a.exec();
    }
    TcpClient::getInstance().show();

    return a.exec();
}
