#include "book.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QStyle>
#include <QVBoxLayout>
#include <cstring>
namespace {
constexpr qint64 MaxFileSize = 256LL * 1024 * 1024;
constexpr qint64 ChunkSize = 64 * 1024;
QByteArray pathBytes(const QString &path) { QByteArray b = path.toUtf8(); b.append('\0'); return b; }
}
Book::Book(QTcpSocket *socket, QWidget *parent) : QWidget(parent), m_socket(socket) {
    m_pathLabel = new QLabel("目录：/", this);
    m_list = new QListWidget(this); m_list->setObjectName("remoteFileList");
    m_status = new QLabel("单文件上限 256 MiB；同名文件不会覆盖。", this); m_status->setWordWrap(true);
    m_progress = new QProgressBar(this); m_progress->setRange(0, 1000); m_progress->setValue(0);
    m_cancel = new QPushButton("取消传输", this); m_cancel->setEnabled(false);
    auto *buttons = new QHBoxLayout;
    auto button = [&](const QString &text) {
        auto *b = new QPushButton(text, this); buttons->addWidget(b); m_actions.append(b); return b;
    };
    connect(button("上一级"), &QPushButton::clicked, this, [this] {
        if (m_path.isEmpty()) return;
        int slash = m_path.lastIndexOf('/'); listPath(slash < 0 ? QString() : m_path.left(slash));
    });
    connect(button("刷新"), &QPushButton::clicked, this, &Book::flushDir);
    connect(button("新建目录"), &QPushButton::clicked, this, [this] {
        bool ok; auto name = QInputDialog::getText(this, "新建目录", "目录名称", QLineEdit::Normal, {}, &ok);
        if (ok) makeDirectory(name);
    });
    connect(button("重命名"), &QPushButton::clicked, this, [this] {
        auto *item = m_list->currentItem(); if (!item) return;
        bool ok; auto name = QInputDialog::getText(this, "重命名", "新名称", QLineEdit::Normal, item->text(), &ok);
        if (ok) renameEntry(item->text(), name);
    });
    connect(button("删除"), &QPushButton::clicked, this, &Book::deleteSelected);
    connect(button("上传"), &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, "上传文件");
        if (!path.isEmpty()) startUpload(path, QFileInfo(path).fileName());
    });
    connect(button("下载"), &QPushButton::clicked, this, [this] {
        auto *item = m_list->currentItem(); if (!item || item->data(Qt::UserRole).toBool()) return;
        auto path = QFileDialog::getSaveFileName(this, "保存下载文件", item->text());
        if (!path.isEmpty()) startDownload(item->text(), path);
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item->data(Qt::UserRole).toBool()) listPath(childPath(item->text()));
    });
    connect(m_cancel, &QPushButton::clicked, this, &Book::cancelTransfer);
    connect(socket, &QTcpSocket::disconnected, this, [this] {
        if (m_busy) finish(m_transfer.isEmpty() ? QString::fromLatin1(m_pending) : m_transfer, false, "连接断开，未完成传输已取消。");
        m_status->setText("连接已断开，请重新打开客户端并登录。");
    });
    m_timeout.setSingleShot(true); m_timeout.setInterval(30000);
    m_pump.setSingleShot(true);
    connect(&m_pump, &QTimer::timeout, this, [this] {
        if (m_transfer == "upload") nextUpload(); else nextDownload();
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        finish(m_transfer.isEmpty() ? QString::fromLatin1(m_pending) : m_transfer, false, "请求超时，请重新连接。");
        m_socket->abort();
    });
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pathLabel); layout->addLayout(buttons); layout->addWidget(m_list);
    layout->addWidget(m_progress); layout->addWidget(m_cancel); layout->addWidget(m_status);
}
QString Book::getShareFileName() const { return m_list->currentItem() ? m_list->currentItem()->text() : QString(); }
QString Book::childPath(const QString &name) const { return m_path.isEmpty() ? name : m_path + "/" + name; }
bool Book::validName(const QString &name) {
    bool valid = !name.isEmpty() && name != "." && name != ".." && name.toUtf8().size() <= 255
        && !name.contains('/') && !name.contains('\\') && !name.startsWith(".upload-");
    for (QChar ch : name) valid = valid && ch.unicode() >= 32 && ch.unicode() != 127;
    if (!valid) m_status->setText("名称须为 1–255 个 UTF-8 字节，不能含路径分隔符或控制字符。");
    return valid;
}
void Book::setBusy(bool value) {
    m_busy = value;
    for (auto *button : m_actions) button->setEnabled(!value);
    m_list->setEnabled(!value); m_cancel->setEnabled(value && !m_transfer.isEmpty() && !m_cancelling);
}
void Book::reset() {
    m_pump.stop();
    m_timeout.stop(); m_upload.close(); m_download.reset(); m_pending.clear();
    m_transfer.clear(); m_cancelling = false; m_path.clear(); m_list->clear();
    m_pathLabel->setText("目录：/"); m_progress->setValue(0); setBusy(false);
}
bool Book::send(const QByteArray &op, const QByteArray &body, const QByteArray &argument) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) { finish(QString::fromLatin1(op), false, "尚未连接服务器。"); return false; }
    PDU *pdu = mkPDU(static_cast<uint>(body.size()));
    if (!pdu) { finish(QString::fromLatin1(op), false, "内存不足。"); return false; }
    pdu->uiMsgType = ENUM_MSG_TYPE_FILE_REQUEST;
    std::memcpy(pdu->caData, op.constData(), static_cast<size_t>(qMin(31, op.size())));
    std::memcpy(pdu->caData + 32, argument.constData(), static_cast<size_t>(qMin(31, argument.size())));
    if (!body.isEmpty()) std::memcpy(pdu->caMsg, body.constData(), static_cast<size_t>(body.size()));
    qint64 expected = pdu->uiPDULen;
    qint64 written = m_socket->write(reinterpret_cast<const char *>(pdu), expected); free(pdu);
    if (written != expected) { finish(QString::fromLatin1(op), false, "网络发送失败。"); m_socket->abort(); return false; }
    m_pending = op; setBusy(true); m_timeout.start(); return true;
}
void Book::finish(const QString &operation, bool ok, const QString &message) {
    m_pump.stop();
    // Copy: operation may alias m_transfer, which is cleared below.
    const QString completedOperation = operation;
    m_timeout.stop(); m_upload.close(); m_download.reset(); m_pending.clear();
    m_transfer.clear(); m_cancelling = false; setBusy(false); m_status->setText(message);
    emit operationFinished(completedOperation, ok, message);
}
void Book::listPath(const QString &path) {
    if (m_busy) return;
    m_requestedPath = path; send("list", pathBytes(path));
}
void Book::flushDir() { listPath(m_path); }
void Book::makeDirectory(const QString &name) {
    if (!m_busy && validName(name)) send("mkdir", pathBytes(childPath(name)));
}
void Book::renameEntry(const QString &name, const QString &newName) {
    if (!m_busy && validName(name) && validName(newName)) send("rename", pathBytes(childPath(name)) + pathBytes(childPath(newName)));
}
void Book::removeEntry(const QString &name, bool directory) {
    if (!m_busy && validName(name)) send(directory ? "rmdir" : "remove", pathBytes(childPath(name)));
}
void Book::deleteSelected() {
    auto *item = m_list->currentItem(); if (!item) return;
    bool directory = item->data(Qt::UserRole).toBool();
    QString prompt = directory ? "删除目录“%1”？目录必须为空。" : "永久删除文件“%1”？";
    if (QMessageBox::question(this, "确认删除", prompt.arg(item->text())) == QMessageBox::Yes) removeEntry(item->text(), directory);
}
void Book::startUpload(const QString &localPath, const QString &remoteName) {
    if (m_busy || !validName(remoteName)) return;
    m_upload.setFileName(localPath);
    if (!m_upload.open(QIODevice::ReadOnly)) { finish("upload", false, "无法打开本地文件。"); return; }
    m_total = m_upload.size(); m_done = m_chunk = 0;
    if (m_total < 0 || m_total > MaxFileSize) { finish("upload", false, "文件超过 256 MiB 限制。"); return; }
    m_transfer = "upload"; updateProgress(); send("upload", pathBytes(childPath(remoteName)), QByteArray::number(m_total));
}
void Book::nextUpload() {
    if (m_cancelling || m_transfer != "upload") return;
    if (m_done == m_total) { send("commit"); return; }
    QByteArray chunk = m_upload.read(qMin(ChunkSize, m_total - m_done));
    if (chunk.isEmpty()) { cancelTransfer(); m_status->setText("本地读取失败，正在取消上传。"); return; }
    m_chunk = chunk.size(); send("chunk", chunk);
}
void Book::startDownload(const QString &remoteName, const QString &localPath) {
    if (m_busy || !validName(remoteName)) return;
    m_download.reset(new QSaveFile(localPath)); m_download->setDirectWriteFallback(false);
    if (!m_download->open(QIODevice::WriteOnly)) { finish("download", false, "无法创建本地临时文件。"); return; }
    m_total = m_done = 0; m_transfer = "download"; updateProgress(); send("download", pathBytes(childPath(remoteName)));
}
void Book::nextDownload() { if (!m_cancelling && m_transfer == "download") send("read"); }
void Book::cancelTransfer() {
    if (m_transfer.isEmpty() || m_cancelling) return;
    m_pump.stop(); m_cancelling = true; m_upload.close(); m_download.reset(); send("cancel");
}
void Book::updateProgress() {
    m_progress->setValue(m_total ? static_cast<int>(m_done * 1000 / m_total) : 0);
    m_status->setText(QString("%1：%2 / %3 字节").arg(m_transfer == "upload" ? "上传" : "下载").arg(m_done).arg(m_total));
}
void Book::handleResponse(const PDU *pdu) {
    if (!m_busy) return;
    if (!std::memchr(pdu->caData, 0, 32) || !std::memchr(pdu->caData + 32, 0, 32)) { m_socket->abort(); return; }
    QByteArray op(pdu->caData), status(pdu->caData + 32);
    QByteArray body(reinterpret_cast<const char *>(pdu->caMsg), static_cast<int>(pdu->uiMsgLen));
    if (m_cancelling && op != "cancel") return;
    if (op != m_pending) { m_socket->abort(); return; }
    m_timeout.stop();
    if (status == "error") { finish(m_transfer.isEmpty() ? QString::fromLatin1(op) : m_transfer, false, QString::fromUtf8(body)); return; }
    if (op == "cancel") { finish("cancel", status == "ok", "传输已取消，未完成文件不会保存。"); return; }
    if (status != "ok" && !(op == "read" && (status == "more" || status == "done"))) { m_socket->abort(); return; }
    if (op == "list") {
        QList<QPair<QString, bool>> entries;
        for (int pos = 0; pos < body.size();) {
            int end = body.indexOf('\0', pos + 1);
            if (end < 0 || (body[pos] != 'D' && body[pos] != 'F') || end == pos + 1) { m_socket->abort(); return; }
            entries.append(qMakePair(QString::fromUtf8(body.constData() + pos + 1, end - pos - 1), body[pos] == 'D')); pos = end + 1;
        }
        m_list->clear();
        for (const auto &entry : entries) {
            auto *item = new QListWidgetItem(style()->standardIcon(entry.second ? QStyle::SP_DirIcon : QStyle::SP_FileIcon), entry.first, m_list);
            item->setData(Qt::UserRole, entry.second);
        }
        m_path = m_requestedPath; m_pathLabel->setText("目录：/" + m_path); finish("list", true, QString("共 %1 项").arg(entries.size()));
    } else if (op == "upload" || op == "chunk") {
        if (op == "chunk") m_done += m_chunk;
        updateProgress(); m_pump.start(12);
    } else if (op == "commit") {
        m_progress->setValue(1000); finish("upload", true, "上传完成。"); flushDir();
    } else if (op == "download") {
        bool ok; m_total = body.toLongLong(&ok);
        if (!ok || m_total < 0 || m_total > MaxFileSize) { cancelTransfer(); return; }
        updateProgress(); m_pump.start(12);
    } else if (op == "read") {
        if (status == "done") {
            if (!body.isEmpty() || m_done != m_total) { finish("download", false, "下载长度校验失败。"); return; }
            bool ok = m_download->commit(); m_progress->setValue(ok ? 1000 : 0);
            finish("download", ok, ok ? "下载完成。" : "保存下载文件失败。");
        } else {
            if (body.isEmpty() || body.size() > ChunkSize || body.size() > m_total - m_done
                || m_download->write(body) != body.size()) { cancelTransfer(); m_status->setText("下载写入或长度校验失败，正在取消。"); return; }
            m_done += body.size(); updateProgress(); m_pump.start(12);
        }
    } else { finish(QString::fromLatin1(op), true, "操作成功。"); flushDir(); }
}
