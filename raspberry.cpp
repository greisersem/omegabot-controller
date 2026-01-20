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

#define UART_DEVICE "/dev/ttyACM0"   // UART устройство
#define SERVER_IP   "192.168.0.104"  // IP адрес ноутбука дома
// #define SERVER_IP "192.168.31.34"    // IP вдрес ноутбука в аудитории

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
    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);

    auto last_heartbeat = std::chrono::steady_clock::now();
    bool was_connection_lost = false;

    std::cout << "Entering heartbeat loop..." << std::endl;

    while (heartbeat_running) {
        int received = recvfrom(heartbeat_sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&client_addr, &client_addr_len);
        if (received > 0) {
            buffer[received] = '\0';
            if (buffer[0] == '1') {
                last_heartbeat = std::chrono::steady_clock::now();
                if (was_connection_lost) {
                    std::cout << "Connection restored." << std::endl;
                    was_connection_lost = false;
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
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count();

        if (elapsed >= CRITICAL_TIMEOUT.count()) {
            if (!was_connection_lost) {
                std::cout << "Connection lost detected." << std::endl;
                was_connection_lost = true;
                connection_lost = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Exiting heartbeat loop and closing socket." << std::endl;
    close(heartbeat_sock);
}


void video_stream_sender() {
    gst_init(nullptr, nullptr);

    std::string pipeline_str = 
        "v4l2src device=/dev/video0 ! "
        "image/jpeg, width=640, height=480, "
        "framerate=30/1 ! jpegparse ! avdec_mjpeg ! "
        "videoconvert ! x264enc tune=zerolatency bitrate=1000 "
        "speed-preset=ultrafast ! "
        "rtph264pay config-interval=1 pt=96 !" 
        "udpsink host=" + std::string(SERVER_IP) + " port=" + std::to_string(VIDEO_PORT);
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipeline_str, &error);

    if (!pipeline) {
        std::cerr << "Error creating pipeline with GStreamer: " << (error ? error->message : "неизвестная ошибка") << std::endl;
        if (error) g_error_free(error);
        return;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Error opening pipline with GStreamer!" << std::endl;
        gst_object_unref(pipeline);
        return;
    }

    std::cout << "Video stream is sending. Press Ctrl+C to exit." << std::endl;
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
}


void send_logs(int uart, const std::string& server_ip) {
    int log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (log_sock < 0) {
        std::cerr << "Error binding log socket." << std::endl;
        return;
    }

    sockaddr_in log_addr;
    log_addr.sin_family = AF_INET;
    log_addr.sin_port = htons(LOGS_PORT);
    inet_pton(AF_INET, server_ip.c_str(), &log_addr.sin_addr);

    while (logs_running) {
        char uart_buffer[256] = {0};
        int bytes_read = serRead(uart, uart_buffer, sizeof(uart_buffer) - 1);
        if (bytes_read > 0) {
            uart_buffer[bytes_read] = '\0';
            sendto(log_sock, uart_buffer, strlen(uart_buffer), 0, (sockaddr*)&log_addr, sizeof(log_addr));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    close(log_sock);
}

int main() {
    if (gpioInitialise() < 0) {
        std::cerr << "Pigpio init error." << std::endl;
        return -1;
    }

    int uart = serOpen(UART_DEVICE, 9600, 0);
    if (uart < 0) {
        std::cerr << "UART opening error." << std::endl;
        gpioTerminate();
        return -1;
    }

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

    std::thread logThread(send_logs, uart, SERVER_IP);
    std::thread videoThread(video_stream_sender);
    std::thread heartbeatThread(monitor_heartbeat);

    bool e_sent = false;

    while (true) {
        if (connection_lost) {
            if (!e_sent) {
                std::cout << "Connection is lost" << std::endl;
                serWriteByte(uart, 'e');
                e_sent = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (connection_restored) {
            connection_restored = false;
            connection_lost = false;
            e_sent = false;              
            continue;
        }

        char buffer[1];
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int received = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&client_addr, &addr_len);
        if (received > 0) {
            std::cout << "Received command: " << buffer[0] << std::endl;
            serWriteByte(uart, buffer[0]);

            CRITICAL_TIMEOUT = std::chrono::seconds(15);
        }
    }

    logs_running = false;
    heartbeat_running = false;
    heartbeatThread.join();
    logThread.join();
    videoThread.join();

    close(sock);
    serClose(uart);
    gpioTerminate();
    return 0;
}
