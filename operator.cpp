#include <QObject>
#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QTimer>
#include <QMetaObject>
#include <QImage>
#include <QPixmap>

#include <opencv2/opencv.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <thread>
#include <atomic>
#include <iostream>
#include <vector>
#include <csignal>
#include <cstring>

#define RASPBERRY_IP "192.168.0.103"  // IP raspberry дома
// #define RASPBERRY_IP "192.168.31.34"  // IP raspberry в аудитории
#define SERVER_PORT     12345
#define VIDEO_PORT      12346
#define LOGS_PORT       12347
#define HEARTBEAT_PORT  12348

std::atomic<bool> running(true);
std::atomic<bool> running_logs(true);
std::atomic<bool> do_not_stop(false);

int command_sock = -1;

// --- Поток heartbeat ---
void send_heartbeat() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HEARTBEAT_PORT);
    inet_pton(AF_INET, RASPBERRY_IP, &addr.sin_addr);

    while (running) {
        const char* msg = "1";
        sendto(sock, msg, std::strlen(msg), 0, (sockaddr*)&addr, sizeof(addr));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    close(sock);
}

// --- Поток логов ---
void receive_logs(QTextEdit* log_widget) {
    if (!log_widget) return;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(LOGS_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (sockaddr*)&local, sizeof(local)) < 0) {
        close(sock);
        return;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    char buffer[512];
    sockaddr_in client{};
    socklen_t client_len = sizeof(client);

    while (running_logs) {
        int n = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&client, &client_len);
        if (n > 0) {
            buffer[n] = '\0';
            QString msg = QString::fromUtf8(buffer);

            QMetaObject::invokeMethod(
                log_widget,
                [log_widget, msg]() { log_widget->append(msg); },
                Qt::QueuedConnection
            );
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    close(sock);
}

// --- Главный класс окна ---
class ControllerWindow : public QWidget {
    Q_OBJECT
public:
    explicit ControllerWindow(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(800, 600);

        video_label = new QLabel(this);
        video_label->setFixedSize(640, 480);
        video_label->setText("Waiting for video...");

        log_widget = new QTextEdit(this);
        log_widget->setReadOnly(true);

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->addWidget(video_label);
        layout->addWidget(log_widget);
        setLayout(layout);

        // --- Вывод версии OpenCV ---
        log_widget->append(QString("[INFO] OpenCV version: %1").arg(CV_VERSION));

        // --- Проверяем поддержку GStreamer в сборке ---
        std::string build_info = cv::getBuildInformation();
        bool gst_support = (build_info.find("GStreamer:                    YES") != std::string::npos);
        log_widget->append(QString("[INFO] GStreamer support: %1").arg(gst_support ? "YES" : "NO"));

        // --- Пытаемся открыть UDP H264 поток ---
        const std::string gst_pipeline =
            "udpsrc port=12346 caps=application/x-rtp,media=video,encoding-name=H264,payload=96 "
            "! rtph264depay "
            "! avdec_h264 "
            "! videoconvert "
            "! appsink sync=false";

        log_widget->append("[INFO] Trying to open UDP H264 stream...");
        if (!cap.open(gst_pipeline, cv::CAP_GSTREAMER)) {
            video_ready = false;
            log_widget->append("[ERROR] Failed to open UDP H264 stream!");
            log_widget->append("[INFO] Possible reasons:");
            log_widget->append("  1) OpenCV built without GStreamer support");
            log_widget->append("  2) Missing GStreamer plugins (rtph264depay, avdec_h264)");
            log_widget->append("  3) Wrong port/IP or firewall blocks the stream");
            video_label->setText("Video not available");
        } else {
            video_ready = true;
            log_widget->append("[OK] Video stream opened successfully.");
        }

        // --- Таймер обновления кадров ---
        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &ControllerWindow::updateFrame);
        timer->start(33);

        // --- Таймер отправки команд ---
        commandTimer = new QTimer(this);
        connect(commandTimer, &QTimer::timeout, [this]() {
            if (command_sock != -1) sendCommand(currentCommand);
        });
        commandTimer->start(50);
    }

    ~ControllerWindow() override {
        video_ready = false;
        if (cap.isOpened()) cap.release();
    }

    QTextEdit* logs() const { return log_widget; }

protected:
    void keyPressEvent(QKeyEvent* event) override { /* оставляем как есть */ }
    void keyReleaseEvent(QKeyEvent* /*event*/) override { /* оставляем как есть */ }

private:
    QLabel* video_label = nullptr;
    QTextEdit* log_widget = nullptr;
    QTimer* timer = nullptr;
    QTimer* commandTimer = nullptr;

    cv::VideoCapture cap;
    std::atomic<bool> video_ready{false};
    char currentCommand = 's';

    void updateFrame() {
        if (!video_ready.load()) return;
        if (!cap.isOpened()) return;

        cv::Mat frame;
        cap >> frame;
        if (frame.empty()) return;

        QImage img(frame.data, frame.cols, frame.rows, frame.step, QImage::Format_BGR888);
        video_label->setPixmap(QPixmap::fromImage(img).scaled(video_label->size(),
                                                              Qt::KeepAspectRatio,
                                                              Qt::SmoothTransformation));
    }

    void sendCommand(char cmd) { /* оставляем как есть */ }
};


int main(int argc, char* argv[]) {
    signal(SIGINT, [](int){ running = false; running_logs = false; });

    command_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (command_sock < 0) { perror("command socket"); return -1; }

    QApplication app(argc, argv);

    ControllerWindow window;
    window.show();

    std::thread heartbeatThread(send_heartbeat);
    std::thread logThread(receive_logs, window.logs());

    int ret = app.exec();

    running = false;
    running_logs = false;

    if (heartbeatThread.joinable()) heartbeatThread.join();
    if (logThread.joinable()) logThread.join();

    close(command_sock);
    return ret;
}

#include "operator.moc"