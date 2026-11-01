#pragma once

#include <QAbstractVideoSurface>
#include <QElapsedTimer>
#include <QImage>
#include <QMutex>

// The backend may call present outside the GUI thread. Keep one owned image;
// the GUI polls it at 10 Hz instead of queuing an event for every camera frame.
class CameraSurface : public QAbstractVideoSurface {
public:
    explicit CameraSurface(QObject *parent = nullptr);
    QList<QVideoFrame::PixelFormat> supportedPixelFormats(
        QAbstractVideoBuffer::HandleType type = QAbstractVideoBuffer::NoHandle) const override;
    bool present(const QVideoFrame &frame) override;
    QImage takeLatest();
    static QImage toImage(const QVideoFrame &frame);

private:
    QMutex mutex_;
    QElapsedTimer throttle_;
    QImage latest_;
};
