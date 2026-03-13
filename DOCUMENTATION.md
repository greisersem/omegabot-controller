# OmegaBot Controller — Документация проекта

## Оглавление

1. [Обзор проекта](#обзор-проекта)
2. [Архитектура системы](#архитектура-системы)
3. [Схема сетевого взаимодействия](#схема-сетевого-взаимодействия)
4. [Структура файлов](#структура-файлов)
5. [Клиент — client.cpp](#клиент--clientcpp)
   - [GUI и окно управления](#gui-и-окно-управления)
   - [Управление с клавиатуры](#управление-с-клавиатуры)
   - [Видеопоток и запись](#видеопоток-и-запись)
   - [YOLO-детекция объектов](#yolo-детекция-объектов)
   - [Heartbeat (контроль соединения)](#heartbeat-контроль-соединения)
   - [Система логирования на клиенте](#система-логирования-на-клиенте)
   - [Отправка команд](#отправка-команд)
6. [Сервер (Raspberry Pi) — server.cpp](#сервер-raspberry-pi--servercpp)
   - [Приём команд и UART-мост](#приём-команд-и-uart-мост)
   - [Видеострим с камеры](#видеострим-с-камеры)
   - [Ретрансляция логов](#ретрансляция-логов)
   - [Мониторинг heartbeat](#мониторинг-heartbeat)
   - [Обработка потери соединения](#обработка-потери-соединения)
7. [Arduino — arduino.cpp](#arduino--arduinocpp)
   - [Класс Driver](#класс-driver)
   - [Управление моторами](#управление-моторами)
   - [Команды движения](#команды-движения)
   - [Поворот на заданный угол](#поворот-на-заданный-угол)
   - [Датчик препятствий (HC-SR04)](#датчик-препятствий-hc-sr04)
   - [Датчик температуры и влажности (DHT11)](#датчик-температуры-и-влажности-dht11)
   - [Режим инспекции](#режим-инспекции)
   - [Потеря соединения](#потеря-соединения)
   - [Прерывание действий](#прерывание-действий)
   - [Главный цикл](#главный-цикл)
8. [YOLO-детекция (Python) — yolo_detection.py](#yolo-детекция-python--yolo_detectionpy)
9. [Сборка и запуск](#сборка-и-запуск)
10. [Порты и сетевые настройки](#порты-и-сетевые-настройки)
11. [Аппаратное подключение Arduino](#аппаратное-подключение-arduino)

---

## Обзор проекта

**OmegaBot Controller** - система дистанционного управления колёсным роботом OmegaBot. Робот оснащён Raspberry Pi и Arduino, связанными по UART. Оператор управляет роботом с ноутбука через Wi-Fi-сеть, получая видеопоток с камеры и логи датчиков в реальном времени.

Система реализует:

- Управление движением робота с клавиатуры (вперёд/назад/влево/вправо/повороты)
- Прямую трансляцию видео с камеры робота
- Детекцию объектов на видеопотоке (YOLOv8)
- Автоматическую запись видео в файл
- Сбор и отображение логов от Arduino (датчики, состояние)
- Обнаружение препятствий ультразвуковым датчиком с блокировкой движения вперёд
- Режим автономной инспекции территории
- Heartbeat-механизм для отслеживания обрыва связи с безопасной остановкой робота

---

## Архитектура системы

```
┌──────────────┐        Wi-Fi (UDP)        ┌──────────────┐     UART 115200     ┌──────────────┐
│              │  ─── команды (12345) ───>  │              │  ── команды (1B) ─> │              │
│    Клиент    │  <── видео   (12346) ───── │  Raspberry   │  <── логи Serial ── │   Arduino    │
│  (ноутбук)   │  <── логи    (12347) ───── │     Pi       │                     │              │
│              │  ─── heartbeat(12348) ───> │  (сервер)    │                     │  (моторы,    │
│  client.cpp  │                            │  server.cpp  │                     │   датчики)   │
└──────────────┘                            └──────────────┘                     └──────────────┘
```

**Поток данных:**

1. **Команды**: Клиент -> (UDP:12345) -> Raspberry Pi -> (UART) -> Arduino
2. **Видео**: Камера на Raspberry Pi -> (GStreamer, UDP:12346) -> Клиент
3. **Логи**: Arduino -> (Serial/UART) -> Raspberry Pi -> (UDP:12347) -> Клиент
4. **Heartbeat**: Клиент -> (UDP:12348) -> Raspberry Pi (каждые 2 сек)

---

## Схема сетевого взаимодействия

| Канал | Протокол | Порт | Направление | Описание |
|-------|----------|------|-------------|----------|
| Команды | UDP | 12345 | Клиент → Сервер | Однобайтовые команды управления |
| Видео | UDP (RTP/H264) | 12346 | Сервер → Клиент | H.264 видеопоток 640x480@30fps |
| Логи | UDP | 12347 | Сервер → Клиент | Текстовые сообщения от Arduino |
| Heartbeat | UDP | 12348 | Клиент → Сервер | Пакет `"1"` каждые 2 секунды |

Все каналы работают по **UDP** для минимальной задержки. Видеопоток передаётся через **GStreamer** с кодеком **H.264** в формате **RTP**.

---

## Структура файлов

| Файл | Где запускается | Назначение |
|------|----------------|------------|
| `client.cpp` | Ноутбук оператора | GUI-клиент: управление, видео, логи, YOLO |
| `server.cpp` | Raspberry Pi (на роботе) | Сервер: мост команд, видеострим, логи, heartbeat |
| `arduino.cpp` | Arduino (на роботе) | Прошивка: моторы, датчики, логика поведения |
| `yolo_detection.py` | Ноутбук оператора (альтернатива) | Python-скрипт YOLO-детекции через ultralytics |
| `CMakeLists.txt` | Ноутбук оператора | Сборка клиента (CMake + Qt5 + OpenCV) |

---

## Клиент — `client.cpp`

Клиент — это десктопное Qt5-приложение с графическим интерфейсом. Отвечает за управление роботом, отображение видео, получение логов и обнаружение объектов.

### GUI и окно управления

Класс `ControllerWindow` наследует `QWidget` и формирует окно 800x600:

```cpp
class ControllerWindow : public QWidget {
    Q_OBJECT
    // ...
    QLabel* video_label;       // отображение видеокадра 640x480
    QTextEdit* log_widget;     // текстовое поле для логов (только чтение)
    QTimer* video_timer;       // таймер обновления кадра (33 мс ≈ 30 fps)
    QTimer* command_timer;     // таймер отправки команд (20 мс ≈ 50 раз/сек)
};
```

Окно состоит из двух элементов, расположенных вертикально (`QVBoxLayout`):
- **Видео** (`QLabel`, 640x480) — отображение кадров с камеры робота
- **Логи** (`QTextEdit`, только чтение) — сообщения от Arduino в реальном времени

### Управление с клавиатуры

Команды делятся на **непрерывные** (отправляются пока клавиша зажата) и **одиночные** (отправляются один раз при нажатии):

**Непрерывные команды** — `command_timer` отправляет текущую команду каждые 20 мс:

| Клавиша | Команда (char) | Действие |
|---------|---------------|----------|
| `W` | `'w'` | Движение вперёд |
| `S` | `'s'` | Стоп (остановка моторов) |
| `A` | `'a'` | Поворот влево (на месте) |
| `D` | `'d'` | Поворот вправо (на месте) |
| `X` | `'x'` | Движение назад |

При отпускании клавиш W/S/A/D `current_command` сбрасывается в `0` и команды перестают отправляться — робот останавливается.

**Одиночные команды** — отправляются однократно при `keyPressEvent`:

| Клавиша | Команда (char) | Действие |
|---------|---------------|----------|
| `E` | `'e'` | Поворот на 180° по часовой |
| `Q` | `'q'` | Поворот на 180° против часовой |
| `C` | `'c'` | Запуск режима инспекции |
| `F` | `'f'` | Запрос данных с датчиков |

Ключевой код обработки нажатий:

```cpp
void keyPressEvent(QKeyEvent* event) override {
    if (event->isAutoRepeat()) return;   // игнорируем автоповторы ОС
    switch (event->key()) {
        case Qt::Key_W: current_command = 'w'; break;  // непрерывная
        case Qt::Key_E: send_command('e'); break;       // одиночная
        // ...
    }
}

void keyReleaseEvent(QKeyEvent* event) override {
    if (event->isAutoRepeat()) return;
    switch (event->key()) {
        case Qt::Key_W:
        case Qt::Key_S:
        case Qt::Key_A:
        case Qt::Key_D:
            current_command = 0;  // прекращаем отправку
            break;
    }
}
```

### Видеопоток и запись

Видеопоток принимается через **GStreamer** по UDP в отдельном потоке:

```cpp
std::string gst_pipeline =
    "udpsrc port=12346 caps=application/x-rtp,media=video,"
    "encoding-name=H264,payload=96 ! "
    "rtph264depay ! h264parse ! avdec_h264 ! "
    "videoconvert ! appsink sync=false max-buffers=2 drop=true";
```

Параметры пайплайна:
- `udpsrc port=12346` — приём UDP-пакетов с видео
- `rtph264depay` → `h264parse` → `avdec_h264` — декодирование RTP/H.264
- `appsink sync=false max-buffers=2 drop=true` — буфер на 2 кадра, старые отбрасываются (минимизация задержки)

**Автоматическая запись** — при получении первого кадра создаётся файл `video_YYYY-MM-DD_HH-MM-SS.avi` (кодек MJPG, 30 fps). Каждый кадр записывается параллельно с отображением:

```cpp
if (recording && video_writer.isOpened())
    video_writer.write(frame);
```

### YOLO-детекция объектов

На каждом кадре запускается **YOLOv8n** через OpenCV DNN:

```cpp
void init_yolo() {
    yolo_net = cv::dnn::readNet("yolov8n.onnx");
    yolo_net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);  // GPU-ускорение
    yolo_net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
}
```

Процесс детекции в `update_frame()`:
1. Кадр преобразуется в blob 640x640 с нормализацией `1/255`
2. Сеть выполняет forward pass
3. Результаты фильтруются по порогу уверенности `0.4`
4. Применяется **NMS** (Non-Maximum Suppression) с IoU-порогом `0.5`
5. Bounding box'ы рисуются зелёными прямоугольниками на кадре

Модель использует **CUDA** для GPU-ускорения. В коде объявлен полный массив из 80 классов COCO, но на рамках текущей реализации отображается метка `"person"`.

### Heartbeat (контроль соединения)

Клиент отправляет серверу пакет `"1"` каждые **2 секунды** по UDP на порт 12348:

```cpp
void send_heartbeat() {
    while (running) {
        const char* msg = "1";
        sendto(sock, msg, strlen(msg), 0, (sockaddr*)&addr, sizeof(addr));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
```

Если сервер не получает heartbeat в течение **30 секунд**, он считает соединение потерянным и отправляет Arduino команду аварийной остановки.

### Система логирования на клиенте

Логи приходят от сервера по UDP (порт 12347) и обрабатываются двумя способами:

1. **Отображение в GUI** — безопасная передача в UI-поток через `QMetaObject::invokeMethod`:

```cpp
QMetaObject::invokeMethod(
    log_widget,
    [log_widget, msg]() { log_widget->append(msg); },
    Qt::QueuedConnection
);
```

2. **Запись в файл** — файл `logs_YYYY-MM-DD_HH-MM-SS.txt` с метками времени:

```
[2026-03-13 14:32:01] Arduino started
[2026-03-13 14:32:05] Obstacle detected! Forward blocked
```

### Отправка команд

Команды отправляются как **один байт** по UDP:

```cpp
void send_command(char cmd) {
    sendto(command_sock, &cmd, 1, 0, (sockaddr*)&server, sizeof(server));
}
```

`command_timer` (интервал 20 мс) обеспечивает непрерывную отправку при зажатой клавише:

```cpp
command_timer->start(20);
// callback:
char cmd = current_command.load();
if (cmd != 0 && command_sock != -1) {
    send_command(cmd);
}
```

---

## Сервер (Raspberry Pi) — `server.cpp`

Сервер работает на Raspberry Pi, установленном на роботе. Выступает **мостом** между клиентом (Wi-Fi/UDP) и Arduino (UART). Параллельно стримит видео с камеры.

### Приём команд и UART-мост

Основной цикл принимает однобайтовые UDP-команды и пробрасывает их в Arduino через UART:

```cpp
int received = recvfrom(sock, buffer, sizeof(buffer), 0,
                        (sockaddr*)&client_addr, &addr_len);
if (received > 0) {
    std::cout << "Received command: " << buffer[0] << std::endl;
    serWriteByte(uart, buffer[0]);  // pigpio UART write
}
```

- Сокет работает в **неблокирующем режиме** (`O_NONBLOCK`)
- UART открывается через **pigpio** на устройстве `/dev/ttyUSB0` со скоростью **115200 бод**

### Видеострим с камеры

Видео с USB-камеры `/dev/video0` стримится через **GStreamer** в отдельном потоке:

```cpp
std::string pipeline_str = 
    "v4l2src device=/dev/video0 ! "
    "image/jpeg, width=640, height=480, framerate=30/1 ! "
    "jpegparse ! avdec_mjpeg ! videoconvert ! "
    "x264enc tune=zerolatency bitrate=2000 speed-preset=ultrafast ! "
    "rtph264pay config-interval=1 pt=96 ! "
    "udpsink host=<CLIENT_IP> port=12346";
```

Параметры:
- Захват: MJPEG 640x480 @ 30fps
- Кодирование: H.264 (`x264enc`), режим `zerolatency`, битрейт 2000 кбит/с
- `speed-preset=ultrafast` — минимальная задержка кодирования
- Доставка: RTP over UDP

### Ретрансляция логов

Отдельный поток читает данные из UART (сообщения от Arduino) и пересылает их клиенту по UDP:

```cpp
void send_logs(int uart, const std::string& server_ip) {
    while (logs_running) {
        char uart_buffer[256] = {0};
        int bytes_read = serRead(uart, uart_buffer, sizeof(uart_buffer) - 1);
        if (bytes_read > 0) {
            uart_buffer[bytes_read] = '\0';
            sendto(log_sock, uart_buffer, strlen(uart_buffer), 0,
                   (sockaddr*)&log_addr, sizeof(log_addr));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

Arduino пишет логи через `Serial.println()`, Raspberry Pi читает их из UART и отправляет клиенту.

### Мониторинг heartbeat

Сервер слушает heartbeat-пакеты от клиента на порту 12348:

```cpp
void monitor_heartbeat() {
    // ...
    auto last_heartbeat = std::chrono::steady_clock::now();
    while (heartbeat_running) {
        int received = recvfrom(heartbeat_sock, buffer, ...);
        if (received > 0 && buffer[0] == '1') {
            last_heartbeat = std::chrono::steady_clock::now();
            // если связь восстановлена — отмечаем
        }
        // проверяем таймаут
        if (elapsed >= CRITICAL_TIMEOUT.count()) {
            connection_lost = true;
        }
    }
}
```

Таймаут: **30 секунд** (`CRITICAL_TIMEOUT`). Если за это время ни одного heartbeat не пришло — соединение считается потерянным.

### Обработка потери соединения

При `connection_lost == true` основной цикл прекращает передачу команд клиента и вместо этого отправляет Arduino команду **`'o'`** (аварийная остановка):

```cpp
if (connection_lost) {
    if (!e_sent) {
        serWriteByte(uart, 'o');  // команда аварийного отъезда назад
        e_sent = true;
    }
    continue;  // игнорируем команды клиента
}
```

При восстановлении связи (`connection_restored`) нормальная работа возобновляется.

### Потоки на сервере

Сервер запускает **3 фоновых потока** + основной цикл:

| Поток | Функция | Описание |
|-------|---------|----------|
| Основной | `main()` | Приём команд UDP → UART |
| Логи | `send_logs()` | Чтение UART → отправка UDP |
| Видео | `video_stream_sender()` | Захват камеры → GStreamer → UDP |
| Heartbeat | `monitor_heartbeat()` | Контроль связи с клиентом |

---

## Arduino — `arduino.cpp`

Прошивка Arduino отвечает за непосредственное управление моторами и датчиками робота. Получает команды от Raspberry Pi по UART (Serial, 115200 бод).

### Класс Driver

Центральный класс, инкапсулирующий всю логику управления:

```cpp
class Driver {
public:
    void set_motors(int velo_left, int velo_right);   // установка скорости моторов
    void connection_lost_case();                        // обработка потери связи
    void inspection();                                  // режим инспекции
    void turn_on_degree(int degree);                    // поворот на угол

    char read_command();                                // чтение команды из UART
    void get_command_wheels(char command);               // обработка команд движения
    void get_command_other(char command);                // обработка прочих команд

    long int get_distance();                            // ультразвуковой датчик
    double get_temperature();                           // температура (DHT11)
    double get_humidity();                              // влажность (DHT11)

    void check_obstacle();                              // проверка препятствия
    void check_flags();                                 // проверка флагов состояния
    void interrupt_actions();                           // прерывание автономных действий
};
```

Глобальный экземпляр создаётся при старте:

```cpp
Driver Wheels(MOTOR_LEFT, MOTOR_RIGHT, MOTOR_DIR_LEFT,
              MOTOR_DIR_RIGHT, SPEED, TRIG_PIN, ECHO_PIN, ROTATION_TIME);
```

### Управление моторами

Робот имеет два мотора (левый и правый) с управлением через PWM + направление:

```cpp
void Driver::set_motors(const int velo_left, const int velo_right) {
    int motor_dir_left  = (velo_left  >= 0) ? HIGH : LOW;
    int motor_dir_right = (velo_right >= 0) ? HIGH : LOW;
    digitalWrite(pin_motor_dir_left, motor_dir_left);
    digitalWrite(pin_motor_dir_right, motor_dir_right);
    analogWrite(pin_motor_left,  constrain(abs(velo_left),  0, 255));
    analogWrite(pin_motor_right, constrain(abs(velo_right), 0, 255));
}
```

- Положительное значение — вперёд, отрицательное — назад
- Скорость ограничена диапазоном 0–255 (PWM)
- Базовая скорость: `SPEED = 150`

### Команды движения

| Команда | Левый мотор | Правый мотор | Результат |
|---------|------------|-------------|-----------|
| `'w'` | +speed | +speed | Вперёд |
| `'x'` | -speed | -speed | Назад |
| `'s'` | 0 | 0 | Стоп |
| `'d'` | +speed | -speed | Поворот вправо |
| `'a'` | -speed | +speed | Поворот влево |
| `'q'` | поворот | поворот | 180° против часовой |
| `'e'` | поворот | поворот | 180° по часовой |

Команда `'w'` блокируется при обнаруженном препятствии:

```cpp
case 'w':
    if (obstacle) {
        set_motors(0, 0);
        Serial.println("Forward blocked by obstacle");
    } else {
        set_motors(speed, speed);
    }
    break;
```

### Поворот на заданный угол

Неблокирующий поворот — вызывается многократно из `loop()`, пока не завершится:

```cpp
void Driver::turn_on_degree(const int degree) {
    if (!rotating) {
        rotation_start_time = millis();
        current_rotation_duration = (ROTATION_TIME / 360.0) * abs(degree);
        // включаем моторы в противоположных направлениях
        rotating = true;
        return;
    }
    if (millis() - rotation_start_time >= current_rotation_duration) {
        set_motors(0, 0);
        rotating = false;
    }
}
```

- `ROTATION_TIME = 2000` мс — время полного оборота (360°)
- Длительность поворота пропорциональна углу: `(2000 / 360) * |degree|` мс
- Работает асинхронно — не блокирует основной `loop()`

### Датчик препятствий (HC-SR04)

Ультразвуковой датчик измеряет расстояние до ближайшего объекта:

```cpp
long Driver::get_distance() {
    digitalWrite(pin_trig, LOW);
    delayMicroseconds(2);
    digitalWrite(pin_trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(pin_trig, LOW);
    long duration = pulseIn(pin_echo, HIGH, 30000);
    if (duration == 0) return -1;
    return duration * 0.034 / 2;  // перевод в сантиметры
}
```

Проверка выполняется в `check_obstacle()` **каждую итерацию** `loop()`:
- Критическая дистанция: **10 см** (`CRITICAL_DISTANCE`)
- При обнаружении препятствия: флаг `obstacle = true`, движение вперёд блокируется
- Логирование: при появлении, каждые 2 секунды пока присутствует, при исчезновении
- При появлении препятствия во время движения вперёд — мгновенная остановка

### Датчик температуры и влажности (DHT11)

Считывание по команде `'f'`:

```cpp
case 'f':
    double temp = get_temperature();
    double hum = get_humidity();
    long dist = get_distance();
    Serial.print("Sensors -> Temp: ");
    Serial.print(temp);
    Serial.print(" C, Hum: ");
    Serial.print(hum);
    Serial.print(" %, Dist: ");
    Serial.print(dist);
    Serial.println(" cm");
    break;
```

Результат передаётся через Serial → Raspberry Pi → клиент (в логи).

### Режим инспекции

Автономный режим обследования территории, запускается командой `'c'`. Реализован как **конечный автомат** с 4 состояниями:

```
Состояние 0: Движение вперёд (2 сек или до препятствия)
     │
     ▼
Состояние 1: Пауза (100 мс), запуск поворота на 180°
     │
     ▼
Состояние 2: Ожидание завершения поворота
     │
     ▼
Состояние 3: Движение назад (столько же, сколько ехали вперёд)
     │
     ▼
   Конец инспекции
```

Ключевые особенности:
- Если при движении вперёд встречается препятствие — робот останавливается раньше, запоминает пройденное время
- Время движения назад равно фактическому времени движения вперёд (робот возвращается в исходную точку)
- Инспекция выполняется **неблокирующе** — основной `loop()` продолжает работать

### Потеря соединения

При получении команды `'o'` от Raspberry Pi (heartbeat-таймаут):

```cpp
void Driver::connection_lost_case() {
    if (connection) {
        Serial.println("Connection lost. Moving backward");
        connection = false;
        disconnect_start_time = millis();
        set_motors(-speed, -speed);  // откат назад
        return;
    }
    if (millis() - disconnect_start_time >= connection_lost_duration) {
        set_motors(0, 0);  // остановка через 2 секунды
        connection = true;
    }
}
```

Робот **отъезжает назад** на протяжении 2 секунд (`DISCONNECTION_DURATION`), затем останавливается. Это мера безопасности — робот отходит от потенциально опасной зоны.

### Прерывание действий

Любая команда движения (W/A/S/D/X/Q/E) прерывает текущие автономные действия:

```cpp
void Driver::interrupt_actions() {
    Serial.println("Action interrupted");
    rotating = false;
    inspecting = false;
    connection = true;
    inspection_state = 0;
    set_motors(0, 0);
}
```

Это позволяет оператору в любой момент перехватить управление у автономных режимов.

### Главный цикл

```cpp
void loop() {    
    Wheels.check_obstacle();     // проверка датчика расстояния
    Wheels.check_flags();        // продолжение автономных действий (поворот, инспекция, дисконнект)
    
    char command = Wheels.read_command();
    if (command != '0') {
        Wheels.get_command_wheels(command);  // команды движения
        Wheels.get_command_other(command);   // команды датчиков/инспекции
    } else {
        // нет команд и нет активных действий → остановка
        if (!Wheels.rotating && !Wheels.inspecting && Wheels.connection) {
            Wheels.set_motors(0, 0);
        }
    }
    delay(10);  // ~100 итераций/сек
}
```

Порядок выполнения на каждой итерации:
1. Проверить препятствие
2. Продолжить автономные действия (если запущены)
3. Прочитать команду из UART
4. Выполнить команду или остановиться

---

## YOLO-детекция (Python) — `yolo_detection.py`

Альтернативный скрипт для YOLO-детекции объектов на Python с использованием библиотеки **ultralytics**:

```python
model = YOLO("yolov8n.pt")

while True:
    ret, frame = cap.read()
    results = model(frame)[0]
    annotated_frame = results.plot()
    writer.write(annotated_frame)
```

- Принимает видеопоток с порта 12346 через GStreamer
- Обрабатывает кадры моделью YOLOv8n
- Ретранслирует обработанные кадры на `127.0.0.1:12349` (для локального просмотра)
- Может использоваться вместо встроенной YOLO-детекции в клиенте

---

## Сборка и запуск

### Клиент (ноутбук)

**Зависимости:**
- Qt5 (Widgets, Core, Gui)
- OpenCV 4 (с поддержкой CUDA и GStreamer)
- GStreamer
- CMake >= 3.16
- Компилятор C++17
- Модель `yolov8n.onnx` в директории проекта

**Сборка:**

```bash
mkdir build && cd build
cmake ..
make
```

**Запуск:**

```bash
./operator
```

### Сервер (Raspberry Pi)

**Зависимости:**
- pigpio (для UART)
- GStreamer

**Компиляция:**

```bash
g++ -o server server.cpp -lpigpio -lpthread $(pkg-config --cflags --libs gstreamer-1.0)
```

**Запуск:**

```bash
sudo ./server   # sudo нужен для pigpio
```

### Arduino

Загрузить `arduino.cpp` через Arduino IDE или PlatformIO. Требуемые библиотеки:
- `Servo.h`
- `DHT.h` (Adafruit DHT sensor library)

---

## Порты и сетевые настройки

```cpp
// Определены одинаково на клиенте и сервере:
#define SERVER_PORT     12345   // команды
#define VIDEO_PORT      12346   // видеопоток
#define LOGS_PORT       12347   // логи
#define HEARTBEAT_PORT  12348   // heartbeat
```

IP-адреса настраиваются в define'ах каждого файла:
- `client.cpp` — `SERVER_IP` = IP Raspberry Pi
- `server.cpp` — `SERVER_IP` = IP ноутбука клиента (для отправки видео и логов)

---

## Аппаратное подключение Arduino

### Моторы

| Пин | Назначение |
|-----|-----------|
| 6 | PWM левого мотора (`MOTOR_LEFT`) |
| 5 | PWM правого мотора (`MOTOR_RIGHT`) |
| 7 | Направление левого мотора (`MOTOR_DIR_LEFT`) |
| 4 | Направление правого мотора (`MOTOR_DIR_RIGHT`) |

### Ультразвуковой датчик HC-SR04

| Пин | Назначение |
|-----|-----------|
| 10 | TRIG (выход, триггер) |
| 11 | ECHO (вход, эхо) |

### Датчик DHT11

| Пин | Назначение |
|-----|-----------|
| 9 | DATA (данные DHT11) |

### UART (связь с Raspberry Pi)

| Соединение | Описание |
|-----------|----------|
| Arduino TX → Raspberry Pi RX | Логи от Arduino |
| Raspberry Pi TX → Arduino RX | Команды к Arduino |
| Скорость: 115200 бод | Устройство на Pi: `/dev/ttyUSB0` |
