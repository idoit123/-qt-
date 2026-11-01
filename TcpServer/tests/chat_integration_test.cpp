#include "mytcpserver.h"
#include "opedb.h"

#include <QSet>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <cstddef>
#include <cstring>
#include <cstdlib>

namespace {
constexpr int kClientCount = 20;
constexpr int kMessagesPerClient = 50;
constexpr quint32 kMaxFrameLength = 16 * 1024 * 1024;

QByteArray makePacket(quint32 type, const QByteArray &first,
                      const QByteArray &second = QByteArray(),
                      const QByteArray &message = QByteArray())
{
    PDU *pdu = mkPDU(static_cast<uint>(message.size()));
    pdu->uiMsgType = type;
    std::memcpy(pdu->caData, first.constData(),
                static_cast<size_t>(qMin(first.size(), 31)));
    std::memcpy(pdu->caData + 32, second.constData(),
                static_cast<size_t>(qMin(second.size(), 31)));
    if (!message.isEmpty())
        std::memcpy(pdu->caMsg, message.constData(),
                    static_cast<size_t>(message.size()));
    QByteArray packet(reinterpret_cast<const char *>(pdu),
                      static_cast<int>(pdu->uiPDULen));
    std::free(pdu);
    return packet;
}
}

class ChatIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void twentyClientsExchangeMessages();

private:
    void consume(int clientIndex);

    QTemporaryDir m_tempDir;
    QVector<QTcpSocket *> m_clients;
    QVector<QByteArray> m_buffers;
    QVector<bool> m_loggedIn;
    QSet<QString> m_received;
    QSet<int> m_groupRecipients;
    QStringList m_errors;
};

void ChatIntegrationTest::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    const QString databasePath = m_tempDir.filePath(QStringLiteral("chat.db"));
    OpeDB &db = OpeDB::getInstance();
    db.setDatabasePath(databasePath);
    db.init();
    for (int i = 0; i < kClientCount; ++i) {
        const QByteArray name = QByteArray("user") + QByteArray::number(i);
        QVERIFY(db.handelRegist(name.constData(), "pw"));
    }
    for (int i = 1; i <= 3; ++i) {
        const QByteArray name = QByteArray("user") + QByteArray::number(i);
        QVERIFY(db.handleAddFriendAgree("user0", name.constData()));
    }
}

void ChatIntegrationTest::cleanupTestCase()
{
    MyTcpServer::getInstance().shutdown();
    qDeleteAll(m_clients);
    m_clients.clear();
    OpeDB::getInstance().closeConnectionForCurrentThread();
}

void ChatIntegrationTest::consume(int clientIndex)
{
    QByteArray &buffer = m_buffers[clientIndex];
    buffer.append(m_clients[clientIndex]->readAll());
    while (buffer.size() >= static_cast<int>(sizeof(quint32))) {
        quint32 length = 0;
        std::memcpy(&length, buffer.constData(), sizeof(length));
        if (length < sizeof(PDU) || length > kMaxFrameLength) {
            m_errors.append(QStringLiteral("invalid response length"));
            buffer.clear();
            return;
        }
        if (buffer.size() < static_cast<int>(length)) return;
        const QByteArray frame = buffer.left(static_cast<int>(length));
        buffer.remove(0, static_cast<int>(length));
        quint32 type = 0;
        quint32 messageLength = 0;
        std::memcpy(&type, frame.constData() + offsetof(PDU, uiMsgType), sizeof(type));
        std::memcpy(&messageLength, frame.constData() + offsetof(PDU, uiMsgLen),
                    sizeof(messageLength));
        if (messageLength > length - sizeof(PDU)) {
            m_errors.append(QStringLiteral("invalid response message length"));
            return;
        }
        if (type == ENUM_MSG_TYPE_LOGIN_RESPOND) {
            if (QByteArray(frame.constData() + offsetof(PDU, caData), 64).startsWith(LOGIN_OK))
                m_loggedIn[clientIndex] = true;
            else
                m_errors.append(QStringLiteral("login failed"));
        } else if (type == ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST) {
            const QString message = QString::fromUtf8(frame.constData() + sizeof(PDU));
            m_received.insert(QString::number(clientIndex) + QLatin1Char('|') + message);
        } else if (type == ENUM_MSG_TYPE_GROUP_CHAT_RESPOND) {
            m_groupRecipients.insert(clientIndex);
        } else {
            m_errors.append(QStringLiteral("unexpected response type: %1").arg(type));
        }
    }
}

void ChatIntegrationTest::twentyClientsExchangeMessages()
{
    MyTcpServer &server = MyTcpServer::getInstance();
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket unauthenticated;
    unauthenticated.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(unauthenticated.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));
    unauthenticated.write(makePacket(ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST,
                                     "user0", "user1", "unauthorized\0"));
    QVERIFY(unauthenticated.waitForBytesWritten(3000));
    QTRY_COMPARE_WITH_TIMEOUT(unauthenticated.state(),
                              QAbstractSocket::UnconnectedState, 3000);
    QCOMPARE(server.onlineUserCount(), 0);

    m_clients.resize(kClientCount);
    m_buffers.resize(kClientCount);
    m_loggedIn.fill(false, kClientCount);

    for (int i = 0; i < kClientCount; ++i) {
        QTcpSocket *client = new QTcpSocket(this);
        m_clients[i] = client;
        connect(client, &QTcpSocket::readyRead, this, [this, i]() { consume(i); });
        client->connectToHost(QHostAddress::LocalHost, server.serverPort());
        QVERIFY(client->waitForConnected(3000));
        QVERIFY(server.waitForNewConnection(3000));
    }

    for (int i = 0; i < kClientCount; ++i) {
        const QByteArray name = QByteArray("user") + QByteArray::number(i);
        m_clients[i]->write(makePacket(ENUM_MSG_TYPE_LOGIN_REQUEST, name, "pw"));
    }
    QTRY_COMPARE_WITH_TIMEOUT(server.onlineUserCount(), kClientCount, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_loggedIn.contains(false), 10000);
    QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join(QLatin1Char('\n'))));

    QSet<QString> expected;
    for (int sender = 0; sender < kClientCount; ++sender) {
        const int recipient = (sender + 1) % kClientCount;
        const QByteArray source = QByteArray("user") + QByteArray::number(sender);
        const QByteArray destination = QByteArray("user") + QByteArray::number(recipient);
        for (int sequence = 0; sequence < kMessagesPerClient; ++sequence) {
            const QByteArray message = QByteArray::number(sender) + ':'
                    + QByteArray::number(sequence) + '\0';
            expected.insert(QString::number(recipient) + QLatin1Char('|')
                            + QString::fromUtf8(message.constData()));
            m_clients[sender]->write(makePacket(ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST,
                                                source, destination, message));
        }
    }
    QTRY_COMPARE_WITH_TIMEOUT(m_received.size(), expected.size(), 30000);
    QCOMPARE(m_received, expected);
    QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join(QLatin1Char('\n'))));

    m_clients[0]->write(makePacket(ENUM_MSG_TYPE_GROUP_CHAT_REQUEST,
                                   "user0", QByteArray(), "group\0"));
    QTRY_COMPARE_WITH_TIMEOUT(m_groupRecipients.size(), 3, 10000);
    QCOMPARE(m_groupRecipients, (QSet<int>{1, 2, 3}));
    QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join(QLatin1Char('\n'))));

    const QString forgedMessage = QStringLiteral("forged-identity");
    m_clients[0]->write(makePacket(ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST,
                                   "user1", "user2",
                                   forgedMessage.toUtf8() + '\0'));
    QVERIFY(m_clients[0]->waitForBytesWritten(3000));
    QTRY_COMPARE_WITH_TIMEOUT(m_clients[0]->state(),
                              QAbstractSocket::UnconnectedState, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(server.onlineUserCount(), kClientCount - 1, 3000);
    QVERIFY(!m_received.contains(QStringLiteral("2|") + forgedMessage));

    QByteArray flood;
    const QByteArray floodPacket = makePacket(ENUM_MSG_TYPE_GROUP_CHAT_REQUEST,
                                              "user4", QByteArray(), "flood\0");
    for (int i = 0; i < 300; ++i)
        flood.append(floodPacket);
    m_clients[4]->write(flood);
    QTRY_COMPARE_WITH_TIMEOUT(m_clients[4]->state(),
                              QAbstractSocket::UnconnectedState, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(server.onlineUserCount(), kClientCount - 2, 5000);

    QByteArray oversizedMessage(128 * 1024, 'x');
    m_clients[6]->write(makePacket(ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST,
                                   "user6", "user5", oversizedMessage));
    QTRY_COMPARE_WITH_TIMEOUT(m_clients[6]->state(),
                              QAbstractSocket::UnconnectedState, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(server.onlineUserCount(), kClientCount - 3, 5000);
    QCOMPARE(m_clients[5]->state(), QAbstractSocket::ConnectedState);

    disconnect(m_clients[5], &QTcpSocket::readyRead, this, nullptr);
    m_clients[5]->setReadBufferSize(1);
    const QByteArray slowConsumerMessage(32 * 1024, 's');
    QByteArray slowConsumerBatch7;
    QByteArray slowConsumerBatch8;
    const QByteArray packet7 = makePacket(ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST,
                                          "user7", "user5", slowConsumerMessage);
    const QByteArray packet8 = makePacket(ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST,
                                          "user8", "user5", slowConsumerMessage);
    for (int i = 0; i < 100; ++i) {
        slowConsumerBatch7.append(packet7);
        slowConsumerBatch8.append(packet8);
    }
    m_clients[7]->write(slowConsumerBatch7);
    m_clients[8]->write(slowConsumerBatch8);
    QTRY_COMPARE_WITH_TIMEOUT(server.onlineUserCount(), kClientCount - 4, 5000);
    QCOMPARE(m_clients[7]->state(), QAbstractSocket::ConnectedState);
    QCOMPARE(m_clients[8]->state(), QAbstractSocket::ConnectedState);
}

QTEST_GUILESS_MAIN(ChatIntegrationTest)
#include "chat_integration_test.moc"
