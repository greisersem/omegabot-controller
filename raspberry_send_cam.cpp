#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QTimer>
#include <QMutex>

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

#define SERVER_IP       "192.168.1.46"
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
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    while (running) {
        const char* msg = "1";
        sendto(sock, msg, strlen(msg), 0, (sockaddr*)&addr, sizeof(addr));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    close(sock);
}

// --- Поток логов ---
void receive_logs(QTextEdit* log_widget) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(LOGS_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (sockaddr*)&local, sizeof(local));

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    char buffer[256];
    sockaddr_in client{};
    socklen_t client_len = sizeof(client);

    while (running_logs) {
        int n = recvfrom(sock, buffer, sizeof(buffer)-1, 0, (sockaddr*)&client, &client_len);
        if (n > 0) {
            buffer[n] = '\0';
            QString msg(buffer);
            QMetaObject::invokeMethod(log_widget, [log_widget, msg]() {
                log_widget->append(msg);
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    close(sock);
}

// --- Класс главного окна ---
class ControllerWindow : public QWidget {
    Q_OBJECT
public:
    ControllerWindow(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(800, 600);

        video_label = new QLabel(this);
        video_label->setFixedSize(640, 480);

        log_widget = new QTextEdit(this);
        log_widget->setReadOnly(true);

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->addWidget(video_label);
        layout->addWidget(log_widget);
        setLayout(layout);

        // Таймер обновления видео каждые 33 мс (~30 FPS)
        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &ControllerWindow::updateFrame);
        timer->start(33);

        // OpenCV VideoCapture через GStreamer
        std::string gst_pipeline =
            "udpsrc port=" + std::to_string(VIDEO_PORT) + " ! "
            "application/x-rtp, encoding-name=H264 ! "
            "rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! appsink";
        cap.open(gst_pipeline, cv::CAP_GSTREAMER);
        if(!cap.isOpened())
            log_widget->append("Error opening video stream!");
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        char command = 0;
        switch(event->key()) {
            case Qt::Key_W: command='1'; break;
            case Qt::Key_S: command='2'; break;
            case Qt::Key_D: command='3'; break;
            case Qt::Key_A: command='4'; break;
            case Qt::Key_Y: command='y'; do_not_stop=true; break;
            case Qt::Key_O: command='o'; do_not_stop=true; break;
            case Qt::Key_Space: command='s'; break;
            case Qt::Key_1: command='f'; do_not_stop=true; break;
            case Qt::Key_Up: command='5'; break;
            case Qt::Key_Down: command='6'; break;
            case Qt::Key_Left: command='7'; break;
            case Qt::Key_Right: command='8'; break;
            case Qt::Key_Escape: running=false; running_logs=false; break;
        }
        if(command && command_sock!=-1) sendCommand(command);
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if(do_not_stop) { do_not_stop=false; return; }
        char command='s';
        if(command_sock!=-1) sendCommand(command);
    }

private:
    QLabel* video_label;
    QTextEdit* log_widget;
    QTimer* timer;
    cv::VideoCapture cap;

    void updateFrame() {
        if(!cap.isOpened()) return;
        cv::Mat frame;
        cap >> frame;
        if(frame.empty()) return;
        cv::rotate(frame, frame, cv::ROTATE_180);
        QImage img(frame.data, frame.cols, frame.rows, frame.step, QImage::Format_RGB888);
        img = img.rgbSwapped();
        video_label->setPixmap(QPixmap::fromImage(img).scaled(video_label->size(), Qt::KeepAspectRatio));
    }

    void sendCommand(char cmd) {
        sockaddr_in server{};
        server.sin_family=AF_INET;
        server.sin_port=htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &server.sin_addr);
        sendto(command_sock, &cmd, 1, 0, (sockaddr*)&server, sizeof(server));
    }
};

int main(int argc, char* argv[]) {
    signal(SIGINT, [](int){ running=false; running_logs=false; });

    command_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(command_sock<0){ perror("command socket"); return -1; }

    QApplication app(argc, argv);
    ControllerWindow window;
    window.show();

    std::thread heartbeatThread(send_heartbeat);
    std::thread logThread(receive_logs, window.findChild<QTextEdit*>());

    int ret = app.exec();

    running=false;
    running_logs=false;
    if(heartbeatThread.joinable()) heartbeatThread.join();
    if(logThread.joinable()) logThread.join();
    close(command_sock);
    return ret;
}

#include "raspberry_send_cam.moc"
