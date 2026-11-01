#ifndef BOOK_H
#define BOOK_H
#include <QWidget>
#include <QFile>
#include <QSaveFile>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#include "protocol.h"
class Book : public QWidget {
    Q_OBJECT
public:
    explicit Book(QTcpSocket *socket, QWidget *parent = nullptr);
    void handleResponse(const PDU *pdu);
    void reset();
    bool busy() const { return m_busy; }
    QString currentPath() const { return m_path; }
    QString getShareFileName() const;
    void listPath(const QString &path);
    void makeDirectory(const QString &name);
    void renameEntry(const QString &name, const QString &newName);
    void removeEntry(const QString &name, bool directory);
    void startUpload(const QString &localPath, const QString &remoteName);
    void startDownload(const QString &remoteName, const QString &localPath);
public slots:
    void flushDir();
    void cancelTransfer();
signals:
    void operationFinished(const QString &operation, bool ok, const QString &message);
private:
    bool send(const QByteArray &op, const QByteArray &body = {}, const QByteArray &argument = {});
    void finish(const QString &op, bool ok, const QString &message);
    void setBusy(bool busy);
    void nextUpload();
    void nextDownload();
    void updateProgress();
    QString childPath(const QString &name) const;
    bool validName(const QString &name);
    void deleteSelected();
    QTcpSocket *m_socket;
    QListWidget *m_list;
    QLabel *m_pathLabel, *m_status;
    QProgressBar *m_progress;
    QPushButton *m_cancel;
    QList<QPushButton *> m_actions;
    QTimer m_timeout;
    QTimer m_pump;
    QFile m_upload;
    std::unique_ptr<QSaveFile> m_download;
    QString m_path, m_requestedPath, m_transfer;
    QByteArray m_pending;
    qint64 m_total = 0, m_done = 0, m_chunk = 0;
    bool m_busy = false, m_cancelling = false;
};
#endif
