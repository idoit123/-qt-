#include "opedb.h"

#include <QCoreApplication>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QDebug>

OpeDB::OpeDB(QObject *parent) : QObject(parent)
{
}

OpeDB &OpeDB::getInstance()
{
    static OpeDB instance;
    return instance;
}

void OpeDB::setDatabasePath(const QString &path)
{
    m_databasePath = path;
}

QString OpeDB::connectionNameForCurrentThread() const
{
    return QStringLiteral("cloud_%1")
        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);
}

QSqlDatabase OpeDB::connectionForCurrentThread()
{
    const QString name = connectionNameForCurrentThread();
    QSqlDatabase db = QSqlDatabase::contains(name)
        ? QSqlDatabase::database(name)
        : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    if (!db.isOpen()) {
        const QString path = m_databasePath.isEmpty()
            ? QCoreApplication::applicationDirPath() + QStringLiteral("/cloud.db")
            : m_databasePath;
        db.setDatabaseName(path);
        if (!db.open()) {
            qWarning() << "Cannot open database" << path << db.lastError().text();
        } else {
            QSqlQuery pragma(db);
            pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
            pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
        }
    }
    return db;
}

void OpeDB::init()
{
    QMutexLocker locker(&m_writeMutex);
    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen()) return;
    QSqlQuery schema(db);
    if (!schema.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS usrInfo ("
            "id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, "
            "pwd TEXT NOT NULL, online INTEGER NOT NULL DEFAULT 0)"))) {
        qWarning() << "Failed to initialize user table" << schema.lastError().text();
        return;
    }
    if (!schema.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS friend ("
            "id INTEGER NOT NULL, friendId INTEGER NOT NULL)"))) {
        qWarning() << "Failed to initialize friend table" << schema.lastError().text();
        return;
    }
    QSqlQuery clearQuery(db);
    if (!clearQuery.exec(QStringLiteral("update usrInfo set online = 0"))) {
        qWarning() << "Failed to reset online state" << clearQuery.lastError().text();
    }
}

void OpeDB::closeConnectionForCurrentThread()
{
    const QString name = connectionNameForCurrentThread();
    if (!QSqlDatabase::contains(name)) return;
    {
        QSqlDatabase db = QSqlDatabase::database(name);
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
}

OpeDB::~OpeDB()
{
    closeConnectionForCurrentThread();
}

bool OpeDB::handelRegist(const char *name, const char *pwd)
{
    QMutexLocker locker(&m_writeMutex);
    if (!name || !pwd) return false;
    QSqlQuery query(connectionForCurrentThread());
    if (!query.prepare(QStringLiteral("INSERT INTO usrInfo(name, pwd) VALUES(:name, :pwd)")))
        return false;
    query.bindValue(QStringLiteral(":name"), QString::fromUtf8(name));
    query.bindValue(QStringLiteral(":pwd"), QString::fromUtf8(pwd));
    return query.exec();
}

bool OpeDB::handelLogin(const char *name, const char *pwd)
{
    QMutexLocker locker(&m_writeMutex);
    if (!name || !pwd) return false;
    QSqlQuery query(connectionForCurrentThread());
    if (!query.prepare(QStringLiteral("UPDATE usrInfo SET online = 1 "
                                      "WHERE name = :name AND pwd = :pwd AND online = 0")))
        return false;
    query.bindValue(QStringLiteral(":name"), QString::fromUtf8(name));
    query.bindValue(QStringLiteral(":pwd"), QString::fromUtf8(pwd));
    return query.exec() && query.numRowsAffected() == 1;
}

void OpeDB::handleOffline(const char *name)
{
    QMutexLocker locker(&m_writeMutex);
    if (!name) return;
    QSqlQuery query(connectionForCurrentThread());
    if (!query.prepare(QStringLiteral("UPDATE usrInfo SET online = 0 WHERE name = :name")))
        return;
    query.bindValue(QStringLiteral(":name"), QString::fromUtf8(name));
    if (!query.exec()) qWarning() << "Failed to clear online state" << query.lastError().text();
}

QStringList OpeDB::handleAllOnline()
{
    QSqlQuery query(connectionForCurrentThread());
    if (!query.exec(QStringLiteral("SELECT name FROM usrInfo WHERE online = 1")))
        return {};
    QStringList result;
    while (query.next()) result.append(query.value(0).toString());
    return result;
}

int OpeDB::handleSearchUsr(const char *name)
{
    if (!name) return -1;
    QSqlQuery query(connectionForCurrentThread());
    if (!query.prepare(QStringLiteral("SELECT online FROM usrInfo WHERE name = :name")))
        return -1;
    query.bindValue(QStringLiteral(":name"), QString::fromUtf8(name));
    if (!query.exec() || !query.next()) return -1;
    return query.value(0).toInt() == 1 ? 1 : 0;
}

/*
 * 处理添加好友请求，
 * 0是朋友，
 * 1不是朋友但是对方在线，
 * 2不是朋友但是对方不在线在线，
 * 3用户不存在，-1其他情况
 */
int OpeDB::handleAddFriendRequest(const char *usrName, const char *friName)
{
    if (!usrName || !friName) return -1;

    const QSqlDatabase db = connectionForCurrentThread();
    QSqlQuery user(db);
    if (!user.prepare(QStringLiteral("SELECT online FROM usrInfo WHERE name = :name")))
        return -1;
    user.bindValue(QStringLiteral(":name"), QString::fromUtf8(friName));
    if (!user.exec()) return -1;
    if (!user.next()) return 3;
    const bool online = user.value(0).toInt() == 1;

    QSqlQuery relation(db);
    if (!relation.prepare(QStringLiteral(
            "SELECT 1 FROM friend WHERE "
            "(id = (SELECT id FROM usrInfo WHERE name = :user1) "
            "AND friendId = (SELECT id FROM usrInfo WHERE name = :friend1)) "
            "OR (id = (SELECT id FROM usrInfo WHERE name = :friend2) "
            "AND friendId = (SELECT id FROM usrInfo WHERE name = :user2)) LIMIT 1")))
        return -1;
    relation.bindValue(QStringLiteral(":user1"), QString::fromUtf8(usrName));
    relation.bindValue(QStringLiteral(":friend1"), QString::fromUtf8(friName));
    relation.bindValue(QStringLiteral(":friend2"), QString::fromUtf8(friName));
    relation.bindValue(QStringLiteral(":user2"), QString::fromUtf8(usrName));
    if (!relation.exec()) return -1;
    return relation.next() ? 0 : (online ? 1 : 2);
}

bool OpeDB::handleAddFriendAgree(const char *usrName, const char *friName)
{
    QMutexLocker locker(&m_writeMutex);
    if (!usrName || !friName) return false;
    QSqlQuery query(connectionForCurrentThread());
    if (!query.prepare(QStringLiteral(
            "INSERT INTO friend(id, friendId) VALUES("
            "(SELECT id FROM usrInfo WHERE name = :user), "
            "(SELECT id FROM usrInfo WHERE name = :friend))")))
        return false;
    query.bindValue(QStringLiteral(":user"), QString::fromUtf8(usrName));
    query.bindValue(QStringLiteral(":friend"), QString::fromUtf8(friName));
    return query.exec();
}

QSet<QString> OpeDB::handleFlushFriendList(const char *usrName)
{
    QSet<QString> result;

    if (usrName == nullptr) {
        return result;
    }

    // 使用 JOIN 关联表，UNION 组合正反向关系，避免 N+1 查询
    QString sql =
        "SELECT u.name FROM friend f "
        "JOIN usrInfo u ON f.friendId = u.id "
        "WHERE f.id = (SELECT id FROM usrInfo WHERE name = :name1) "
        "UNION "
        "SELECT u.name FROM friend f "
        "JOIN usrInfo u ON f.id = u.id "
        "WHERE f.friendId = (SELECT id FROM usrInfo WHERE name = :name2)";

    QSqlQuery query(connectionForCurrentThread());
    query.prepare(sql);
    query.bindValue(":name1", usrName);
    query.bindValue(":name2", usrName);

    if (query.exec()) {
        while (query.next()) {
            result.insert(query.value(0).toString());
        }
    } else {
        qDebug() << "FlushFriendList Query failed:" << query.lastError().text();
    }

    return result;
}

bool OpeDB::handleDeleteFriend(const char *usrName, const char *friName)
{
    QMutexLocker locker(&m_writeMutex);
    if (!usrName || !friName) return false;
    QSqlQuery query(connectionForCurrentThread());
    if (!query.prepare(QStringLiteral(
            "DELETE FROM friend WHERE "
            "(id = (SELECT id FROM usrInfo WHERE name = :user1) "
            "AND friendId = (SELECT id FROM usrInfo WHERE name = :friend1)) "
            "OR (id = (SELECT id FROM usrInfo WHERE name = :friend2) "
            "AND friendId = (SELECT id FROM usrInfo WHERE name = :user2))")))
        return false;
    query.bindValue(QStringLiteral(":user1"), QString::fromUtf8(usrName));
    query.bindValue(QStringLiteral(":friend1"), QString::fromUtf8(friName));
    query.bindValue(QStringLiteral(":friend2"), QString::fromUtf8(friName));
    query.bindValue(QStringLiteral(":user2"), QString::fromUtf8(usrName));
    return query.exec() && query.numRowsAffected() > 0;
}
