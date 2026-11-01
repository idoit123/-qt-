#include "videoworker.h"
#include <QBuffer>
#include <QImageReader>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>
#include <cstring>

VideoWorker::VideoWorker(QObject *parent) : QObject(parent),
    socket_(new QTcpSocket(this)), timeout_(new QTimer(this))
{
    qRegisterMetaType<quint16>("quint16");
    qRegisterMetaType<quint64>("quint64");
    qRegisterMetaType<qint64>("qint64");
    socket_->setProxy(QNetworkProxy::NoProxy);
    socket_->setReadBufferSize(2 * 1024 * 1024);
    timeout_->setSingleShot(true);
    connect(timeout_, &QTimer::timeout, this, [this] { fail(QStringLiteral("连接或视频响应超时，请重新连接")); });
    connect(socket_, &QTcpSocket::connected, this, [this] {
        socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        authenticate(registerFirst_);
    });
    connect(socket_, &QTcpSocket::readyRead, this, &VideoWorker::receive);
    connect(socket_, &QTcpSocket::disconnected, this, &VideoWorker::close);
    connect(socket_, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [this](QAbstractSocket::SocketError) { fail(socket_->errorString()); });
}

void VideoWorker::open(const QString &host, quint16 port, const QString &name,
                       const QString &password, bool registerFirst)
{
    close();
    if (name.toUtf8().isEmpty() || name.toUtf8().size() > 31
            || password.toUtf8().isEmpty() || password.toUtf8().size() > 31) {
        emit closed();
        emit status(QStringLiteral("用户名和密码须为 1～31 个 UTF-8 字节"));
        return;
    }
    name_ = name; password_ = password; registerFirst_ = registerFirst;
    state_ = Connecting;
    emit status(QStringLiteral("正在连接 %1:%2…").arg(host).arg(port));
    timeout_->start(5000);
    socket_->connectToHost(host, port);
}

void VideoWorker::close()
{
    const bool changed = state_ != Idle;
    state_ = Idle; waiting_ = false;
    resetPending_ = true;
    timeout_->stop(); framer_.clear(); password_.clear();
    socket_->abort();
    if (changed) emit closed();
}

void VideoWorker::fail(const QString &reason)
{
    close();
    emit status(reason);
}

bool VideoWorker::writePacket(quint32 type, const QByteArray &body, const QByteArray &fields)
{
    if (socket_->state() != QAbstractSocket::ConnectedState) return false;
    PDU *pdu = mkPDU(static_cast<uint>(body.size()));
    pdu->uiMsgType = type;
    std::memcpy(pdu->caData, fields.constData(), static_cast<size_t>(qMin(64, fields.size())));
    if (!body.isEmpty()) std::memcpy(pdu->caMsg, body.constData(), static_cast<size_t>(body.size()));
    const qint64 expected = pdu->uiPDULen;
    const qint64 written = socket_->write(reinterpret_cast<const char *>(pdu), expected);
    free(pdu);
    if (written != expected) { fail(QStringLiteral("Socket 写入失败")); return false; }
    return true;
}

void VideoWorker::authenticate(bool registration)
{
    QByteArray fields(64, '\0');
    const auto name = name_.toUtf8(), password = password_.toUtf8();
    std::memcpy(fields.data(), name.constData(), static_cast<size_t>(name.size()));
    std::memcpy(fields.data() + 32, password.constData(), static_cast<size_t>(password.size()));
    state_ = registration ? Registering : LoggingIn;
    if (writePacket(registration ? 1 : 3, {}, fields)) timeout_->start(5000);
}

void VideoWorker::upload(const QImage &image)
{
    if (state_ != Ready || waiting_) { emit frameRejected(QStringLiteral("连接未就绪")); return; }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPEG", 75) || jpeg.size() > 512 * 1024) {
        emit frameRejected(QStringLiteral("图像编码失败或超过 512 KiB")); return;
    }
    QByteArray fields(64, '\0');
    const QByteArray sequence = QByteArray::number(++sequence_);
    std::memcpy(fields.data(), sequence.constData(), static_cast<size_t>(sequence.size()));
    if (resetPending_) std::memcpy(fields.data() + 32, "reset", 5);
    resetPending_ = false;
    waiting_ = true;
    roundTrip_.start();
    timeout_->start(5000);
    writePacket(200, jpeg, fields);
}

void VideoWorker::resetBackground()
{
    resetPending_ = true; // Carried on the next frame, ordered with its pixels.
}

void VideoWorker::receive()
{
    framer_.append(socket_->readAll());
    while (true) {
        QByteArray frame;
        QString error;
        const auto result = framer_.takeFrame(&frame, &error);
        if (result == PduFramer::NeedMoreData) return;
        if (result == PduFramer::InvalidFrame) { fail(error); return; }
        PDU pdu;
        std::memcpy(&pdu, frame.constData(), sizeof(PDU));
        const QByteArray body = frame.mid(sizeof(PDU), static_cast<int>(pdu.uiMsgLen));
        if (state_ == Registering && pdu.uiMsgType == 2) {
            if (QByteArray(pdu.caData, 64).startsWith("regtist ok")) authenticate(false);
            else fail(QStringLiteral("注册失败：用户名可能已存在，请改用“连接”"));
        } else if (state_ == LoggingIn && pdu.uiMsgType == 4) {
            if (!QByteArray(pdu.caData, 64).startsWith("login ok")) {
                fail(QStringLiteral("登录失败：检查账号密码，或账号已在线")); return;
            }
            timeout_->stop(); password_.clear(); state_ = Ready;
            emit status(QStringLiteral("已登录，可以启动摄像头或发送测试图")); emit ready();
        } else if (state_ == Ready && waiting_ && (pdu.uiMsgType == 201 || pdu.uiMsgType == 202)) {
            timeout_->stop(); waiting_ = false;
            if (pdu.uiMsgType == 202) { emit frameRejected(QString::fromUtf8(body)); continue; }
            if (QByteArray(pdu.caData, 32).split('\0').first().toULongLong() != sequence_) {
                fail(QStringLiteral("服务端返回的帧序号不匹配")); return;
            }
            QBuffer buffer;
            buffer.setData(body); buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "JPEG");
            const QSize size = reader.size();
            if (body.size() > 512 * 1024 || !size.isValid() || size.width() > 1280 || size.height() > 720) {
                fail(QStringLiteral("服务端图像尺寸或长度非法")); return;
            }
            const QImage image = reader.read();
            if (image.isNull()) { fail(QStringLiteral("服务端 JPEG 解码失败")); return; }
            const QString motion = QString::fromLatin1(QByteArray(pdu.caData + 32, 32).split('\0').first());
            emit frameReceived(image, sequence_, roundTrip_.elapsed(), motion);
        } else { fail(QStringLiteral("收到不符合当前状态的服务端消息")); return; }
        if (state_ == Idle) return;
    }
}
