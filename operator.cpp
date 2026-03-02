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
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <mutex>


#define SERVER_IP       "192.168.0.105"  // IP raspberry 
// #define SERVER_IP       "192.168.31.172"  // IP in class
#define SERVER_PORT     12345
#define VIDEO_PORT      12346
#define LOGS_PORT       12347
#define HEARTBEAT_PORT  12348

std::atomic<bool> running(true);
std::atomic<bool> running_logs(true);
std::ofstream log_file;
std::mutex log_mutex;

int command_sock = -1;

std::vector<std::string> classNames = {
        "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
        "fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
        "elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
        "skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle",
        "wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange",
        "broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed",
        "dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven",
        "toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"
    };

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


void write_log_to_file(const std::string& text) {
    if (!log_file.is_open())
        return;

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm_ptr = std::localtime(&t);

    log_file << "[" << std::put_time(tm_ptr, "%Y-%m-%d %H:%M:%S") << "] "
             << text << std::endl;
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
        int n = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                        (sockaddr*)&client, &client_len);

        if (n > 0) {
            buffer[n] = '\0';

            std::string msg_str(buffer);
            QString msg = QString::fromUtf8(buffer);

            write_log_to_file(msg_str);

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

        video_timer = new QTimer(this);
        connect(video_timer, &QTimer::timeout,
                this, &ControllerWindow::update_frame);
        video_timer->start(33);

        command_timer = new QTimer(this);
        connect(command_timer, &QTimer::timeout, this, [this]() {
            char cmd = current_command.load();
            if (cmd != 0 && command_sock != -1) {
                send_command(cmd);
            }
        });
        command_timer->start(20);

        start_video_open_thread();
        init_yolo();
    }

    ~ControllerWindow() override {
        video_ready = false;
        if (video_open_thread.joinable())
            video_open_thread.join();
        if (video_writer.isOpened())
            video_writer.release();
    }

    QTextEdit* logs() const { return log_widget; }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->isAutoRepeat())
            return;

        switch (event->key()) {
            case Qt::Key_W: current_command = 'w'; break;
            case Qt::Key_S: current_command = 's'; break;
            case Qt::Key_D: current_command = 'd'; break;
            case Qt::Key_A: current_command = 'a'; break;
            case Qt::Key_X: current_command = 'x'; break;

            case Qt::Key_E: send_command('e'); break;
            case Qt::Key_Q: send_command('q'); break;
            case Qt::Key_C: send_command('c'); break;
            case Qt::Key_F: send_command('f'); break;

            default: break;
        }
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (event->isAutoRepeat())
            return;

        switch (event->key()) {
            case Qt::Key_W:
            case Qt::Key_S:
            case Qt::Key_D:
            case Qt::Key_A:
                current_command = 0;
                break;
            default:
                break;
        }
    }

private:
    QLabel* video_label = nullptr;
    QTextEdit* log_widget = nullptr;

    QTimer* video_timer = nullptr;
    QTimer* command_timer = nullptr;

    cv::VideoCapture cap;
    std::atomic<bool> video_ready{false};
    std::thread video_open_thread;
    cv::VideoWriter video_writer;
    std::atomic<bool> recording{false};

    int frame_count = 0;
    cv::Mat last_annotated_frame;
    cv::dnn::Net yolo_net;

    std::atomic<char> current_command{0};

    void start_video_open_thread() {
        const std::string gst_pipeline =
            "udpsrc port=" + std::to_string(VIDEO_PORT) + " caps=application/x-rtp,media=video,encoding-name=H264,payload=96 ! "
            "rtph264depay ! "
            "h264parse ! "
            "avdec_h264 ! "
            "videoconvert ! "
            "appsink sync=false max-buffers=2 drop=true";

        video_open_thread = std::thread([this, gst_pipeline]() {
            bool ok = cap.open(gst_pipeline, cv::CAP_GSTREAMER);
            video_ready = ok;

            if (ok) {
                cv::Mat first_frame;
                cap >> first_frame;

                if (!first_frame.empty()) {

                    int width  = first_frame.cols;
                    int height = first_frame.rows;
                    double fps = 30.0;
                    std::cout << width << "x" << height << std::endl;

                    auto now = std::chrono::system_clock::now();
                    std::time_t t = std::chrono::system_clock::to_time_t(now);
                    std::tm* tm_ptr = std::localtime(&t);

                    std::ostringstream filename;
                    filename << std::getenv("HOME")
                            << "/Desktop/omegabot-controller/video_"
                            << std::put_time(tm_ptr, "%Y-%m-%d_%H-%M-%S")
                            << ".avi";

                    video_writer.open(
                        filename.str(),
                        cv::VideoWriter::fourcc('M','J','P','G'),
                        fps,
                        cv::Size(width, height)
                    );

                    if (video_writer.isOpened()) {
                        recording = true;
                        video_writer.write(first_frame);
                    }
                }
            }

            QMetaObject::invokeMethod(
                this,
                [this, ok]() {
                    if (!ok) {
                        video_label->setText("Video not available");
                    } else {
                        video_label->setText("");
                    }
                },
                Qt::QueuedConnection
            );
        });
    }

    void init_yolo()
    {
        yolo_net = cv::dnn::readNet("/home/greisersem/Desktop/omegabot-controller/yolov8n.onnx");
        yolo_net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
        yolo_net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
    }

   void update_frame() {
        if (!video_ready.load() || !cap.isOpened())
            return;

        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
            return;

        frame_count++;
        cv::Mat yolo_frame = frame.clone();

        cv::Mat blob = cv::dnn::blobFromImage(yolo_frame, 1.0 / 255.0, cv::Size(640, 640), cv::Scalar(), true);
        yolo_net.setInput(blob);
        
        std::vector<cv::Mat> out;
        yolo_net.forward(out);
        cv::Mat output = out[0];

        std::vector<cv::Rect> boxes;
        std::vector<float> confidences;
        float conf = 0.4;

        for (int i = 0; i < output.size[2]; i++) {
            if (output.at<float>(0, 4, i) < conf) {
                continue;
            } else {
                float cx = output.at<float>(0, 0, i);
                float cy = output.at<float>(0, 1, i);
                float w = output.at<float>(0, 2, i);
                float h = output.at<float>(0, 3, i);

                float x = cx - w / 2;
                float y = cy - h / 2;
                boxes.push_back(cv::Rect(x, y, w, h));
                confidences.push_back(output.at<float>(0, 4, i));
            }
        }

        std::vector<int> indices;
        cv::dnn::NMSBoxes(boxes, confidences, 0.4, 0.5, indices);

        for (int idx : indices) {
            cv::Rect box = boxes[idx];

            cv::rectangle(frame, box, cv::Scalar(0, 255, 0));
            cv::putText(
                frame,
                "person", 
                cv::Point(box.x, box.y),
                cv::FONT_HERSHEY_COMPLEX,
                0.5, 
                cv::Scalar(0, 0, 0),
                2
            );
        }
        
        QImage img(frame.data,
                frame.cols,
                frame.rows,
                frame.step,
                QImage::Format_BGR888);

        video_label->setPixmap(
            QPixmap::fromImage(img).scaled(
                video_label->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation
            )
        );

        if (recording && video_writer.isOpened())
            video_writer.write(frame);
    }


    void send_command(char cmd) {
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &server.sin_addr);

        sendto(command_sock,
               &cmd,
               1,
               0,
               (sockaddr*)&server,
               sizeof(server));
    }
};


int main(int argc, char* argv[]) {
    signal(SIGINT, [](int){ running = false; running_logs = false; });

    command_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (command_sock < 0) { perror("command socket"); return -1; }

    QApplication app(argc, argv);

    ControllerWindow window;
    window.show();

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm_ptr = std::localtime(&t);

    std::ostringstream filename;
    filename << std::getenv("HOME")
            << "/Desktop/omegabot-controller/logs_"
            << std::put_time(tm_ptr, "%Y-%m-%d_%H-%M-%S")
            << ".txt";

    log_file.open(filename.str());

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