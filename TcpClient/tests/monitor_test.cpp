#include "../monitor/monitorwindow.h"
#include <QtTest>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

class MonitorTest : public QObject {
    Q_OBJECT
private slots:
    void invalidAccountCanRetry()
    {
        MonitorWindow window;
        auto *button = window.findChild<QPushButton *>("registerConnect");
        button->click(); // Empty password must re-enable the buttons.
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 2000);
    }
    void realServerRoundTrip()
    {
        if (!qEnvironmentVariableIsSet("VIDEO_TEST_PORT")) QSKIP("Set VIDEO_TEST_PORT for real server GUI test");
        MonitorWindow window;
        window.resize(1120, 650);
        window.show();
        auto *host = window.findChild<QLineEdit *>("serverHost");
        auto *port = window.findChild<QSpinBox *>("serverPort");
        auto *user = window.findChild<QLineEdit *>("userName");
        auto *password = window.findChild<QLineEdit *>("password");
        auto *registration = window.findChild<QPushButton *>("registerConnect");
        auto *test = window.findChild<QPushButton *>("sendTestImage");
        auto *disconnect = window.findChild<QPushButton *>("disconnectServer");
        auto *stats = window.findChild<QLabel *>("videoStats");
        auto *remote = window.findChild<QLabel *>("remoteImage");
        auto *motion = window.findChild<QLabel *>("motionStatus");
        auto *demo = window.findChild<QPushButton *>("motionDemo");
        auto *reset = window.findChild<QPushButton *>("resetBackground");
        auto *stop = window.findChild<QPushButton *>("stopCapture");
        host->setText(qEnvironmentVariable("VIDEO_TEST_HOST", "127.0.0.1"));
        port->setValue(qEnvironmentVariableIntValue("VIDEO_TEST_PORT"));
        user->setText(QString("gui%1").arg(QDateTime::currentMSecsSinceEpoch()));
        password->setText("gui-test-pw");
        registration->click();
        QTRY_VERIFY_WITH_TIMEOUT(test->isEnabled(), 10000);
        test->click();
        QTRY_VERIFY_WITH_TIMEOUT(stats->text().contains(QStringLiteral("已显示 1 帧")), 7000);
        const QPixmap result = *remote->pixmap();
        QVERIFY(!result.isNull());
        const auto image = result.toImage();
        const auto pixel = image.pixelColor(image.width() / 6, image.height() / 4);
        QVERIFY(pixel.red() > 200 && pixel.green() < 50 && pixel.blue() < 50);
        QVERIFY(motion->text().contains(QStringLiteral("背景学习中")));
        demo->click();
        QTRY_VERIFY_WITH_TIMEOUT(motion->text().contains(QStringLiteral("暂无运动")), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(motion->text().contains(QStringLiteral("检测到运动")), 5000);
        window.grab().save("monitor-motion-ui.png"); // Synthetic scene only; never webcam pixels.
        reset->click();
        QTRY_VERIFY_WITH_TIMEOUT(motion->text().contains(QStringLiteral("背景学习中")), 3000);
        stop->click();
        QVERIFY(!stop->isEnabled());
        disconnect->click();
        QTRY_VERIFY_WITH_TIMEOUT(registration->isEnabled(), 3000);
        QVERIFY(!test->isEnabled());
    }
};

QTEST_MAIN(MonitorTest)
#include "monitor_test.moc"
