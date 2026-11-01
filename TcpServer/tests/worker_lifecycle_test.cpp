#include "mytcpserver.h"

#include <QThread>
#include <QTcpSocket>
#include <QtTest>

#include <cstring>
#include <cstdlib>

namespace {
constexpr int kClientCount = 10;

QVector<QTcpSocket *> connectClients(MyTcpServer &server)
{
    QVector<QTcpSocket *> clients;
    for (int i = 0; i < kClientCount; ++i) {
        QTcpSocket *client = new QTcpSocket;
        client->connectToHost(QHostAddress::LocalHost, server.serverPort());
        if (!client->waitForConnected(3000)) {
            delete client;
            break;
        }
        if (!server.waitForNewConnection(3000)) {
            delete client;
            break;
        }
        clients.append(client);
    }
    return clients;
}
}

class WorkerLifecycleTest : public QObject
{
    Q_OBJECT

private slots:
    void disconnectReleasesWorkers();
    void shutdownWaitsForActiveWorkers();
    void malformedNameDisconnects();
};

void WorkerLifecycleTest::disconnectReleasesWorkers()
{
    MyTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QVector<QTcpSocket *> clients = connectClients(server);
    QCOMPARE(clients.size(), kClientCount);
    QTRY_COMPARE(server.findChildren<QThread *>().size(), kClientCount);

    for (QTcpSocket *client : clients) client->disconnectFromHost();
    QTRY_VERIFY(server.findChildren<QThread *>().isEmpty());
    server.shutdown();
    qDeleteAll(clients);
}

void WorkerLifecycleTest::shutdownWaitsForActiveWorkers()
{
    MyTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QVector<QTcpSocket *> clients = connectClients(server);
    QCOMPARE(clients.size(), kClientCount);
    QTRY_COMPARE(server.findChildren<QThread *>().size(), kClientCount);

    server.shutdown();
    QVERIFY(!server.isListening());
    for (QThread *thread : server.findChildren<QThread *>())
        QVERIFY(!thread->isRunning());
    QTRY_VERIFY(server.findChildren<QThread *>().isEmpty());
    qDeleteAll(clients);
}

void WorkerLifecycleTest::malformedNameDisconnects()
{
    MyTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    PDU *packet = mkPDU(0);
    packet->uiMsgType = ENUM_MSG_TYPE_LOGIN_REQUEST;
    std::memset(packet->caData, 'x', sizeof(packet->caData));
    client.write(reinterpret_cast<const char *>(packet), packet->uiPDULen);
    std::free(packet);
    QVERIFY(client.waitForDisconnected(3000));
    QTRY_VERIFY(server.findChildren<QThread *>().isEmpty());
    server.shutdown();
}

QTEST_GUILESS_MAIN(WorkerLifecycleTest)
#include "worker_lifecycle_test.moc"
