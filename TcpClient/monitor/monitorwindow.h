#pragma once

#include <QImage>
#include <QThread>
#include <QWidget>

class CameraSurface;
class VideoWorker;
class QCamera;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

class MonitorWindow : public QWidget {
    Q_OBJECT
public:
    explicit MonitorWindow(QWidget *parent = nullptr);
    ~MonitorWindow() override;

signals:
    void connectRequested(const QString &host, quint16 port, const QString &user,
                          const QString &password, bool registration);
    void disconnectRequested();
    void uploadRequested(const QImage &image);
    void resetBackgroundRequested();

private:
    void connectServer(bool registration);
    void startCamera();
    void stopCamera();
    void startDemo();
    void submit(const QImage &image);
    void setConnected(bool connected);
    QLineEdit *host_, *user_, *password_;
    QSpinBox *port_;
    QComboBox *devices_;
    QPushButton *connect_, *register_, *disconnect_, *start_, *stop_, *test_, *demo_, *reset_;
    QLabel *local_, *remote_, *status_, *stats_, *motion_;
    CameraSurface *surface_;
    QCamera *camera_ = nullptr;
    QTimer *preview_;
    QTimer *demoTimer_;
    int demoFrame_ = 0;
    QThread networkThread_;
    VideoWorker *worker_;
    bool connected_ = false;
    bool waiting_ = false;
    quint64 displayed_ = 0;
};

int runMonitorSmoke(class QApplication &app);
