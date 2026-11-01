#include "../monitor/camerasurface.h"
#include <QtTest>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>

class CameraSurfaceTest : public QObject {
    Q_OBJECT
private slots:
    void ownedRgbPixels()
    {
        QImage original(4, 4, QImage::Format_RGB32);
        original.fill(Qt::red);
        QImage converted = CameraSurface::toImage(QVideoFrame(original));
        original.fill(Qt::blue);
        QCOMPARE(converted.pixelColor(0, 0), QColor(Qt::red));
    }
    void packedYuyv()
    {
        QVideoFrame frame(8, QSize(2, 2), 4, QVideoFrame::Format_YUYV);
        QVERIFY(frame.map(QAbstractVideoBuffer::WriteOnly));
        const uchar pixels[] = {82, 90, 82, 240, 82, 90, 82, 240};
        memcpy(frame.bits(), pixels, sizeof(pixels));
        frame.unmap();
        const auto image = CameraSurface::toImage(frame);
        QCOMPARE(image.size(), QSize(2, 2));
        const auto color = image.pixelColor(1, 1);
        QVERIFY(color.red() > 245 && color.green() < 5 && color.blue() < 5);
    }
    void latestFrameOnly()
    {
        CameraSurface surface;
        QImage original(640, 480, QImage::Format_RGB32);
        original.fill(Qt::green);
        QVERIFY(surface.present(QVideoFrame(original)));
        QVERIFY(!surface.takeLatest().isNull());
        QVERIFY(surface.takeLatest().isNull());
    }
    void realCameraOptional()
    {
        if (!qEnvironmentVariableIsSet("VIDEO_TEST_CAMERA")) QSKIP("Set VIDEO_TEST_CAMERA=1 for opt-in webcam test");
        const auto devices = QCameraInfo::availableCameras();
        QVERIFY2(!devices.isEmpty(), "No camera detected");
        qInfo() << "Camera:" << devices.first().description();
        CameraSurface surface;
        QCamera camera(devices.first());
        camera.setViewfinder(&surface);
        QCameraViewfinderSettings settings; settings.setResolution(640, 480);
        camera.setViewfinderSettings(settings);
        camera.start();
        QElapsedTimer elapsed; elapsed.start();
        int frames = 0;
        while (elapsed.elapsed() < 15000 && frames < 5) {
            QTest::qWait(100);
            if (!surface.takeLatest().isNull()) ++frames;
        }
        camera.stop();
        QVERIFY2(frames == 5, qPrintable(camera.errorString()));
        qInfo() << "CAMERA_CAPTURE_PASS frames=" << frames << "(not saved)";
    }
};

QTEST_MAIN(CameraSurfaceTest)
#include "camerasurface_test.moc"
