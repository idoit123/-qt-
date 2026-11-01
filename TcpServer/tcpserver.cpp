#include "tcpserver.h"
#include "ui_tcpserver.h"
#include "mytcpserver.h"
#include "opedb.h"

TcpServer::TcpServer(QWidget *parent)
    : QWidget(parent), ui(new Ui::TcpServer)
{
    ui->setupUi(this);
    loadConfig();
    OpeDB::getInstance().setDatabasePath(m_databasePath);
    OpeDB::getInstance().init();
    if (!MyTcpServer::getInstance().listen(QHostAddress(m_strIp), m_usPort)) {
        QMessageBox::critical(this, "Server", MyTcpServer::getInstance().errorString());
    }
}

TcpServer::~TcpServer()
{
    MyTcpServer::getInstance().shutdown();
    delete ui;
}

void TcpServer::loadConfig()
{
    QFile file(":/server.config");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "open config", "open config failed");
        return;
    }
    const QStringList lines = QString::fromUtf8(file.readAll())
        .split(QRegExp("[\\r\\n]+"), Qt::SkipEmptyParts);
    if (lines.size() < 2) {
        QMessageBox::critical(this, "open config", "server.config requires IP and port");
        return;
    }
    m_strIp = lines.at(0).trimmed();
    m_usPort = lines.at(1).trimmed().toUShort();
    if (lines.size() >= 3) m_databasePath = lines.at(2).trimmed();
}
