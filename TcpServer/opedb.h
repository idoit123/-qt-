#ifndef OPEDB_H
#define OPEDB_H

#include <QObject>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QtSql/QSqlDatabase>

class OpeDB : public QObject
{
    Q_OBJECT
public:
    explicit OpeDB(QObject *parent = nullptr);
    static OpeDB &getInstance();
    void setDatabasePath(const QString &path);
    void init();
    ~OpeDB();
    void closeConnectionForCurrentThread();

    bool handelRegist(const char *name, const char *pwd);
    bool handelLogin(const char *name, const char *pwd);
    void handleOffline(const char *name);
    QStringList handleAllOnline();
    int handleSearchUsr(const char *name);
    int handleAddFriendRequest(const char *usrName, const char *friName);
    bool handleAddFriendAgree(const char *usrName, const char *friName);
    QSet<QString> handleFlushFriendList(const char *usrName);
    bool handleDeleteFriend(const char *usrName, const char *friName);

private:
    QSqlDatabase connectionForCurrentThread();
    QString connectionNameForCurrentThread() const;

    QString m_databasePath;
    QMutex m_writeMutex;
};

#endif // OPEDB_H