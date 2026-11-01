#include "camerasurface.h"
#include <QMutexLocker>
#include <QVideoSurfaceFormat>

CameraSurface::CameraSurface(QObject *parent) : QAbstractVideoSurface(parent) {}

QList<QVideoFrame::PixelFormat> CameraSurface::supportedPixelFormats(
    QAbstractVideoBuffer::HandleType type) const
{
    if (type != QAbstractVideoBuffer::NoHandle) return {};
    return {QVideoFrame::Format_RGB32, QVideoFrame::Format_ARGB32,
            QVideoFrame::Format_ARGB32_Premultiplied, QVideoFrame::Format_RGB24,
            QVideoFrame::Format_BGR24, QVideoFrame::Format_YUYV,
            QVideoFrame::Format_UYVY, QVideoFrame::Format_NV12,
            QVideoFrame::Format_NV21, QVideoFrame::Format_YUV420P,
            QVideoFrame::Format_YV12, QVideoFrame::Format_Jpeg};
}

namespace {
QRgb yuvRgb(int y, int u, int v)
{
    const int c = qMax(0, y - 16), d = u - 128, e = v - 128;
    return qRgb(qBound(0, (298*c + 409*e + 128) >> 8, 255),
                qBound(0, (298*c - 100*d - 208*e + 128) >> 8, 255),
                qBound(0, (298*c + 516*d + 128) >> 8, 255));
}
}

QImage CameraSurface::toImage(const QVideoFrame &source)
{
    QVideoFrame frame(source);
    if (!frame.isValid() || !frame.map(QAbstractVideoBuffer::ReadOnly)) return {};
    QImage result;
    const auto format = QVideoFrame::imageFormatFromPixelFormat(frame.pixelFormat());
    const int w = frame.width(), h = frame.height();
    if (format != QImage::Format_Invalid) {
        result = QImage(frame.bits(), w, h, frame.bytesPerLine(), format).copy();
    } else if (frame.pixelFormat() == QVideoFrame::Format_Jpeg) {
        result = QImage::fromData(frame.bits(), frame.mappedBytes(), "JPEG");
    } else if (frame.pixelFormat() == QVideoFrame::Format_BGR24) {
        result = QImage(frame.bits(), w, h, frame.bytesPerLine(), QImage::Format_RGB888).rgbSwapped();
    } else if ((w % 2) == 0 && (h % 2) == 0) {
        const auto pixel = frame.pixelFormat();
        const bool packed = pixel == QVideoFrame::Format_YUYV || pixel == QVideoFrame::Format_UYVY;
        const bool semi = pixel == QVideoFrame::Format_NV12 || pixel == QVideoFrame::Format_NV21;
        const bool planar = pixel == QVideoFrame::Format_YUV420P || pixel == QVideoFrame::Format_YV12;
        if ((packed && frame.bytesPerLine() >= w * 2)
                || ((semi || planar) && frame.planeCount() >= (semi ? 2 : 3))) {
            result = QImage(w, h, QImage::Format_RGB32);
            for (int y = 0; y < h; ++y) {
                const uchar *row = frame.bits() + y * frame.bytesPerLine();
                QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    int yy, u, v;
                    if (packed) {
                        const uchar *p = row + (x / 2) * 4;
                        const bool uyvy = pixel == QVideoFrame::Format_UYVY;
                        yy = p[(x % 2) * 2 + (uyvy ? 1 : 0)];
                        u = p[uyvy ? 0 : 1]; v = p[uyvy ? 2 : 3];
                    } else {
                        yy = row[x];
                        const uchar *uv = frame.bits(1) + (y / 2) * frame.bytesPerLine(1);
                        if (semi) {
                            const int base = (x / 2) * 2;
                            const bool vu = pixel == QVideoFrame::Format_NV21;
                            u = uv[base + (vu ? 1 : 0)]; v = uv[base + (vu ? 0 : 1)];
                        } else {
                            const uchar *other = frame.bits(2) + (y / 2) * frame.bytesPerLine(2);
                            const bool vu = pixel == QVideoFrame::Format_YV12;
                            u = (vu ? other : uv)[x / 2]; v = (vu ? uv : other)[x / 2];
                        }
                    }
                    out[x] = yuvRgb(yy, u, v);
                }
            }
        }
    }
    frame.unmap(); // All paths above return owned pixels, never mapped memory.
    return result;
}

bool CameraSurface::present(const QVideoFrame &frame)
{
    QMutexLocker lock(&mutex_);
    if (throttle_.isValid() && throttle_.elapsed() < 100) return true;
    throttle_.start();
    QImage image = toImage(frame);
    if (image.isNull()) return false;
    if (surfaceFormat().scanLineDirection() == QVideoSurfaceFormat::BottomToTop)
        image = image.mirrored(false, true);
    latest_ = image.scaled(640, 480, Qt::KeepAspectRatio, Qt::FastTransformation);
    return true;
}

QImage CameraSurface::takeLatest()
{
    QMutexLocker lock(&mutex_);
    QImage image;
    image.swap(latest_);
    return image;
}
