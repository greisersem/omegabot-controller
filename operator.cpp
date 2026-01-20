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

#define SERVER_IP       "192.168.0.103"  // IP raspberry 
// #define SERVER_IP       "192.168.31.34"  // IP in class
#define SERVER_PORT     12345
#define VIDEO_PORT      12346
#define LOGS_PORT       12347
#define HEARTBEAT_PORT  12348

std::atomic<bool> running(true);
std::atomic<bool> running_logs(true);
std::atomic<bool> do_not_stop(false);

int command_sock = -1;

void send_heartbeat() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HEARTBEAT_PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    while (running) {
        const char* msg = "1";
        sendto(sock, msg, std::strlen(msg), 0, (sockaddr*)&addr, sizeof(addr));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    close(sock);
}

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

class controller_window : public QWidget {
    Q_OBJECT
public:
    explicit controller_window(QWidget* parent = nullptr)
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

        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &controller_window::update_frame);
        timer->start(33);

        start_video_open_thread();
    }

    ~controller_window() override {
        video_ready = false;
        if (video_open_thread.joinable()) video_open_thread.join();
    }

    QTextEdit* logs() const { return log_widget; }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        char command = 0;
        switch (event->key()) {
            case Qt::Key_W: command = '1'; break;
            case Qt::Key_S: command = '2'; break;
            case Qt::Key_D: command = '3'; break;
            case Qt::Key_A: command = '4'; break;
            case Qt::Key_E: command = '5'; break;
        }
        if (command && command_sock != -1) send_command(command);
    }

    void keyReleaseEvent(QKeyEvent* /*event*/) override {
        if (do_not_stop) { do_not_stop = false; return; }
        if (command_sock != -1) send_command('s');
    }

private:
    QLabel* video_label = nullptr;
    QTextEdit* log_widget = nullptr;
    QTimer* timer = nullptr;

    cv::VideoCapture cap;
    std::atomic<bool> video_ready{false};
    std::thread video_open_thread;

    void start_video_open_thread() {
        const std::string gst_pipeline =
            "udpsrc port=12346 caps=application/x-rtp,media=video,encoding-name=H264,payload=96 "
            "! rtph264depay "
            "! avdec_h264 "
            "! videoconvert "
            "! appsink sync=false";

        video_open_thread = std::thread([this, gst_pipeline]() {
            bool ok = cap.open(gst_pipeline, cv::CAP_GSTREAMER);

            video_ready = ok;

            QMetaObject::invokeMethod(
                this,
                [this, ok]() {
                    if (!ok) {
                        log_widget->append("Error opening video stream! (cap.open blocked/failed)");
                        video_label->setText("Video not available");
                    } else {
                        log_widget->append("Video stream opened");
                        video_label->setText("");
                    }
                },
                Qt::QueuedConnection
            );
        });
    }

    void update_frame() {
        if (!video_ready.load()) return;
        if (!cap.isOpened()) return;

        cv::Mat frame;
        cap >> frame;
        if (frame.empty()) return;

        QImage img(frame.data, frame.cols, frame.rows, frame.step, QImage::Format_BGR888);

        video_label->setPixmap(
            QPixmap::fromImage(img).scaled(
                video_label->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation
            )
        );
    }

    void send_command(char cmd) {
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &server.sin_addr);
        sendto(command_sock, &cmd, 1, 0, (sockaddr*)&server, sizeof(server));
    }
};

int main(int argc, char* argv[]) {
    signal(SIGINT, [](int){ running = false; running_logs = false; });

    command_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (command_sock < 0) { perror("command socket"); return -1; }

    QApplication app(argc, argv);

    controller_window window;
    window.show();

    std::thread heartbeat_thread(send_heartbeat);
    std::thread log_thread(receive_logs, window.logs());

    int ret = app.exec();

    running = false;
    running_logs = false;

    if (heartbeat_thread.joinable()) heartbeat_thread.join();
    if (log_thread.joinable()) log_thread.join();

    close(command_sock);
    return ret;
}

#include "operator.moc"
