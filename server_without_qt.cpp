#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// --- Настройки (Обновлены до 115200 и 5000) ---
#define TCP_PORT            12345          // Использовать порт 5000
#define SERIAL_PORT         "/dev/ttyUSB0"
#define BAUD_RATE           B115200       // Использовать скорость 115200
#define MAX_CLIENTS         1
#define BUFFER_SIZE         256
#define LOG_FILENAME_PREFIX "arduino_"
// ------------------

int serial_fd = -1;
FILE *log_file = NULL;

// Вспомогательная функция: получение временной метки
void get_timestamp(char *buf, size_t size) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

// Настройка последовательного порта
int setup_serial() {
    // Используем O_RDWR для чтения и записи
    serial_fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY | O_NDELAY);
    if (serial_fd < 0) {
        fprintf(stderr, "Ошибка: не удалось открыть %s: %s\n", SERIAL_PORT, strerror(errno));
        return -1;
    }

    struct termios tty;
    if (tcgetattr(serial_fd, &tty) != 0) {
        perror("tcgetattr");
        close(serial_fd);
        return -1;
    }

    cfmakeraw(&tty);
    cfsetospeed(&tty, BAUD_RATE);
    cfsetispeed(&tty, BAUD_RATE);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(serial_fd);
        return -1;
    }

    // Добавляем задержку и очистку, чтобы исключить стартовый мусор от Arduino
    usleep(250000); // Ждем 250 мс для перезагрузки Arduino
    tcflush(serial_fd, TCIOFLUSH); // Очищаем входные и выходные буферы

    printf("Serial порт %s открыт успешно на скорости %d.\n", SERIAL_PORT, (int)BAUD_RATE);
    return 0;
}

// Открытие лог-файла
int open_log_file() {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char filename[256];
    snprintf(filename, sizeof(filename), "%s%04d-%02d-%02d_%02d-%02d-%02d.log",
        LOG_FILENAME_PREFIX,
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);

    log_file = fopen(filename, "w");
    if (!log_file) {
        perror("fopen log");
        return -1;
    }

    char ts[64];
    get_timestamp(ts, sizeof(ts));
    fprintf(log_file, "[LOG START] %s\n", ts);
    fflush(log_file);
    printf("Лог записывается в: %s\n", filename);
    return 0;
}

// Отправка данных в serial
void send_to_serial(const char *data, size_t len) {
    if (serial_fd >= 0) {
        ssize_t written = write(serial_fd, data, len);
        if (written != (ssize_t)len) {
            fprintf(stderr, "Предупреждение: отправлено %zd из %zu байт в serial\n", written, len);
        }
    }
}

// Обработка данных от Arduino
void handle_serial_input() {
    static char serial_buffer[BUFFER_SIZE];
    static int buf_pos = 0;

    char temp[64];
    ssize_t n = read(serial_fd, temp, sizeof(temp) - 1);
    if (n <= 0) return;

    for (ssize_t i = 0; i < n; i++) {
        serial_buffer[buf_pos++] = temp[i];
        if (temp[i] == '\n' || buf_pos >= BUFFER_SIZE - 1) {
            serial_buffer[buf_pos] = '\0';
            char *line = serial_buffer;
            
            // Удаляем \n
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            
            // Удаляем \r
            char *cr = strchr(line, '\r');
            if (cr) *cr = '\0';

            if (strlen(line) > 0) {
                // Запись в лог
                if (log_file) {
                    char ts[64];
                    get_timestamp(ts, sizeof(ts));
                    fprintf(log_file, "[%s] %s\n", ts, line);
                    fflush(log_file);
                }
                // Вывод в консоль
                if (strncmp(line, "HB:", 3) == 0) {
                    printf("[ARDUINO HEARTBEAT] %s\n", line);
                } else {
                    printf("[ARDUINO STATUS] %s\n", line);
                }
            }
            buf_pos = 0;
        }
    }
}

int main() {
    // 1. Открыть serial
    if (setup_serial() != 0) return 1;

    // 2. Открыть лог
    if (open_log_file() != 0) {
        fprintf(stderr, "Предупреждение: лог не будет записываться.\n");
    }

    // 3. Создать TCP-сервер
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(TCP_PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("TCP-сервер запущен на порту %d\n", TCP_PORT);

    // Массив клиентских сокетов
    int clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = -1;

    struct pollfd fds[MAX_CLIENTS + 2]; // +2: сервер и serial
    int nfds = 0;

    while (1) {
        nfds = 0;

        // Серверный сокет — слушаем новые подключения
        fds[nfds].fd = server_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        // Serial порт — читаем данные от Arduino
        if (serial_fd >= 0) {
            fds[nfds].fd = serial_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        // Существующие клиенты
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] >= 0) {
                fds[nfds].fd = clients[i];
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }

        int ret = poll(fds, nfds, 100); // таймаут 100 мс
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        // 1. Новое подключение
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                perror("accept");
            } else {
                // Найти свободный слот
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i] == -1) {
                        slot = i;
                        break;
                    }
                }
                if (slot == -1) {
                    close(client_fd);
                    printf("Слишком много клиентов, отклонено.\n");
                } else {
                    clients[slot] = client_fd;
                    printf("Новое подключение: %s:%d\n",
                        inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                }
            }
        }

        // 2. Данные от Arduino
        int serial_idx = (serial_fd >= 0) ? 1 : -1;
        if (serial_idx != -1 && (fds[serial_idx].revents & POLLIN)) {
            handle_serial_input();
        }

        // 3. Данные от клиентов или отключение
        int client_index_start = (serial_fd >= 0) ? 2 : 1; 
        int current_poll_index = client_index_start;
        
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] < 0) continue;

            // Находим соответствие в pollfds
            if (current_poll_index < nfds && fds[current_poll_index].fd == clients[i]) {
                if (fds[current_poll_index].revents & POLLIN) {
                    char buffer[BUFFER_SIZE];
                    ssize_t n = recv(clients[i], buffer, sizeof(buffer) - 1, 0);
                    if (n <= 0) {
                        // Клиент отключился
                        printf("Клиент отключён\n");
                        send_to_serial("P\n", 2); // отправить команду остановки
                        close(clients[i]);
                        clients[i] = -1;
                    } else {
                        buffer[n] = '\0';
                        
                        // Команды от TCP клиента могут содержать \r\n,
                        // поэтому используем strtok для извлечения первой чистой команды.
                        char *cmd = strtok(buffer, "\r\n");
                        while (cmd != NULL) {
                            if (strlen(cmd) > 0) {
                                // Отправить команду на Arduino с \n
                                char full_cmd[BUFFER_SIZE];
                                snprintf(full_cmd, sizeof(full_cmd), "%s\n", cmd);
                                send_to_serial(full_cmd, strlen(full_cmd));
                                printf("-> Команда от клиента: '%s'\n", cmd);
                            }
                            // Переход к следующей команде, если она была отправлена в том же пакете
                            cmd = strtok(NULL, "\r\n");
                        }
                    }
                }
                current_poll_index++;
            }
        }
    }

    // Завершение
    if (log_file) {
        char ts[64];
        get_timestamp(ts, sizeof(ts));
        fprintf(log_file, "[LOG END] %s\n", ts);
        fclose(log_file);
    }
    if (serial_fd >= 0) close(serial_fd);
    close(server_fd);

    return 0;
}
