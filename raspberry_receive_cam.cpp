#include <iostream>
#include <thread>
#include <cstring>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pigpio.h>
#include <gst/gst.h>

#define SERVER_PORT     12345  // Порт для приёма команд
#define VIDEO_PORT      12346  // Порт для отправки видеопотока
#define LOGS_PORT       12347  // Порт для отправки логов
#define HEARTBEAT_PORT  12348  // Порт для отслеживания соединения

const char* UART_DEVICE = "/dev/ttyACM0";  // UART устройство
const char* SERVER_IP = "192.168.221.235";  // IP адрес ноутбука
//const char* SERVER_IP = "192.168.100.120";  // IP WSL
// const char* SERVER_IP = "192.168.1.55";

volatile bool logs_running = true;
volatile bool heartbeat_running = true;
volatile bool connection_lost = false;
volatile bool connection_restored = false;

auto CRITICAL_TIMEOUT = std::chrono::seconds(120);

void monitor_heartbeat()
{
    int heartbeat_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (heartbeat_sock < 0) {
        std::cerr << "Error creating heartbeat socket." << std::endl;
        return;
    }

    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(HEARTBEAT_PORT);
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(heartbeat_sock, (sockaddr*)&localAddr, sizeof(localAddr)) < 0) {
        std::cerr << "Error binding heartbeat socket." << std::endl;
        close(heartbeat_sock);
        return;
    }

    // Set the socket to non-blocking mode
    int flags = fcntl(heartbeat_sock, F_GETFL, 0);

    if (flags < 0) {
        std::cerr << "Error getting socket flags." << std::endl;
        close(heartbeat_sock);
        return;
    }

    if (fcntl(heartbeat_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "Error setting socket to non-blocking mode." << std::endl;
        close(heartbeat_sock);
        return;
    }

    char buffer[2] = {0};
    sockaddr_in clientAddr{};
    socklen_t clientAddrLen = sizeof(clientAddr);

    auto lastHeartbeat = std::chrono::steady_clock::now();
    bool wasConnectionLost = false;

    std::cout << "Entering heartbeat loop..." << std::endl;

    while (heartbeat_running) {
        int received = recvfrom(heartbeat_sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&clientAddr, &clientAddrLen);
        if (received > 0) {
            buffer[received] = '\0';
            if (buffer[0] == '1') {
                // std::cout << "Heartbeat received." << std::endl;
                lastHeartbeat = std::chrono::steady_clock::now();
                if (wasConnectionLost) {
                    std::cout << "Connection restored." << std::endl;
                    wasConnectionLost = false;
                    connection_lost = false;
                    connection_restored = true;
                }
            } else {
                std::cerr << "Invalid heartbeat message: " << buffer << std::endl;
            }
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "Error receiving heartbeat: " << strerror(errno) << std::endl;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastHeartbeat).count();

        if (elapsed >= CRITICAL_TIMEOUT.count()) {
            if (!wasConnectionLost) {
                std::cout << "Connection lost detected." << std::endl;
                wasConnectionLost = true;
                connection_lost = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Exiting heartbeat loop and closing socket." << std::endl;
    close(heartbeat_sock);
}

void videoStreamSender() {
    // Инициализация GStreamer
    gst_init(nullptr, nullptr);

    // Создание элемента пайплайна GStreamer
    std::string pipelineStr =
    "libcamerasrc ! "
    "video/x-raw, width=320, height=320, framerate=10/1 ! "
    "videoconvert ! queue ! "
    "x264enc tune=zerolatency bitrate=350 speed-preset=superfast ! "
    "rtph264pay ! "
    "udpsink host=" + std::string(SERVER_IP) + " port=" + std::to_string(VIDEO_PORT);

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipelineStr.c_str(), &error);

    if (!pipeline) {
        std::cerr << "Error creating pipeline with GStreamer: " << (error ? error->message : "неизвестная ошибка") << std::endl;
        if (error) g_error_free(error);
        return;
    }

    // Запуск пайплайна
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Error opening pipline with GStreamer!" << std::endl;
        gst_object_unref(pipeline);
        return;
    }

    // Ожидание завершения (или можно сделать основной цикл)
    std::cout << "Video stream is sending. Press Ctrl+C to exit." << std::endl;
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

    // Остановка пайплайна
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
}

void sendLogs(int uart, const char* server_ip) {
    int log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (log_sock < 0) {
        std::cerr << "Error binding log socket." << std::endl;
        return;
    }

    sockaddr_in logAddr;
    logAddr.sin_family = AF_INET;
    logAddr.sin_port = htons(LOGS_PORT);
    inet_pton(AF_INET, server_ip, &logAddr.sin_addr);

    while (logs_running) {
        char uart_buffer[256] = {0};
        int bytesRead = serRead(uart, uart_buffer, sizeof(uart_buffer) - 1); // Чтение строки от Arduino
        if (bytesRead > 0) {
            uart_buffer[bytesRead] = '\0'; // Добавляем терминальный символ
            sendto(log_sock, uart_buffer, strlen(uart_buffer), 0, (sockaddr*)&logAddr, sizeof(logAddr));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Задержка для снижения нагрузки (100 мс)
    }

    close(log_sock);
}

int main() {
    // Инициализация pigpio
    if (gpioInitialise() < 0) {
        std::cerr << "Pigpio init error." << std::endl;
        return -1;
    }

    // Открытие UART
    int uart = serOpen((char*)UART_DEVICE, 9600, 0);
    if (uart < 0) {
        std::cerr << "UART opening error." << std::endl;
        gpioTerminate();
        return -1;
    }

    // Настройка сокета для приёма данных по UDP
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        std::cerr << "Error creating socket." << std::endl;
        serClose(uart);
        gpioTerminate();
        return -1;

    }

    sockaddr_in localAddr;
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(SERVER_PORT);
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (sockaddr*)&localAddr, sizeof(localAddr)) < 0) {
        std::cerr << "Error binding socket." << std::endl;
        close(sock);
        serClose(uart);
        gpioTerminate();
        return -1;
    }

    // Устанавливаем сокет в неблокирующий режим
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "Error getting socket flags." << std::endl;
        close(sock);
        gpioTerminate();
        return -1;
    }

    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "Error setting socket to non-blocking mode." << std::endl;
        close(sock);
        gpioTerminate();
        return -1;
    }

    // Запуск потоков
    std::thread logThread(sendLogs, uart, SERVER_IP);               // Поток для отправки логов
    std::thread videoThread(videoStreamSender);                     // Поток для видеопотока
    std::thread heartbeatThread(monitor_heartbeat);                 // Поток для мониторинга сердцебиения

    // Основной цикл: приём команд и управление UART
    while (true) {
        if (connection_lost) {
            std::cout << "Connection is lost" << std::endl;
            serWriteByte(uart, 'e');
            std::this_thread::sleep_for(std::chrono::seconds(1)); // Исключаем спам
            continue;
        }

        if (connection_restored) {
            serWriteByte(uart, 's');
            connection_restored = false;
            continue;
        }

        char buffer[1];
        sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);

        int received = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&clientAddr, &addrLen);
        if (received > 0) {
            std::cout << "Received command: " << buffer[0] << std::endl;
            serWriteByte(uart, buffer[0]);

            CRITICAL_TIMEOUT = std::chrono::seconds(15);
        }
    }

    // Ожидание завершения потоков
    logs_running = false;
    heartbeat_running = false;
    heartbeatThread.join();
    logThread.join();
    videoThread.join();

    // Завершение работы
    close(sock);
    serClose(uart);
    gpioTerminate();
    return 0;
}
