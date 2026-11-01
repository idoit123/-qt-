#pragma once

#include "../pduframer.h"
#include <QElapsedTimer>
#include <QImage>
#include <QObject>

class QTcpSocket;
class QTimer;

class VideoWorker : public QObject {
    Q_OBJECT
public:
    explicit VideoWorker(QObject *parent = nullptr);

public slots:
    void open(const QString &host, quint16 port, const QString &name,
              const QString &password, bool registerFirst);
    void close();
    void upload(const QImage &image);
    void resetBackground();

signals:
    void ready();
    void closed();
    void status(const QString &text);
    void frameReceived(const QImage &image, quint64 sequence, qint64 roundTripMs,
                       const QString &motionStatus);
    void frameRejected(const QString &reason);

private:
    enum State { Idle, Connecting, Registering, LoggingIn, Ready };
    void receive();
    void fail(const QString &reason);
    void authenticate(bool registration);
    bool writePacket(quint32 type, const QByteArray &body = {}, const QByteArray &fields = {});
    QTcpSocket *socket_;
    QTimer *timeout_;
    PduFramer framer_;
    State state_ = Idle;
    QString name_, password_;
    bool registerFirst_ = false;
    bool waiting_ = false;
    bool resetPending_ = true;
    quint64 sequence_ = 0;
    QElapsedTimer roundTrip_;
};
