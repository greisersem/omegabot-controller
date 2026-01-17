#include <Arduino.h>
#include <Servo.h>
#include <DHT.h>
#include <math.h>

#define CRITICAL_DISTANCE       30

#define SENSOR_DIST_FRONT       A3
#define SENSOR_DIST_SIDE        A0
#define SENSOR_LIGHT            A1
#define AUDIO_PIN               11
#define LAMP_PIN                10

#define DHTPIN                  9
#define DHTTYPE                 DHT11

DHT dht_sensor(DHTPIN, DHTTYPE);

volatile bool LAMP_STATE   = false;
volatile bool STOP_COMMAND = false;


class Driver {
public:
    Driver(int left_pin, int right_pin, int left_dir_pin, int right_dir_pin, int servo_arm_pin, int servo_hand_pin, int motor_speed = 250); 
    ~Driver();
    
    void set_motors(const int velo_left, const int velo_right);
    void stop_motors();
    void drive_forward_until(const int dist);
    void connection_lost_case();
    void inspection();
    void turn_on_degree(const int degree);
    void autonomous_drive();  // Добавлено объявление

    void arm(const int power);
    void hand(const int power);

    char read_command();
    void clear_serial_buffer();
    void get_command_wheels(char command);  // Исправлена опечатка
    void get_command_arm(char command);     // Исправлена опечатка
    void get_command_other(char command);   // Исправлена опечатка

    long int get_distance(uint8_t sensor);  // Добавлен параметр
    double get_temperature();
    double get_humidity();
    
    void sound_signal(const int delay_time);
    
    void write_logs(char command, unsigned long time);

private:
    Servo servo_arm;
    Servo servo_hand;
    uint8_t pos_servo_arm;
    uint8_t pos_servo_hand;
    uint8_t pin_servo_arm;
    uint8_t pin_servo_hand;
    int pin_motor_left;
    int pin_motor_right;
    int pin_motor_dir_left;
    int pin_motor_dir_right;
    
private:
    char last_msg_hand;
    char previos_command;
    unsigned long previous_command_time = 0;
    int speed;
    int rotation_speed; 
    int rotation_time;          //milliseconds
};


Driver::Driver(int left_pin, int right_pin, int left_dir_pin, int right_dir_pin, int servo_arm_pin, int servo_hand_pin, int motor_speed) 
    : pin_motor_left(left_pin), pin_motor_right(right_pin), pin_motor_dir_left(left_dir_pin), pin_motor_dir_right(right_dir_pin), 
      speed(motor_speed), pin_servo_arm(servo_arm_pin), pin_servo_hand(servo_hand_pin),
      pos_servo_arm(0), pos_servo_hand(0), last_msg_hand('0'), previos_command('0'), rotation_time(1120)
{
    pinMode(pin_motor_left, OUTPUT);
    pinMode(pin_motor_right, OUTPUT);
    pinMode(pin_motor_dir_left, OUTPUT);
    pinMode(pin_motor_dir_right, OUTPUT);
    pinMode(SENSOR_DIST_FRONT, INPUT);
    pinMode(SENSOR_DIST_SIDE, INPUT);
    pinMode(SENSOR_LIGHT, INPUT);  // Добавлено
    pinMode(AUDIO_PIN, OUTPUT);
    pinMode(LAMP_PIN, OUTPUT);
    dht_sensor.begin();

    servo_arm.attach(pin_servo_arm);
    servo_hand.attach(pin_servo_hand);
    rotation_speed  = 0.5 * speed;

    set_motors(0, 0);
    servo_arm.write(0);
    servo_hand.write(150);
}


Driver::~Driver()
{
    set_motors(0, 0);
    servo_arm.write(0);
    servo_hand.write(150);
}


void Driver::clear_serial_buffer()
{
    while (Serial.available() > 0) {
        Serial.read();
    }
}


char Driver::read_command()
{
    char command = '0';
    auto current_time = millis();

    if (Serial.available() > 0) {
        command = Serial.read();

        if (command == '\n' || command == '\r') {
            return '0';
        }

        if (previos_command == command && (current_time - previous_command_time) <= 50) {
            clear_serial_buffer();
            return '0';
        }

        previos_command = command;
        previous_command_time = current_time;
    }

    return command;
}


void Driver::write_logs(char command, unsigned long time) 
{
    double temp = get_temperature();
    double hum  = get_humidity();
    int brightness = analogRead(SENSOR_LIGHT);
    long int dist_forw = get_distance(SENSOR_DIST_FRONT);  // Исправлен тип
    long int dist_side = get_distance(SENSOR_DIST_SIDE);   // Исправлен тип

    String log_msg;
    log_msg = "Time: " + String(time) + "; Command: " + String(command) + "; Dist. Forw.: " + String(dist_forw) 
            + "; Dist. Side: " + String(dist_side) + "; Temperature: " + String(temp, 2) + "; Humidity: "
            + String(hum, 2) + "; Brightness: " + String(brightness) + " %";

    Serial.println(log_msg);
}


void Driver::sound_signal(const int delay_time)
{
    int time = delay_time;
    if (delay_time > 1000) time = 1000;
    digitalWrite(AUDIO_PIN, HIGH);
    delay(time);
    digitalWrite(AUDIO_PIN, LOW);
    delay(time);
}


void Driver::set_motors(const int velo_left, const int velo_right)
{
    int motor_dir_left  = (velo_left  >= 0) ? HIGH : LOW;
    int motor_dir_right = (velo_right >= 0) ? HIGH : LOW;

    digitalWrite(pin_motor_dir_left,   motor_dir_left);
    digitalWrite(pin_motor_dir_right, motor_dir_right);

    if (STOP_COMMAND) {
        analogWrite(pin_motor_left,  0);
        analogWrite(pin_motor_right, 0);
        return;
    }

    analogWrite(pin_motor_left,  constrain(abs(velo_left),  0, 255));
    analogWrite(pin_motor_right, constrain(abs(velo_right), 0, 255));
}


void Driver::stop_motors()
{
    set_motors(0, 0);
}


double Driver::get_temperature()
{
    float temp = dht_sensor.readTemperature();
    if (isnan(temp)) return -1.0;
    return (double)temp;
}


double Driver::get_humidity()
{
    float hum = dht_sensor.readHumidity();
    if (isnan(hum)) return -1.0;
    return (double)hum;
}


long int Driver::get_distance(uint8_t sensor)
{
    long int IR_value = analogRead(sensor);
    // Безопасная проверка на ноль перед логарифмированием
    if (IR_value <= 0) return 3 * CRITICAL_DISTANCE;
    
    double log_val = log10((double)IR_value / 1821.2);
    if (log_val >= 0) return 3 * CRITICAL_DISTANCE;  // Защита от отрицательного логарифма
    
    long int distance = pow(10, log_val / -0.65);
    if (distance <= 0) distance = 3 * CRITICAL_DISTANCE;
    return distance;
}


void Driver::drive_forward_until(const int dist)
{
    while (get_distance(SENSOR_DIST_FRONT) > dist) {
        set_motors(speed, speed);
        if (read_command() == 's' || STOP_COMMAND) break;
        delay(10);  // Добавлена небольшая задержка
    }

    stop_motors();
    sound_signal(100);
}


void Driver::autonomous_drive()
{
    long int dist_front, dist_side;
    int e;
    double u;

    double kp = 2.5;

    while (read_command() != 's' && !STOP_COMMAND) {
        dist_front = get_distance(SENSOR_DIST_FRONT);
        dist_side = get_distance(SENSOR_DIST_SIDE);

        if (dist_front <= CRITICAL_DISTANCE) {
            stop_motors();
            turn_on_degree(45);
        }

        e = CRITICAL_DISTANCE * 0.9 - dist_side;
        u = kp * e;
        // Ограничение скорости
        int left_speed = constrain(110 - (int)u, -255, 255);
        int right_speed = constrain(110 + (int)u, -255, 255);
        set_motors(left_speed, right_speed);
        delay(10);
    }
}


void Driver::connection_lost_case()
{
    stop_motors();
    sound_signal(100);

    const int delay_time = 10;     
    const int drive_time = 2000;   
    int elapsed = 0;

    while (elapsed < drive_time) {
        if (STOP_COMMAND) break;

        set_motors(-150, -150);    
        delay(delay_time);
        elapsed += delay_time;
    }

    stop_motors();

    sound_signal(50);
    sound_signal(100);
    sound_signal(200);
}


void Driver::turn_on_degree(const int degree)
{
    int millisecs = rotation_time / 360 * abs(degree);
    int delay_counter = 0;

    // Используем простую проверку знака вместо copysign
    int left_speed = (degree >= 0) ? -speed : speed;
    int right_speed = (degree >= 0) ? speed : -speed;
    
    set_motors(left_speed, right_speed);
    
    while (delay_counter < millisecs) {
        if (read_command() == 's' || STOP_COMMAND) break;
        delay(1);
        delay_counter++;
    }
    stop_motors();
}


void Driver::inspection()
{
    int delay_time = 100;     // milliseconds
    int drive_time = 2000;
    int forward_counter = 0;
    int counter_limit = drive_time / delay_time;

    while (forward_counter < counter_limit) {
        set_motors(150, 150);
        if (get_distance(SENSOR_DIST_FRONT) < CRITICAL_DISTANCE * 1.5) break;
        if (read_command() == 's' || STOP_COMMAND) break;
        delay(delay_time);
        forward_counter++;
    }

    stop_motors();
    delay(100);

    turn_on_degree(360);
    delay(100);

    int back_counter = 0;

    while (back_counter < forward_counter) {
        set_motors(-150, -150);
        if (read_command() == 's' || STOP_COMMAND) break;
        delay(delay_time);
        back_counter++;
    }

    stop_motors();
    sound_signal(100);
}


void Driver::arm(const int power)
{
    pos_servo_arm = constrain(pos_servo_arm + ((power > 0) ? 5 : -5), 0, 45);
    servo_arm.write(pos_servo_arm);
}


void Driver::hand(const int power)
{
    if (power > 0) {
        servo_hand.write(10);
    } else {
        servo_hand.write(150);
    }
}


void Driver::get_command_wheels(char command)
{
    if (command == '0') {
        stop_motors();
        return;
    }

    STOP_COMMAND = false;
    LAMP_STATE = !LAMP_STATE;
    digitalWrite(LAMP_PIN, LAMP_STATE);

    switch (command) {
        case 's':
            STOP_COMMAND = true;
            stop_motors();
            break;
        case '1':
            set_motors(speed, speed);    // Вперед
            break;
        case '2':
            set_motors(-speed, -speed);  // Назад
            break;
        case '3':
            set_motors(speed, -speed);   // Поворот вправо
            break;
        case '4':
            set_motors(-speed, speed);   // Поворот влево
            break;
        default:
            stop_motors();
            break;
    }
}


void Driver::get_command_arm(char command)
{
    if (command != '0') {
        STOP_COMMAND = false;
        LAMP_STATE = !LAMP_STATE;
        digitalWrite(LAMP_PIN, LAMP_STATE);

        switch (command) {
            case 's':
                STOP_COMMAND = true;
                stop_motors();
                break;
            case '5':
                arm(100);   // Вверх
                break;
            case '6':
                arm(-100);  // Вниз
                break;
            case '7':
                if (command != last_msg_hand) {
                    hand(100);  // Сжать
                    last_msg_hand = command;
                }
                break;
            case '8':
                if (command != last_msg_hand) {
                    hand(-100);  // Разжать
                    last_msg_hand = command;
                }
                break;
            default:
                break;
        }
    }
}


void Driver::get_command_other(char command)
{
    if (command != '0') {
        STOP_COMMAND = false;
        LAMP_STATE = !LAMP_STATE;
        digitalWrite(LAMP_PIN, LAMP_STATE);

        switch (command) {
            case 's':
                STOP_COMMAND = true;
                stop_motors();
                break;
            case 'f':
                drive_forward_until(CRITICAL_DISTANCE);
                break;
            case 'e':
                connection_lost_case();
                break;
            case 'y':
                inspection();
                break;
            case 'o':
                turn_on_degree(360);
                break;
            case 'g':
                autonomous_drive();
                break;  // Добавлен break
            default:
                break;
        }
    }
}


Driver Wheels(6, 5, 7, 4, A1, A2, 200);


void setup()
{
    Serial.begin(9600);
    delay(3000);
    Serial.println("Arduino started. Logging initialized... %");
}


void loop()
{
    char command = Wheels.read_command();
    if (command != '0') { 
        unsigned long currentMillis = millis();

        Wheels.get_command_arm(command);
        Wheels.get_command_wheels(command);
        Wheels.get_command_other(command);

        Wheels.write_logs(command, currentMillis);

        Wheels.clear_serial_buffer();
    }
    delay(10);  // Небольшая задержка для стабильности
}