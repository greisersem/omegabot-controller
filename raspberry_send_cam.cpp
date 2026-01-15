#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <thread>
#include <vector>
#include <opencv2/opencv.hpp>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <csignal>

#pragma comment(lib, "ws2_32.lib")      // Подключение библиотеки Winsock

// #define SERVER_IP "192.168.110.232"     // IP адрес Raspberry
#define SERVER_IP "192.168.1.46"
#define SERVER_PORT     12345           // Порт для передачи команд
#define VIDEO_PORT      12346           // Порт для приёма видеопотока
#define LOGS_PORT       12347           // Порт для приёма логов
#define HEARTBEAT_PORT  12348           // Порт для отслеживания соединения

std::atomic<bool> running_logs(true);
std::atomic<bool> running(true);
std::atomic<bool> new_log_file(true);
std::atomic<bool> do_not_stop(false);

void signal_handler(int signum)
{
    std::cout << "Signal " << signum << " received, stopping program..." << std::endl;
    running = false;
    running_logs = false;
}

void send_heartbeat()
{
    SOCKET heartbeat_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (heartbeat_sock == INVALID_SOCKET) {
        std::cerr << "Error creating heartbeat socket." << std::endl;
        return;
    }

    sockaddr_in heartbeat_addr;
    heartbeat_addr.sin_family = AF_INET;
    heartbeat_addr.sin_port = htons(HEARTBEAT_PORT);
    inet_pton(AF_INET, SERVER_IP, &heartbeat_addr.sin_addr);

    while (running) {
        const char* heartbeat_msg = "1";
        int sent = sendto(heartbeat_sock, heartbeat_msg, strlen(heartbeat_msg), 0, (sockaddr*)&heartbeat_addr, sizeof(heartbeat_addr));
        if (sent < 0) {
            std::cerr << "Failed to send heartbeat: " << strerror(errno) << std::endl;
        } /* else {
            std::cout << "Heartbeat sent." << std::endl;
        } */
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    closesocket(heartbeat_sock);
}

void receive_video_stream()
{
    std::string gst_pipeline =
        "udpsrc port=" + std::to_string(VIDEO_PORT) + " ! "
        "application/x-rtp, encoding-name=H264 ! "
        "rtph264depay ! "
        "h264parse ! avdec_h264 ! videoconvert ! appsink";

    cv::VideoCapture cap(gst_pipeline, cv::CAP_GSTREAMER);
    if (!cap.isOpened()) {
        std::cerr << "Error opening video stream with GStreamer" << std::endl;
        return;
    }

    cv::Mat frame, rotated_frame;
    while (running) {
        cap >> frame;

        if (frame.empty()) {
            std::cerr << "Empty frame." << std::endl;
            break;
        }

        cv::rotate(frame, rotated_frame, cv::ROTATE_180);
        cv::imshow("Video Stream", rotated_frame);  
        if (cv::waitKey(1) == 27) {  
            break;
        }
    }

    cap.release();
    cv::destroyAllWindows();
}

void receive_logs()
{
    SOCKET log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (log_sock == INVALID_SOCKET) {
        std::cerr << "Error creating socket for logs." << std::endl;
        return;
    }

    u_long mode = 1; // 1 to enable non-blocking mode
    if (ioctlsocket(log_sock, FIONBIO, &mode) != 0) {
        std::cerr << "Failed to set socket to non-blocking mode. Error: " << WSAGetLastError() << std::endl;
        closesocket(log_sock);
        return;
    }

    sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(LOGS_PORT);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(log_sock, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        std::cerr << "Error binding log socket." << std::endl;
        closesocket(log_sock);
        return;
    }

    int logging_flag;

    if (new_log_file) logging_flag = std::ios::trunc;
    else logging_flag = std::ios::app;

    std::ofstream log_file("logs.txt", logging_flag);
    if (!log_file.is_open()) {
        std::cerr << "Failed to open log file." << std::endl;
        closesocket(log_sock);
        return;
    }

    std::cout << "Log receiver started. Saving logs to 'logs.txt'." << std::endl;

    char buffer[256];
    sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);

    while (running_logs && running) {  // Проверяем флаг завершения
        int received = recvfrom(log_sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&client_addr, &client_addr_len);
        if (received > 0) {
            buffer[received] = '\0';
            std::cout << "Log: " << buffer << std::endl;
            log_file << buffer << std::endl;
            log_file.flush();
        }
    }

    log_file.close();
    closesocket(log_sock);
}

std::string trim(const std::string& str)
{
    auto start = str.begin();
    while (start != str.end() && std::isspace(*start)) {
        ++start;
    }

    auto end = str.end();
    do {
        --end;
    } while (std::distance(start, end) > 0 && std::isspace(*end));

    return std::string(start, end + 1);
}

void clean_logs() {
    std::ifstream input_file("logs.txt");
    if (!input_file.is_open()) {
        std::cerr << "Failed to open logs.txt for reading." << std::endl;
        return;
    }

    std::vector<std::string> lines;
    std::string line;

    // Читаем строки, удаляем пробелы и пропускаем пустые строки
    /* while (std::getline(input_file, line)) {
        std::string trimmed_line = trim(line);
        if (!trimmed_line.empty()) {
            lines.push_back(trimmed_line);
        }
    } */

    while (std::getline(input_file, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    input_file.close();

    // Обрабатываем строки, объединяя те, где отсутствует символ "%"
    std::vector<std::string> cleaned_lines;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string current_line = lines[i];

        // Пока строка не заканчивается на "%" и есть следующая строка
        while (!current_line.empty() && current_line.back() != '%' && i + 1 < lines.size()) {
            current_line += " " + lines[++i]; // Объединяем с следующей строкой
        }

        current_line.pop_back();

        cleaned_lines.push_back(current_line);
    }

    // Перезаписываем файл
    std::ofstream output_file("logs.txt", std::ios::trunc);
    if (!output_file.is_open()) {
        std::cerr << "Failed to open logs.txt for writing." << std::endl;
        return;
    }

    for (const auto& valid_line : cleaned_lines) {
        output_file << valid_line << std::endl;
    }

    output_file.close();
    std::cout << "Logs cleaned successfully." << std::endl;
}

int main()
{
    // Устанавливаем обработчик сигнала
    signal(SIGINT, signal_handler);

    // Инициализация Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "Winsock initialization error." << std::endl;
        return -1;
    }

    // Создание сокета для отправки команд
    SOCKET command_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (command_sock == INVALID_SOCKET) {
        std::cerr << "Error creating socket for commands." << std::endl;
        WSACleanup();
        return -1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    // Инициализация SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Robot Controller", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 600, SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    std::thread videoThread(receive_video_stream);
    std::thread logThread(receive_logs);
    std::thread heartbeatThread(send_heartbeat);

    SDL_Event e;

    while (running) {
        while (SDL_PollEvent(&e) != 0) {  // Обрабатываем события SDL
            new_log_file = false;
            if (e.type == SDL_QUIT) {  // Закрытие окна
                running = false;
                running_logs = false;
            } else if (e.type == SDL_KEYDOWN) {  // Нажатие клавиши
                char command = 0;

                switch (e.key.keysym.sym) {
                    case SDLK_w:        command = '1'; break;         // Вперёд
                    case SDLK_s:        command = '2'; break;         // Назад
                    case SDLK_d:        command = '3'; break;         // Вправо
                    case SDLK_a:        command = '4'; break;         // Влево
                    case SDLK_y:        command = 'y';
                                        do_not_stop = true; break;    // Обследовать зону без связи
                    case SDLK_o:        command = 'o';
                                        do_not_stop = true; break;    // Разворот на месте
                    case SDLK_SPACE:    command = 's'; break;         // Стоп
                    case SDLK_1:        command = 'f'; 
                                        do_not_stop = true; break;    // До препятствия
                    case SDLK_UP:       command = '5'; break;         // Вверх
                    case SDLK_DOWN:     command = '6'; break;         // Вниз
                    case SDLK_LEFT:     command = '7'; break;         // Поворот влево
                    case SDLK_RIGHT:    command = '8'; break;         // Поворот вправо
                    case SDLK_ESCAPE:   running = false;
                                        running_logs = false; break;  // Завершение программы
                    default: break;
                }

                if (command != 0) {
                    sendto(command_sock, &command, sizeof(command), 0, (sockaddr*)&server_addr, sizeof(server_addr));
                }
            } else if (e.type == SDL_KEYUP) {
                if (do_not_stop) {
                    do_not_stop = false;
                    continue;
                }
                char command = 's';
                sendto(command_sock, &command, sizeof(command), 0, (sockaddr*)&server_addr, sizeof(server_addr));
            }

            if (!running) {
                std::cout << "Quiting process..." << std::endl;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        SDL_Delay(10);  // Небольшая задержка для снижения нагрузки на CPU
    }

    if (videoThread.joinable()) {
        videoThread.join();
        std::cout << "Video thread terminated." << std::endl;
    }
    if (logThread.joinable()) {
        logThread.join();
        std::cout << "Log thread terminated." << std::endl;
    }
    if (heartbeatThread.joinable()) {
        heartbeatThread.join();
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    closesocket(command_sock);
    WSACleanup();

    clean_logs();

    return 0;
}
