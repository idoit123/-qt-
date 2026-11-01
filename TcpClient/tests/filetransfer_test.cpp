#include "book.h"
#include "pduframer.h"
#include <QtTest>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QUuid>
#include <cstring>

class FileTransferTest : public QObject {
    Q_OBJECT
private slots:
    void endToEnd() {
        QTcpSocket socket;
        socket.setProxy(QNetworkProxy::NoProxy);
        auto host = qEnvironmentVariable("CHAT_TEST_HOST", "127.0.0.1");
        auto port = qEnvironmentVariable("CHAT_TEST_PORT", "18889").toUShort();
        socket.connectToHost(host, port);
        QVERIFY2(socket.waitForConnected(5000), qPrintable(socket.errorString()));
        QByteArray user = "qtfile_" + QUuid::createUuid().toString(QUuid::Id128).left(12).toLatin1();
        PduFramer framer;
        auto auth = [&](uint type, const char *expected) {
            PDU *pdu = mkPDU(0); pdu->uiMsgType = type;
            memcpy(pdu->caData, user.constData(), static_cast<size_t>(user.size()));
            memcpy(pdu->caData + 32, "pw", 2);
            socket.write(reinterpret_cast<const char *>(pdu), pdu->uiPDULen); free(pdu);
            QElapsedTimer timer; timer.start();
            while (timer.elapsed() < 5000) {
                if (!socket.bytesAvailable()) socket.waitForReadyRead(200);
                framer.append(socket.readAll()); QByteArray frame; QString error;
                if (framer.takeFrame(&frame, &error) == PduFramer::FrameReady)
                    return frame.mid(8, 32).startsWith(expected);
            }
            return false;
        };
        QVERIFY(auth(ENUM_MSG_TYPE_REGIST_REQUEST, "regtist ok"));
        QVERIFY(auth(ENUM_MSG_TYPE_LOGIN_REQUEST, "login ok"));
        Book book(&socket); book.resize(850, 450); book.show();
        connect(&socket, &QTcpSocket::readyRead, &book, [&] {
            framer.append(socket.readAll());
            for (;;) {
                QByteArray frame; QString error;
                auto result = framer.takeFrame(&frame, &error);
                if (result == PduFramer::NeedMoreData) break;
                if (result == PduFramer::InvalidFrame) { socket.abort(); break; }
                PDU *pdu = static_cast<PDU *>(malloc(static_cast<size_t>(frame.size())));
                memcpy(pdu, frame.constData(), static_cast<size_t>(frame.size()));
                if (pdu->uiMsgType == ENUM_MSG_TYPE_FILE_RESPOND) book.handleResponse(pdu);
                free(pdu);
            }
        });
        QSignalSpy results(&book, &Book::operationFinished);
        auto successful = [&](const QString &op, bool ok = true) {
            for (const auto &row : results) if (row[0].toString() == op && row[1].toBool() == ok) return true;
            return false;
        };
        book.flushDir(); QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("list"));
        results.clear(); book.makeDirectory(QStringLiteral("中文 目录"));
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("mkdir"));
        book.listPath(QStringLiteral("中文 目录")); QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000);
        QCOMPARE(book.currentPath(), QStringLiteral("中文 目录"));
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        QByteArray content(2 * 1024 * 1024 + 7, '\0');
        for (int i = 0; i < content.size(); ++i) content[i] = static_cast<char>(i % 256);
        QString source = tmp.filePath("source.bin");
        { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(content), qint64(content.size())); }
        results.clear(); book.startUpload(source, QStringLiteral("文件 空格.bin"));
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 30000); QVERIFY(successful("upload"));
        auto *list = book.findChild<QListWidget *>("remoteFileList"); QVERIFY(list); QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->text(), QStringLiteral("文件 空格.bin"));
        QString downloaded = tmp.filePath("downloaded.bin");
        results.clear(); book.startDownload(QStringLiteral("文件 空格.bin"), downloaded);
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 30000); QVERIFY(successful("download"));
        { QFile file(downloaded); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), content); }
        // A failed download must not truncate an existing local target.
        results.clear(); book.startDownload("missing", downloaded);
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("download", false));
        { QFile file(downloaded); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), content); }
        results.clear(); book.startUpload(source, "cancelled"); book.cancelTransfer();
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("cancel"));
        book.flushDir(); QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QCOMPARE(list->count(), 1);
        QString empty = tmp.filePath("empty"); { QFile file(empty); QVERIFY(file.open(QIODevice::WriteOnly)); }
        results.clear(); book.startUpload(empty, "empty");
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("upload"));
        results.clear(); book.startDownload("empty", tmp.filePath("empty-download"));
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("download"));
        QCOMPARE(QFileInfo(tmp.filePath("empty-download")).size(), qint64(0));
        results.clear(); book.renameEntry(QStringLiteral("文件 空格.bin"), "renamed.bin");
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("rename"));
        results.clear(); book.removeEntry("renamed.bin", false);
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("remove"));
        book.removeEntry("empty", false); QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000);
        book.listPath({}); QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000);
        results.clear(); book.removeEntry(QStringLiteral("中文 目录"), true);
        QTRY_VERIFY_WITH_TIMEOUT(!book.busy(), 10000); QVERIFY(successful("rmdir"));
        QCOMPARE(list->count(), 0);
        // Disconnect discards local QSaveFile output and clears the busy state.
        book.startUpload(source, "disconnect"); socket.abort(); QTRY_VERIFY(!book.busy());
    }
};
QTEST_MAIN(FileTransferTest)
#include "filetransfer_test.moc"
