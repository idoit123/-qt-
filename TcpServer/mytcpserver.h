#ifndef MYTCPSERVER_H
#define MYTCPSERVER_H

#include <QTcpServer>
#include <QHash>
#include <QSet>
#include <QThread>
#include "mytcpsocket.h"

class MyTcpServer : public QTcpServer
{
    Q_OBJECT
public:
    MyTcpServer();
    ~MyTcpServer() override;
    static MyTcpServer &getInstance();
    void shutdown();
    int onlineUserCount() const { return m_users.size(); }
    void incomingConnection(qintptr socketDescriptor) override;
    void resend(const char *friName, PDU *pdu);
    void resendGroupMsg(QSet<QString> friendSet, PDU *pdu);

public slots:
    void deleteSocket(MyTcpSocket *socket);
    void registerUser(MyTcpSocket *socket, const QString &userName);

signals:
    void routeRequested(const QString &userName, const QByteArray &payload);

private slots:
    void routeToUser(const QString &userName, const QByteArray &payload);

private:
    QHash<MyTcpSocket *, QThread *> m_connections;
    QSet<QThread *> m_threads;
    QHash<QString, MyTcpSocket *> m_users;
    bool m_stopping = false;
};

Q_DECLARE_METATYPE(MyTcpSocket *)

#endif // MYTCPSERVER_H
