#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QTcpSocket>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QImage>
#include <QPixmap>

#include <iostream>
#include <queue>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <atomic>
#include <thread>
#include <condition_variable>

// --- ЗАГОЛОВКИ OPENCV ---
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
// -------------------------

// --- НАСТРОЙКИ СЕТИ ---
const QHostAddress SERVER_ADDRESS = QHostAddress("172.20.10.2"); // localhost
const quint16 SERVER_PORT = 12345;
// ----------------------

bool flag_connect = false;

#define MIN_AREA 1000

cv::Mat getting_mask(cv::Mat* img)
 { 
    if (!img || img->empty()) return {};

    

    cv::Mat hsv, mask;

    cv::cvtColor(*img, hsv, cv::COLOR_BGR2HSV);
    //подставить значения перед проверкой
    cv::Scalar lower(0, 99, 152);
    cv::Scalar upper(100, 228, 255);
    cv::inRange(hsv, lower, upper, mask);
    return mask;

 }


std::vector<cv::Point> getting_largest_countours(cv::Mat* mask){
    std::vector<std::vector<cv::Point>> contours;

    cv::findContours(*mask, contours, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) return{};
    auto largest_contour = *std::max_element(
            contours.begin(),
            contours.end(),
            [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
                return cv::contourArea(a) < cv::contourArea(b);
            });
    if (cv::contourArea(largest_contour)  < MIN_AREA) return{};
    return largest_contour;
}


const cv::Size TARGET_SAVE_SIZE(320, 240); 

constexpr int MAX_BUFFER_FRAMES = 10; 

const double OUTPUT_FPS = 30.0; 

class VideoBufferLogger {
private:
    
    std::queue<cv::Mat> frame_buffer;
    std::mutex buffer_mutex;
    
    std::string save_directory;
    int video_counter;
    double fps;
    cv::Size frame_size;
    std::atomic<bool> running;
    std::thread worker_thread;

    cv::VideoWriter writer;
    
    
    void processBuffer() {
        
        int fourcc = cv::VideoWriter::fourcc('M','J','P','G'); 
        std::string current_filename = save_directory + "/live_log.avi"; 
        
        
        if (!writer.open(current_filename, fourcc, fps, frame_size, true)) {
            std::cerr << "[WORKER] Error: Could not open VideoWriter. Check codec support!" 
            << std::endl;
            running = false; 
            return;
        }
        
        std::cout << "[WORKER] VideoWriter opened for continuous logging to: " 
                  << current_filename << std::endl;

       
        while (running || !frame_buffer.empty()) {
            cv::Mat current_frame;
            bool frame_available = false;

            {
                
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (!frame_buffer.empty()) {
                    
                    current_frame = std::move(frame_buffer.front()); 
                    frame_buffer.pop();
                    frame_available = true;
                }
            } 
            if (frame_available) {
                
                writer.write(current_frame); 
            } else {
            
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        
      
        if (writer.isOpened()) {
            writer.release();
        }
        std::cout << "[WORKER] Recording finished and file closed." << std::endl;
    }
    
public:
    VideoBufferLogger(const std::string& dir = "./video_logs")
        : save_directory(dir), 
          video_counter(0),
          fps(OUTPUT_FPS),
          frame_size(TARGET_SAVE_SIZE), 
          running(true) 
    {   
        std::filesystem::create_directories(save_directory);
       
        worker_thread = std::thread(&VideoBufferLogger::processBuffer, this);
    }
    
    ~VideoBufferLogger() {
        stop();
    }
    
   
    void stop() {
        running = false;
        if (worker_thread.joinable()) {
            worker_thread.join(); 
        }
    }
    
   
    void addFrame(const cv::Mat& frame) {
        if (frame.empty()) return;

        cv::Mat frame_to_add;
       
        if (frame.size() != frame_size) {
           cv::resize(frame, frame_to_add, frame_size, 0, 0, cv::INTER_NEAREST); 
        } else {
            frame_to_add = frame;
        }

        {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            
            
            if (frame_buffer.size() >= MAX_BUFFER_FRAMES) {
                frame_buffer.pop(); 
            }
            
            
            frame_buffer.push(frame_to_add.clone()); 
        }
    }
    
    int getBufferSize() {
        std::lock_guard<std::mutex> lock(buffer_mutex);
        return frame_buffer.size();
    }
};


class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        // --- ИНИЦИАЛИЗАЦИЯ UI ---
        QWidget *centralWidget = new QWidget(this);
        setCentralWidget(centralWidget);
        QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);

        QWidget *leftWidget = new QWidget(centralWidget);
        QVBoxLayout *leftLayout = new QVBoxLayout(leftWidget);
        QWidget *rightWidget = new QWidget(centralWidget);
        QVBoxLayout *rightLayout = new QVBoxLayout(rightWidget);

        // Создаем элементы управления
        QPushButton *button = new QPushButton("Нажми меня!", rightWidget);
        logTextEdit = new QTextEdit(rightWidget);
        QSlider *slider = new QSlider(Qt::Horizontal, leftWidget);
        QLabel *label = new QLabel("150", leftWidget);
        connectionStatusLabel = new QLabel("Статус: Отключено", leftWidget);
        recordingStatusLabel = new QLabel("Режим: Обычный", leftWidget);

        // ★ НОВЫЙ ЭЛЕМЕНТ: Метка для отображения видео
        videoLabel = new QLabel("Ожидание видеопотока...", leftWidget);
        videoLabel->setMinimumSize(320, 240); // Задаем минимальный размер
        videoLabel->setAlignment(Qt::AlignCenter);
        videoLabel->setStyleSheet("QLabel { border: 1px solid gray; background-color: black; color: white; }");

        // Настройки окна вывода логов
        logTextEdit->setReadOnly(true);
        logTextEdit->setPlaceholderText("Логи будут отображаться здесь...");
        logTextEdit->setStyleSheet(
            "QTextEdit { background-color: #f0f0f0; border-radius: 5px; }");

        // Настраиваем слайдер
        slider->setRange(0, 255);
        slider->setValue(150);

        // Компоновка
        leftLayout->addWidget(connectionStatusLabel);
        leftLayout->addWidget(recordingStatusLabel);
        leftLayout->addWidget(slider);
        leftLayout->addWidget(label);
        leftLayout->addWidget(videoLabel); // Добавляем метку видео

        rightLayout->addWidget(button);
        rightLayout->addWidget(logTextEdit);

        mainLayout->addWidget(leftWidget);
        mainLayout->addWidget(rightWidget);

        mainLayout->setStretchFactor(leftWidget, 1);
        mainLayout->setStretchFactor(rightWidget, 2);

        // Подключаем сигналы UI
        connect(button, &QPushButton::clicked, this, &MainWindow::onButtonClicked);
        connect(slider, &QSlider::valueChanged,
                [label](int value) { label->setText(QString::number(value)); });
        connect(slider, &QSlider::valueChanged, this,
                &MainWindow::onSliderValueChanged);
        
        // --- ИНИЦИАЛИЗАЦИЯ И НАСТРОЙКА TCP ---
        socket = new QTcpSocket(this);

        // Подключение сигналов сокета
        connect(socket, &QTcpSocket::connected, this, &MainWindow::onConnected);
        connect(socket, &QTcpSocket::disconnected, this,
                &MainWindow::onDisconnected);
        connect(
            socket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
            this, &MainWindow::onErrorOccurred);

        // Таймер для автоматической попытки переподключения
        QTimer *reconnectTimer = new QTimer(this);
        connect(reconnectTimer, &QTimer::timeout, this,
                &MainWindow::connectToServer);
        reconnectTimer->start(1000); // Попытка каждые 1 секунду

        // Инициализация состояния записи
        isRecording = false;

        // Первая попытка подключения
        connectToServer();

        logMessage("Приложение запущено. Попытка подключения к серверу...");

        // === ИНИЦИАЛИЗАЦИЯ ЛОГ-ФАЙЛА ===
        QString logFileName =
            QDateTime::currentDateTime().toString("yyyy-MM-dd_hh-mm-ss") +
            "_client_log.txt";
        logFile.setFileName(logFileName);

        if (!logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logMessage("Ошибка: невозможно открыть лог-файл для записи!");
        } else {
            logMessage("Лог-файл открыт: " + logFileName);
        }

        // Настройка окна для отслеживания клавиш
        setFocusPolicy(Qt::StrongFocus);
        setFocus();

        setWindowTitle("Клиент: Отправка клавиш и Видеопоток");
        resize(1000, 600);
        
        // --- НАСТРОЙКА ВИДЕОПОТОКА OPENCV/GSTREAMER ---
        
        // Строка GStreamer пайплайна для cv::VideoCapture.
        // *Важно:* Для работы GStreamer в OpenCV необходимо, чтобы он был
        // скомпилирован с поддержкой GStreamer.
        const char* pipeline = 
            "udpsrc port=5000 ! application/x-rtp,encoding-name=JPEG,payload=26 ! rtpjpegdepay ! jpegdec ! videoconvert ! appsink";
            
        // Если вы используете старую версию OpenCV (до 4.5.1), 
        // может потребоваться явно указать бэкенд CAP_GSTREAMER:
        // videoCapture.open(pipeline, cv::CAP_GSTREAMER);
        videoCapture.open(pipeline); 

        if (!videoCapture.isOpened()) {
            logMessage("Ошибка: Не удалось открыть видеопоток GStreamer/UDP на порту 5000.");
        } else {
            logMessage("Видеопоток GStreamer/UDP успешно инициализирован.");
            
            // --- ИНИЦИАЛИЗАЦИЯ И ЗАПУСК ЛОГГЕРА ---
	    std::string log_dir = "/home/vova/Desktop/video_log"; 
	    
	    // Создаем логгер в куче (heap), чтобы он жил вместе с MainWindow
	    try {
		videoLogger = new VideoBufferLogger(log_dir);
		logMessage("Логгер видеопотока запущен в /live_log.avi");
	    } catch (const std::exception& e) {
		logMessage("Ошибка при инициализации логгера: ");
		// Можно продолжить без логирования, если это не критично
	    }

	    // Запускаем таймер для обновления кадров
	    videoTimer = new QTimer(this);
	    connect(videoTimer, &QTimer::timeout, this, &MainWindow::updateVideoFrame);
	    // Частота обновления, например, 30 кадров в секунду (33 мс)
	    videoTimer->start(33);
        }
    }
    
    ~MainWindow() {
        if (videoCapture.isOpened()) {
            videoCapture.release();
        }
    }

private:
    QTextEdit *logTextEdit;
    QTcpSocket *socket;
    QLabel *connectionStatusLabel;
    QSet<int> pressedKeys;
    QFile logFile;

    bool isRecording;
    QString recordedSequence;
    QLabel *recordingStatusLabel;

    // --- ПОЛЯ ДЛЯ ВИДЕО ---
    cv::VideoCapture videoCapture;
    QLabel *videoLabel;
    QTimer *videoTimer;
    // -----------------------
    
    VideoBufferLogger *videoLogger = nullptr;
    int frame_id = 0;
    const int LOG_EVERY_N = 3; // Логировать каждый N-ный кадр

private slots:
    void onButtonClicked() {
        QMessageBox::information(this, "Сообщение", "Кнопка нажата!");
        logMessage("Кнопка нажата!");
    }

    void logMessage(const QString &message) {
        QString full = QString("[%1] %2")
                           .arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
                           .arg(message);

        // Вывод в окно
        logTextEdit->append(full);

        // Скролл вниз
        QTextCursor cursor = logTextEdit->textCursor();
        cursor.movePosition(QTextCursor::End);
        logTextEdit->setTextCursor(cursor);

        // === Запись в файл ===
        if (logFile.isOpen()) {
            QTextStream out(&logFile);
            out << full << "\n";
            logFile.flush(); // на всякий случай, для стабильности
        }
    }

    void connectToServer() {
        if (socket->state() == QAbstractSocket::UnconnectedState) {
            if (flag_connect == true) {
                logMessage(QString("Попытка подключения к %1:%2...")
                               .arg(SERVER_ADDRESS.toString())
                               .arg(SERVER_PORT));
            }
            socket->connectToHost(SERVER_ADDRESS, SERVER_PORT);
        }
    }

    void onConnected() {
        connectionStatusLabel->setText(
            "Статус: <font color='green'>Подключено</font>");
        logMessage("Успешно подключено к серверу!");
        flag_connect = true;
    }

    void onDisconnected() {
        connectionStatusLabel->setText(
            "Статус: <font color='red'>Отключено</font>");
        logMessage("Отключено от сервера. Таймер запустил переподключение...");
    }

    void onErrorOccurred(QAbstractSocket::SocketError socketError) {
        Q_UNUSED(socketError);
        if (flag_connect == true) {
            if (socket->state() == QAbstractSocket::UnconnectedState) {
                connectionStatusLabel->setText(
                    "Статус: <font color='red'>Ошибка/Отключено</font>");
                logMessage(QString("Ошибка сокета: %1").arg(socket->errorString()));
            }
            flag_connect = false;
        }
    }

    void onSliderValueChanged(int value) {
        if (!isRecording && socket->state() == QAbstractSocket::ConnectedState) {
            QString dataToSend = QString("V%1").arg(value);
            qint64 bytesWritten = socket->write(dataToSend.toLatin1());
            if (bytesWritten <= 0) {
                logMessage(QString("Не удалось отправить значение слайдера: %1")
                               .arg(socket->errorString()));
            }
        }
    }
    
    // --- НОВЫЙ СЛОТ: Обновление кадра видео ---
    void updateVideoFrame() {
        cv::Mat frame;
    
        if (videoCapture.read(frame) && !frame.empty()) {
			if (videoLogger != nullptr) {
				if (frame_id % LOG_EVERY_N == 0) {
				    videoLogger->addFrame(frame);
				}
			}
			
			frame_id++;
            
            // Преобразование BGR (OpenCV) в RGB
            cv::cvtColor(frame, frame, cv::COLOR_BGR2RGB);
			
			cv::Mat  mask;
			bool metka = false;

			mask = getting_mask(&frame);
			
			std::vector<cv::Point> area =  getting_largest_countours(&mask);
			if (!area.empty()){
				cv::Rect bondRect = cv::boundingRect(area);
   				cv::rectangle(frame, bondRect, cv::Scalar(0, 255, 0), 5);
   			}
   			cv::imshow("mask", mask);

            // Создание QImage
            QImage image(frame.data, 
                         frame.cols, 
                         frame.rows, 
                         static_cast<int>(frame.step), // Использование frame.step для шага
                         QImage::Format_RGB888);

            // Преобразование в QPixmap и масштабирование под QLabel
            QPixmap pixmap = QPixmap::fromImage(image);
            
            // Масштабирование с сохранением пропорций
            videoLabel->setPixmap(pixmap.scaled(videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    // ------------------------------------------

    void keyPressEvent(QKeyEvent *event) override {
        // ... (остальная логика keyPressEvent без изменений)
        if (event->isAutoRepeat())
            return; // <── блокируем автоповтор

        int key = event->key();

        // если клавиша уже зажата — НИЧЕГО НЕ ДЕЛАЕМ
        if (pressedKeys.contains(key))
            return;
        // иначе — отмечаем, что она теперь зажата
        pressedKeys.insert(key);

        QString keyChar = "";
        QString logKeyText;
        bool isControlKey = false;

        switch (event->key()) {
        case Qt::Key_W:
        case Qt::Key_A:
        case Qt::Key_S:
        case Qt::Key_D:
            keyChar = QChar(key).toUpper();
            logKeyText = QString("%1 (%2)").arg(keyChar).arg(
                key == Qt::Key_W ? "Вперед" : (key == Qt::Key_A ? "Влево" : (key == Qt::Key_S ? "Назад" : "Вправо")));
            isControlKey = true;
            break;
        case Qt::Key_1:
        case Qt::Key_2:
        case Qt::Key_3:
            keyChar = QChar(key);
            logKeyText = keyChar;
            isControlKey = true;
            break;
        case Qt::Key_Escape:
            logKeyText = "ESC (Выход)";
            QApplication::quit();
            break;

        // ★ ОБРАБОТКА КЛАВИШИ '0' ДЛЯ УПРАВЛЕНИЯ ПАКЕТОМ
        case Qt::Key_0:
            if (isRecording) {
                // 1. ОСТАНОВКА ЗАПИСИ И ОТПРАВКА ПАКЕТА
                isRecording = false;
                recordingStatusLabel->setText("Режим: Обычный");
                logMessage("Запись остановлена. Попытка отправки пакета.");

                if (!recordedSequence.isEmpty()) {
                    if (socket->state() == QAbstractSocket::ConnectedState) {
                        // Отправка накопленной последовательности как одного пакета
                        qint64 bytesWritten = socket->write(recordedSequence.toLatin1());
                        if (bytesWritten > 0) {
                            logMessage(QString("ПАКЕТ ОТПРАВЛЕН (%1 байт): '%2'")
                                           .arg(bytesWritten)
                                           .arg(recordedSequence));
                        } else {
                            logMessage(QString("Ошибка отправки пакета: %1")
                                           .arg(socket->errorString()));
                        }
                    } else {
                        logMessage("Нет TCP-подключения. Пакет не отправлен.");
                    }
                    recordedSequence.clear();
                } else {
                    logMessage("Пакет пуст. Отправка не требуется.");
                }
            } else {
                // 2. НАЧАЛО ЗАПИСИ
                isRecording = true;
                recordedSequence.clear();
                recordingStatusLabel->setText("Режим: <font color='orange'>ЗАПИСЬ "
                                              "(Нажмите 0 для отправки)</font>");
                logMessage("Запись пакета начата. Нажмите W/A/S/D/P.");
            }
            break;

        default:
            logKeyText = QString("Key: %1").arg(event->text().isEmpty()
                                                    ? QString::number(event->key())
                                                    : event->text());
            break;
        }

        // ЛОГИКА ЗАПИСИ ИЛИ ОТПРАВКИ КОНТРОЛЬНЫХ КЛАВИШ
        if (isControlKey) {
            if (isRecording) {
                // РЕЖИМ 1: ЗАПИСЬ (Накопление пакета)
                recordedSequence.append(keyChar);
                logMessage(QString("Записана клавиша: %1. Последовательность: %2")
                               .arg(logKeyText)
                               .arg(recordedSequence));
            } else {
                // РЕЖИМ 2: ОБЫЧНЫЙ (Немедленная отправка)
                if (socket->state() == QAbstractSocket::ConnectedState) {
                    // Отправка одного байта данных
                    qint64 bytesWritten = socket->write(keyChar.toLatin1());
                    if (bytesWritten > 0) {
                        logMessage(QString("Отправлено по TCP: '%1'").arg(keyChar));
                    } else {
                        logMessage(QString("Не удалось отправить данные: %1")
                                       .arg(socket->errorString()));
                    }
                } else {
                    logMessage(QString("Клавиша %1 нажата, но нет TCP-подключения!")
                                   .arg(logKeyText));
                }
            }
        }

        QMainWindow::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent *event) override {
        // ... (логика keyReleaseEvent без изменений)
        if (event->isAutoRepeat())
            return;

        int key = event->key();

        if (pressedKeys.contains(key)) {
            pressedKeys.remove(key);

            // если это W/A/S/D — отправляем STOP
            if (!isRecording && (key == Qt::Key_W || key == Qt::Key_A ||
                                 key == Qt::Key_S || key == Qt::Key_D)) {
                if (socket->state() == QAbstractSocket::ConnectedState) {
                    socket->write("P");
                    logMessage("STOP (отпускание клавиши)");
                }
            }
        }

        QMainWindow::keyReleaseEvent(event);
    }
};

#include "client.moc"

int main(int argc, char *argv[]) {
    // Важно: в Linux и macOS, может понадобиться вручную 
    // инициализировать GStreamer, если OpenCV не делает это автоматически
    // cv::setLogLevel(cv::LogLevel::LOG_LEVEL_INFO); 
    
    QApplication app(argc, argv);

    MainWindow window;
    window.show();

    return app.exec();
}
