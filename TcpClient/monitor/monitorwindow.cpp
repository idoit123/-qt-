#include "monitorwindow.h"
#include "camerasurface.h"
#include "videoworker.h"

#include <QApplication>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <iostream>

namespace {
QImage testImage()
{
    QImage image(640, 480, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(0, 0, 213, 480, Qt::red);
    painter.fillRect(213, 0, 214, 480, Qt::green);
    painter.fillRect(427, 0, 213, 480, Qt::blue);
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Arial"), 22));
    painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("COLOR UPLOAD → MOTION DETECTION"));
    return image;
}

QImage demoImage(int frame)
{
    QImage image(640, 480, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(image.rect(), QColor(35, 48, 66));
    painter.fillRect(40, 60, 130, 120, QColor(50, 95, 140));
    painter.fillRect(470, 70, 120, 100, QColor(110, 75, 45));
    if (frame >= 25) {
        const int x = 60 + ((frame - 25) * 8) % 460;
        // Separated patches move together: spatial grouping should outline
        // the whole cluster rather than produce one box for every patch.
        painter.fillRect(x, 260, 26, 26, QColor(230, 90, 50));
        painter.fillRect(x + 42, 260, 26, 26, QColor(230, 90, 50));
        painter.fillRect(x + 18, 310, 28, 28, QColor(230, 90, 50));
    }
    return image;
}

void showImage(QLabel *label, const QImage &image)
{
    label->setPixmap(QPixmap::fromImage(image).scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
}

MonitorWindow::MonitorWindow(QWidget *parent) : QWidget(parent),
    surface_(new CameraSurface(this)), preview_(new QTimer(this)), demoTimer_(new QTimer(this)),
    worker_(new VideoWorker)
{
    setWindowTitle(QStringLiteral("远程视频监控 · Qt + Linux OpenCV"));
    resize(1120, 650);
    host_ = new QLineEdit(qEnvironmentVariable("CHAT_SERVER_IP", "127.0.0.1"), this);
    port_ = new QSpinBox(this); port_->setRange(1, 65535); port_->setValue(18888);
    user_ = new QLineEdit(QStringLiteral("video_demo"), this);
    password_ = new QLineEdit(this); password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(QStringLiteral("首次使用请填写密码并注册"));
    connect_ = new QPushButton(QStringLiteral("连接"), this);
    register_ = new QPushButton(QStringLiteral("注册并连接"), this);
    disconnect_ = new QPushButton(QStringLiteral("断开"), this);
    devices_ = new QComboBox(this);
    for (const auto &device : QCameraInfo::availableCameras())
        devices_->addItem(device.description(), device.deviceName());
    if (!devices_->count()) devices_->addItem(QStringLiteral("未发现摄像头，可先发送测试图"));
    start_ = new QPushButton(QStringLiteral("启动摄像头"), this);
    stop_ = new QPushButton(QStringLiteral("停止采集"), this);
    test_ = new QPushButton(QStringLiteral("发送彩色测试图"), this);
    demo_ = new QPushButton(QStringLiteral("运动演示"), this);
    reset_ = new QPushButton(QStringLiteral("重置背景"), this);
    local_ = new QLabel(QStringLiteral("本地彩色预览"), this);
    remote_ = new QLabel(QStringLiteral("等待服务器返回运动检测画面"), this);
    for (auto *label : {local_, remote_}) {
        label->setMinimumSize(400, 300); label->setAlignment(Qt::AlignCenter);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        label->setStyleSheet("background:#101827;color:#b8c5d6;border:1px solid #34445d;");
    }
    status_ = new QLabel(QStringLiteral("填写服务器和账号。首次使用点击“注册并连接”。"), this);
    status_->setWordWrap(true);
    stats_ = new QLabel(QStringLiteral("单路 JPEG / 最多 640×480 / 目标 10 FPS / 一帧在途"), this);
    motion_ = new QLabel(QStringLiteral("运动状态：等待视频；启动后先保持摄像头稳定约 2 秒"), this);
    motion_->setStyleSheet("color:#526174;font-weight:bold;");
    host_->setObjectName("serverHost"); port_->setObjectName("serverPort");
    user_->setObjectName("userName"); password_->setObjectName("password");
    register_->setObjectName("registerConnect"); connect_->setObjectName("connectServer");
    disconnect_->setObjectName("disconnectServer"); test_->setObjectName("sendTestImage");
    remote_->setObjectName("remoteImage"); stats_->setObjectName("videoStats");
    motion_->setObjectName("motionStatus"); reset_->setObjectName("resetBackground");
    demo_->setObjectName("motionDemo"); stop_->setObjectName("stopCapture");
    auto *layout = new QGridLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("服务器")), 0, 0); layout->addWidget(host_, 0, 1);
    layout->addWidget(new QLabel(QStringLiteral("端口")), 0, 2); layout->addWidget(port_, 0, 3);
    layout->addWidget(new QLabel(QStringLiteral("用户名")), 1, 0); layout->addWidget(user_, 1, 1);
    layout->addWidget(new QLabel(QStringLiteral("密码")), 1, 2); layout->addWidget(password_, 1, 3);
    auto *buttons = new QHBoxLayout;
    for (auto *button : {connect_, register_, disconnect_}) buttons->addWidget(button);
    layout->addLayout(buttons, 2, 0, 1, 4);
    auto *capture = new QHBoxLayout;
    capture->addWidget(devices_);
    for (auto *button : {start_, stop_, test_, demo_, reset_}) capture->addWidget(button);
    layout->addLayout(capture, 3, 0, 1, 4);
    layout->addWidget(new QLabel(QStringLiteral("本地原图（上传）")), 4, 0, 1, 2);
    layout->addWidget(new QLabel(QStringLiteral("Linux OpenCV 运动检测（绿色框为运动区域）")), 4, 2, 1, 2);
    layout->addWidget(local_, 5, 0, 1, 2); layout->addWidget(remote_, 5, 2, 1, 2);
    layout->addWidget(motion_, 6, 0, 1, 4);
    layout->addWidget(status_, 7, 0, 1, 4); layout->addWidget(stats_, 8, 0, 1, 4);
    layout->setRowStretch(5, 1);
    worker_->moveToThread(&networkThread_);
    connect(&networkThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &MonitorWindow::connectRequested, worker_, &VideoWorker::open);
    connect(this, &MonitorWindow::disconnectRequested, worker_, &VideoWorker::close);
    connect(this, &MonitorWindow::uploadRequested, worker_, &VideoWorker::upload);
    connect(this, &MonitorWindow::resetBackgroundRequested, worker_, &VideoWorker::resetBackground);
    connect(worker_, &VideoWorker::status, status_, &QLabel::setText);
    connect(worker_, &VideoWorker::ready, this, [this] { waiting_ = false; setConnected(true); });
    connect(worker_, &VideoWorker::closed, this, [this] {
        waiting_ = false; stopCamera(); setConnected(false); status_->setText(QStringLiteral("连接已断开"));
        motion_->setText(QStringLiteral("运动状态：未连接"));
    });
    connect(worker_, &VideoWorker::frameReceived, this,
            [this](const QImage &image, quint64 sequence, qint64 ms, const QString &motion) {
        waiting_ = false; ++displayed_; showImage(remote_, image);
        stats_->setText(QStringLiteral("收到检测帧 #%1 · 已显示 %2 帧 · 本次往返 %3 ms（编码后至解码完成）")
                       .arg(sequence).arg(displayed_).arg(ms));
        if (motion.startsWith("warmup:")) {
            motion_->setText(QStringLiteral("背景学习中 %1，请保持摄像头稳定").arg(motion.mid(7)));
            motion_->setStyleSheet("color:#9a6700;font-weight:bold;");
        } else if (motion.startsWith("motion:")) {
            motion_->setText(QStringLiteral("检测到运动 · %1 个运动区域（不代表人数）").arg(motion.mid(7)));
            motion_->setStyleSheet("color:#b42318;font-weight:bold;");
        } else if (motion == "idle:0") {
            motion_->setText(QStringLiteral("暂无运动"));
            motion_->setStyleSheet("color:#16803b;font-weight:bold;");
        } else motion_->setText(QStringLiteral("服务端未提供运动状态，请更新并重启服务端"));
    });
    connect(worker_, &VideoWorker::frameRejected, this, [this](const QString &reason) {
        waiting_ = false; status_->setText(QStringLiteral("帧处理失败：%1").arg(reason)); stopCamera();
    });
    connect(connect_, &QPushButton::clicked, this, [this] { connectServer(false); });
    connect(register_, &QPushButton::clicked, this, [this] { connectServer(true); });
    connect(disconnect_, &QPushButton::clicked, this, [this] { emit disconnectRequested(); });
    connect(start_, &QPushButton::clicked, this, &MonitorWindow::startCamera);
    connect(stop_, &QPushButton::clicked, this, &MonitorWindow::stopCamera);
    connect(test_, &QPushButton::clicked, this, [this] {
        stopCamera(); emit resetBackgroundRequested(); submit(testImage());
    });
    connect(demo_, &QPushButton::clicked, this, &MonitorWindow::startDemo);
    connect(reset_, &QPushButton::clicked, this, [this] {
        emit resetBackgroundRequested();
        motion_->setText(QStringLiteral("已请求重置背景，将从下一帧重新学习"));
    });
    connect(demoTimer_, &QTimer::timeout, this, [this] {
        if (waiting_ || !connected_) return;
        submit(demoImage(demoFrame_++));
        if (demoFrame_ >= 90) {
            stopCamera(); status_->setText(QStringLiteral("运动演示结束，可重新演示或启动摄像头"));
        }
    });
    connect(preview_, &QTimer::timeout, this, [this] {
        const QImage image = surface_->takeLatest();
        if (!image.isNull()) submit(image);
    });
    networkThread_.start();
    setConnected(false);
}

MonitorWindow::~MonitorWindow()
{
    stopCamera();
    QMetaObject::invokeMethod(worker_, "close", Qt::BlockingQueuedConnection);
    networkThread_.quit(); networkThread_.wait();
}

void MonitorWindow::setConnected(bool connected)
{
    connected_ = connected;
    for (auto *button : {connect_, register_}) button->setEnabled(!connected);
    for (auto *button : {disconnect_, test_, demo_, reset_}) button->setEnabled(connected);
    host_->setEnabled(!connected); port_->setEnabled(!connected);
    user_->setEnabled(!connected); password_->setEnabled(!connected);
    start_->setEnabled(connected && !camera_ && !devices_->currentData().toString().isEmpty());
    stop_->setEnabled(camera_ != nullptr || demoTimer_->isActive());
}

void MonitorWindow::connectServer(bool registration)
{
    connect_->setEnabled(false); register_->setEnabled(false); disconnect_->setEnabled(true);
    emit connectRequested(host_->text().trimmed(), static_cast<quint16>(port_->value()),
                          user_->text(), password_->text(), registration);
}

void MonitorWindow::startCamera()
{
    if (camera_ || !connected_) return;
    stopCamera();
    emit resetBackgroundRequested();
    const auto devices = QCameraInfo::availableCameras();
    for (const auto &device : devices) {
        if (device.deviceName() == devices_->currentData().toString()) {
            camera_ = new QCamera(device, this); break;
        }
    }
    if (!camera_) { status_->setText(QStringLiteral("找不到所选摄像头，请重新打开程序刷新设备")); return; }
    camera_->setViewfinder(surface_);
    QCameraViewfinderSettings settings; settings.setResolution(640, 480);
    camera_->setViewfinderSettings(settings);
    connect(camera_, QOverload<QCamera::Error>::of(&QCamera::error), this, [this](QCamera::Error) {
        status_->setText(QStringLiteral("摄像头错误：%1；请检查 Windows 相机权限及设备占用").arg(camera_->errorString()));
        QTimer::singleShot(0, this, &MonitorWindow::stopCamera);
    });
    camera_->start(); preview_->start(100);
    status_->setText(QStringLiteral("摄像头采集中：固定摄像头，背景学习结束后挥手或移动物体查看绿色框"));
    setConnected(connected_);
}

void MonitorWindow::stopCamera()
{
    preview_->stop();
    demoTimer_->stop();
    if (camera_) {
        QCamera *camera = camera_; camera_ = nullptr;
        camera->disconnect(this); camera->stop(); delete camera;
    }
    surface_->takeLatest();
    start_->setEnabled(connected_ && !devices_->currentData().toString().isEmpty());
    stop_->setEnabled(false);
}

void MonitorWindow::startDemo()
{
    if (!connected_) return;
    stopCamera();
    emit resetBackgroundRequested();
    demoFrame_ = 0;
    demoTimer_->start(100);
    stop_->setEnabled(true);
    status_->setText(QStringLiteral("合成运动演示：先学习静止场景，再检测移动方块；未打开摄像头"));
}

void MonitorWindow::submit(const QImage &image)
{
    showImage(local_, image);
    if (!connected_ || waiting_) return; // Never accumulate upload tasks.
    waiting_ = true;
    emit uploadRequested(image);
}

// Real Qt socket/JPEG interoperability check, without opening the webcam.
int runMonitorSmoke(QApplication &app)
{
    VideoWorker worker;
    int received = 0;
    const auto args = app.arguments();
    const auto arg = [&args](const QString &key, const QString &fallback) {
        const int pos = args.indexOf(key);
        return pos >= 0 && pos + 1 < args.size() ? args[pos + 1] : fallback;
    };
    const QString host = arg("--host", "127.0.0.1");
    const auto port = static_cast<quint16>(arg("--port", "18888").toUShort());
    const QString user = arg("--user", "smoke");
    QObject::connect(&worker, &VideoWorker::status, &app, [](const QString &text) {
        std::cout << text.toUtf8().constData() << std::endl;
    });
    QObject::connect(&worker, &VideoWorker::ready, &app, [&worker] { worker.upload(demoImage(0)); });
    QObject::connect(&worker, &VideoWorker::frameRejected, &app, [&app](const QString &reason) {
        std::cerr << reason.toStdString() << std::endl; app.exit(2);
    });
    QObject::connect(&worker, &VideoWorker::frameReceived, &app,
                     [&](const QImage &image, quint64, qint64, const QString &motion) {
        if (image.size() != QSize(640, 480)) { app.exit(3); return; }
        const bool correct = received < 20 ? motion.startsWith("warmup:")
                            : received < 26 ? motion == "idle:0" : motion == "motion:1";
        if (!correct) { std::cerr << "Unexpected motion state: " << motion.toStdString() << std::endl; app.exit(4); return; }
        if (++received == 30) { std::cout << "QT_MOTION_SMOKE_PASS frames=30" << std::endl; app.exit(0); return; }
        QTimer::singleShot(30, &worker, [&worker, received] { worker.upload(demoImage(received)); });
    });
    QTimer::singleShot(0, &worker, [&worker, host, port, user] { worker.open(host, port, user, "smoke-pw", true); });
    QTimer::singleShot(20000, &app, [&app] { app.exit(6); });
    return app.exec();
}
