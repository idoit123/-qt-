#ifndef PDUFRAMER_H
#define PDUFRAMER_H

#include "protocol.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <cstring>

// The existing wire protocol uses native-endian 32-bit lengths.
class PduFramer
{
public:
    enum Result { NeedMoreData, FrameReady, InvalidFrame };
    static const quint32 MaxPduLength = 16 * 1024 * 1024;

    void append(const QByteArray &data) { m_buffer.append(data); }
    void clear() { m_buffer.clear(); }

    Result takeFrame(QByteArray *frame, QString *error)
    {
        if (m_buffer.size() < static_cast<int>(sizeof(quint32)))
            return NeedMoreData;

        quint32 length = 0;
        std::memcpy(&length, m_buffer.constData(), sizeof(length));
        if (length < sizeof(PDU) || length > MaxPduLength) {
            if (error) *error = QStringLiteral("invalid PDU length: %1").arg(length);
            return InvalidFrame;
        }
        if (m_buffer.size() < static_cast<int>(length))
            return NeedMoreData;

        quint32 messageLength = 0;
        std::memcpy(&messageLength,
                    m_buffer.constData() + offsetof(PDU, uiMsgLen),
                    sizeof(messageLength));
        if (messageLength > length - sizeof(PDU)) {
            if (error) *error = QStringLiteral("invalid PDU message length: %1").arg(messageLength);
            return InvalidFrame;
        }

        if (frame) *frame = m_buffer.left(static_cast<int>(length));
        m_buffer.remove(0, static_cast<int>(length));
        return FrameReady;
    }

private:
    QByteArray m_buffer;
};

#endif // PDUFRAMER_H
