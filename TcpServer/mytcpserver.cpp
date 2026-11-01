#include "mytcpserver.h"

#include <QDebug>
#include <QMetaObject>

MyTcpServer::MyTcpServer()
{
    qRegisterMetaType<MyTcpSocket *>("MyTcpSocket*");
    connect(this, &MyTcpServer::routeRequested, this, &MyTcpServer::routeToUser,
            Qt::QueuedConnection);
}

MyTcpServer::~MyTcpServer()
{
    shutdown();
}

MyTcpServer &MyTcpServer::getInstance()
{
    static MyTcpServer instance;
    return instance;
}

void MyTcpServer::incomingConnection(qintptr socketDescriptor)
{
    if (m_stopping) {
        QTcpSocket rejected;
        if (rejected.setSocketDescriptor(socketDescriptor)) rejected.abort();
        return;
    }

    QThread *thread = new QThread(this);
    MyTcpSocket *socket = new MyTcpSocket;
    socket->moveToThread(thread);
    m_connections.insert(socket, thread);
    m_threads.insert(thread);

    connect(thread, &QThread::started, socket, [socket, socketDescriptor]() {
        socket->initialize(socketDescriptor);
    });
    connect(socket, &MyTcpSocket::offline, this, &MyTcpServer::deleteSocket,
            Qt::QueuedConnection);
    connect(socket, &MyTcpSocket::loggedIn, this, &MyTcpServer::registerUser,
            Qt::QueuedConnection);
    connect(socket, &QObject::destroyed, thread, &QThread::quit,
            Qt::DirectConnection);
    connect(thread, &QThread::finished, this, [this, socket, thread]() {
        m_connections.remove(socket);
        m_threads.remove(thread);
        for (auto it = m_users.begin(); it != m_users.end();) {
            if (it.value() == socket) it = m_users.erase(it);
            else ++it;
        }
        thread->deleteLater();
    }, Qt::QueuedConnection);

    qInfo() << "Accepted connection" << socketDescriptor << "worker" << thread;
    thread->start();
}

void MyTcpServer::shutdown()
{
    if (m_stopping) return;
    m_stopping = true;
    close();
    m_users.clear();

    const auto connections = m_connections;
    m_connections.clear();
    for (auto it = connections.cbegin(); it != connections.cend(); ++it) {
        if (it.value()->isRunning())
            QMetaObject::invokeMethod(it.key(), "shutdown", Qt::QueuedConnection);
    }
    const auto threads = m_threads;
    for (QThread *thread : threads) {
        if (thread->isRunning()) thread->wait();
    }
    m_threads.clear();
}

void MyTcpServer::registerUser(MyTcpSocket *socket, const QString &userName)
{
    if (m_connections.contains(socket)) {
        m_users.insert(userName, socket);
    }
}

void MyTcpServer::deleteSocket(MyTcpSocket *socket)
{
    QThread *thread = m_connections.take(socket);
    if (!thread) {
        return;
    }

    for (auto it = m_users.begin(); it != m_users.end();) {
        if (it.value() == socket) {
            it = m_users.erase(it);
        } else {
            ++it;
        }
    }

    qInfo() << "Removing connection" << static_cast<const void *>(socket);
    QMetaObject::invokeMethod(socket, "deleteLater", Qt::QueuedConnection);
}

void MyTcpServer::resend(const char *friName, PDU *pdu)
{
    if (!friName || !pdu || pdu->uiPDULen < sizeof(PDU)) {
        return;
    }
    emit routeRequested(QString::fromLocal8Bit(friName),
                        QByteArray(reinterpret_cast<const char *>(pdu), pdu->uiPDULen));
}

void MyTcpServer::resendGroupMsg(QSet<QString> friendSet, PDU *pdu)
{
    if (!pdu || pdu->uiPDULen < sizeof(PDU)) {
        return;
    }

    QByteArray payload(reinterpret_cast<const char *>(pdu), pdu->uiPDULen);
    reinterpret_cast<PDU *>(payload.data())->uiMsgType = ENUM_MSG_TYPE_GROUP_CHAT_RESPOND;
    for (const QString &friendName : friendSet) {
        emit routeRequested(friendName, payload);
    }
}

void MyTcpServer::routeToUser(const QString &userName, const QByteArray &payload)
{
    if (m_stopping) return;
    MyTcpSocket *socket = m_users.value(userName, nullptr);
    if (!socket || !m_connections.contains(socket)) {
        qInfo() << "Recipient is offline" << userName;
        return;
    }
    QMetaObject::invokeMethod(socket, "writePayload", Qt::QueuedConnection,
                              Q_ARG(QByteArray, payload));
}
