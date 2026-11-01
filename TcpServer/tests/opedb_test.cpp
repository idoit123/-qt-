#include "opedb.h"

#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <thread>

class OpeDbTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void registrationAndLogin();
    void injectionInputIsLiteral();
    void friendRelationships();
    void concurrentLoginOnlySucceedsOnce();

private:
    QTemporaryDir m_tempDir;
};

void OpeDbTest::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    const QString databasePath = m_tempDir.filePath(QStringLiteral("test.db"));
    OpeDB::getInstance().setDatabasePath(databasePath);
    OpeDB::getInstance().init();
}

void OpeDbTest::cleanupTestCase()
{
    OpeDB::getInstance().closeConnectionForCurrentThread();
}

void OpeDbTest::registrationAndLogin()
{
    OpeDB &db = OpeDB::getInstance();
    QVERIFY(db.handelRegist("alice", "secret"));
    QVERIFY(!db.handelRegist("alice", "secret"));
    QCOMPARE(db.handleSearchUsr("alice"), 0);
    QVERIFY(!db.handelLogin("alice", "wrong"));
    QVERIFY(db.handelLogin("alice", "secret"));
    QVERIFY(!db.handelLogin("alice", "secret"));
    QCOMPARE(db.handleSearchUsr("alice"), 1);
    QVERIFY(db.handleAllOnline().contains(QStringLiteral("alice")));
    db.handleOffline("alice");
    QCOMPARE(db.handleSearchUsr("alice"), 0);
}

void OpeDbTest::injectionInputIsLiteral()
{
    OpeDB &db = OpeDB::getInstance();
    QVERIFY(db.handelRegist("quoted'user", "secret"));
    QVERIFY(db.handelLogin("quoted'user", "secret"));
    QVERIFY(!db.handelLogin("alice", "' OR 1=1 --"));
    QCOMPARE(db.handleSearchUsr("' OR 1=1 --"), -1);
    QCOMPARE(db.handleAddFriendRequest("alice", "' OR 1=1 --"), 3);
    db.handleOffline("quoted'user");
}

void OpeDbTest::friendRelationships()
{
    OpeDB &db = OpeDB::getInstance();
    QVERIFY(db.handelRegist("friend_a", "pw"));
    QVERIFY(db.handelRegist("friend_b", "pw"));
    QCOMPARE(db.handleAddFriendRequest("friend_a", "friend_b"), 2);
    QVERIFY(db.handleAddFriendAgree("friend_a", "friend_b"));
    QCOMPARE(db.handleAddFriendRequest("friend_a", "friend_b"), 0);
    QVERIFY(db.handleFlushFriendList("friend_a").contains(QStringLiteral("friend_b")));
    QVERIFY(db.handleDeleteFriend("friend_a", "friend_b"));
    QVERIFY(!db.handleDeleteFriend("friend_a", "friend_b"));
}

void OpeDbTest::concurrentLoginOnlySucceedsOnce()
{
    OpeDB &db = OpeDB::getInstance();
    QVERIFY(db.handelRegist("parallel", "pw"));
    std::atomic<int> successes(0);
    auto attempt = [&successes]() {
        if (OpeDB::getInstance().handelLogin("parallel", "pw")) ++successes;
        OpeDB::getInstance().closeConnectionForCurrentThread();
    };
    std::thread first(attempt);
    std::thread second(attempt);
    first.join();
    second.join();
    QCOMPARE(successes.load(), 1);
    db.handleOffline("parallel");
}

QTEST_GUILESS_MAIN(OpeDbTest)
#include "opedb_test.moc"
