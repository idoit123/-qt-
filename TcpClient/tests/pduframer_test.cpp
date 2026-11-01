#include "pduframer.h"

#include <QtTest>

#include <cstddef>
#include <cstring>
#include <cstdlib>

namespace {
QByteArray makeFrame(quint32 type, const QByteArray &message = QByteArray())
{
    PDU *pdu = mkPDU(static_cast<uint>(message.size()));
    pdu->uiMsgType = type;
    if (!message.isEmpty())
        std::memcpy(pdu->caMsg, message.constData(), static_cast<size_t>(message.size()));
    QByteArray frame(reinterpret_cast<const char *>(pdu), static_cast<int>(pdu->uiPDULen));
    std::free(pdu);
    return frame;
}
}

class PduFramerTest : public QObject
{
    Q_OBJECT

private slots:
    void splitAtEveryByte();
    void coalescedFrames();
    void rejectsInvalidLengths();
    void rejectsInvalidMessageLength();
};

void PduFramerTest::splitAtEveryByte()
{
    const QByteArray expected = makeFrame(ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST, "hello");
    PduFramer framer;
    QByteArray actual;
    for (int i = 0; i < expected.size(); ++i) {
        framer.append(expected.mid(i, 1));
        QString error;
        const PduFramer::Result result = framer.takeFrame(&actual, &error);
        QCOMPARE(result, i + 1 == expected.size() ? PduFramer::FrameReady
                                                  : PduFramer::NeedMoreData);
    }
    QCOMPARE(actual, expected);
    QCOMPARE(framer.takeFrame(&actual, nullptr), PduFramer::NeedMoreData);
}

void PduFramerTest::coalescedFrames()
{
    const QByteArray first = makeFrame(ENUM_MSG_TYPE_LOGIN_RESPOND);
    const QByteArray second = makeFrame(ENUM_MSG_TYPE_GROUP_CHAT_RESPOND, "group message");
    PduFramer framer;
    framer.append(first + second);

    QByteArray actual;
    QCOMPARE(framer.takeFrame(&actual, nullptr), PduFramer::FrameReady);
    QCOMPARE(actual, first);
    QCOMPARE(framer.takeFrame(&actual, nullptr), PduFramer::FrameReady);
    QCOMPARE(actual, second);
    QCOMPARE(framer.takeFrame(&actual, nullptr), PduFramer::NeedMoreData);
}

void PduFramerTest::rejectsInvalidLengths()
{
    for (quint32 length : {quint32(0), quint32(sizeof(PDU) - 1),
                           PduFramer::MaxPduLength + 1}) {
        PduFramer framer;
        framer.append(QByteArray(reinterpret_cast<const char *>(&length), sizeof(length)));
        QString error;
        QCOMPARE(framer.takeFrame(nullptr, &error), PduFramer::InvalidFrame);
        QVERIFY(!error.isEmpty());
        framer.clear();
        framer.append(makeFrame(ENUM_MSG_TYPE_LOGIN_RESPOND));
        QCOMPARE(framer.takeFrame(nullptr, nullptr), PduFramer::FrameReady);
    }
}

void PduFramerTest::rejectsInvalidMessageLength()
{
    QByteArray frame = makeFrame(ENUM_MSG_TYPE_LOGIN_RESPOND);
    const quint32 invalidLength = 1;
    std::memcpy(frame.data() + offsetof(PDU, uiMsgLen), &invalidLength,
                sizeof(invalidLength));
    PduFramer framer;
    framer.append(frame);
    QString error;
    QCOMPARE(framer.takeFrame(nullptr, &error), PduFramer::InvalidFrame);
    QVERIFY(!error.isEmpty());
}

QTEST_APPLESS_MAIN(PduFramerTest)
#include "pduframer_test.moc"
