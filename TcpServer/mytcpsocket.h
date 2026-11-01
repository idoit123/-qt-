#ifndef MYTCPSOCKET_H
#define MYTCPSOCKET_H

#include <QTcpSocket>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include "protocol.h"

class MyTcpSocket : public QTcpSocket
{
    Q_OBJECT
public:
    MyTcpSocket();
    QString getName() const;

public slots:
    void initialize(qintptr socketDescriptor);
    void shutdown();
    void recvMsg();
    void clientOffline();
    void sendFileToClient();
    void writePayload(const QByteArray &payload);

signals:
    void offline(MyTcpSocket *socket);
    void loggedIn(MyTcpSocket *socket, const QString &userName);

private:
    bool allowIncomingMessage();
    bool queuePayload(const char *data, qint64 size);
    void processPdu(PDU *pdu);
    void copyDir(QString strSrcDir, QString strDesDir);

    QString str_name;
    QByteArray m_receiveBuffer;
    QElapsedTimer m_rateTimer;
    qint64 m_lastRateRefillMs = 0;
    double m_rateTokens = 0;
    QFile m_file;
    qint64 m_iTotal = 0;
    qint64 m_iRecved = 0;
    bool m_isUploadFile = false;
    bool m_offlineHandled = false;
    QTimer *m_pTimer = nullptr;
};

#endif // MYTCPSOCKET_H
