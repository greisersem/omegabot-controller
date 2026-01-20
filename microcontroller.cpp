#include <Arduino.h>
#include <Servo.h>
#include <DHT.h>
#include <math.h>

#define CRITICAL_DISTANCE       30

#define SENSOR_DIST_FRONT       A3
#define SENSOR_DIST_SIDE        A0

#define DHTPIN                  9
#define DHTTYPE                 DHT11

#define TRIG_PIN                10
#define ECHO_PIN                11
#define MAX_DIST                10

#define SENSOR_LIGHT            3

DHT dht_sensor(DHTPIN, DHTTYPE);

volatile bool LAMP_STATE   = false;
volatile bool STOP_COMMAND = false;
bool obstacle = false;
volatile bool LOGS_ENABLED = false;


class Driver {
public:
    Driver(int left_pin, int right_pin, int left_dir_pin, int right_dir_pin, int servo_arm_pin, int servo_hand_pin, int motor_speed = 250); 
    ~Driver();
    
    void set_motors(const int velo_left, const int velo_right);
    void stop_motors();
    void connection_lost_case();
    void inspection();
    void turn_on_degree(const int degree);

    char read_command();
    void clear_serial_buffer();
    void get_command_wheels(char command);
    void get_command_other(char command);

    long int get_distance();
    double get_temperature();
    double get_humidity();
    
    void write_logs(char command, unsigned long time);

private:
    Servo servo_arm;
    Servo servo_hand;
    int pin_motor_left;
    int pin_motor_right;
    int pin_motor_dir_left;
    int pin_motor_dir_right;
    int speed;
    
private:
    char last_msg_hand;
    char previos_command;
    unsigned long previous_command_time = 0;
    int rotation_speed; 
    int rotation_time;          //milliseconds
};


Driver::Driver(int left_pin, int right_pin, int left_dir_pin, int right_dir_pin, int servo_arm_pin, int servo_hand_pin, int motor_speed) 
    : pin_motor_left(left_pin), pin_motor_right(right_pin), pin_motor_dir_left(left_dir_pin), pin_motor_dir_right(right_dir_pin), 
      speed(motor_speed), last_msg_hand('0'), previos_command('0'), rotation_time(1120)
{
    pinMode(pin_motor_left, OUTPUT);
    pinMode(pin_motor_right, OUTPUT);
    pinMode(pin_motor_dir_left, OUTPUT);
    pinMode(pin_motor_dir_right, OUTPUT);
    pinMode(SENSOR_DIST_FRONT, INPUT);
    pinMode(SENSOR_DIST_SIDE, INPUT);
    pinMode(SENSOR_LIGHT, INPUT);  // Добавлено
    dht_sensor.begin();

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
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
    // double temp = get_temperature();
    // double hum  = get_humidity();

    String log_msg;
    log_msg = "Time: " + String(time) + "; Command: " + String(command);

    Serial.println(log_msg);
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


long Driver::get_distance() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    // Считаем время возвращения эха
    long duration = pulseIn(ECHO_PIN, HIGH, 30000); // Таймаут 30 мс (~5 м)
    
    long distance_cm;

    if (duration == 0) {
        distance_cm = -1;  // Если нет сигнала
        return distance_cm;
    } else {
        distance_cm = duration * 0.034 / 2; // Преобразуем в см
        return distance_cm;
    }
}


void Driver::connection_lost_case()
{
    stop_motors();

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
}


void Driver::get_command_wheels(char command)
{
    if (command == '0') {
        stop_motors();
        return;
    }

    STOP_COMMAND = false;
    LAMP_STATE = !LAMP_STATE;

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


void Driver::get_command_other(char command)
{
    if (command != '0') {
        STOP_COMMAND = false;
        LAMP_STATE = !LAMP_STATE;

        switch (command) {
            case 's':
                STOP_COMMAND = true;
                stop_motors();
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
    long distance = Wheels.get_distance();

    if (distance < 30) {
        Wheels.stop_motors();
        if (obstacle) {
          Serial.println("Obstacle detected! Motors stopped.");
        }
        obstacle = true;
    } else {
      obstacle = false;
    }
    
    if (!obstacle) {
        char command = Wheels.read_command();
        if (command != '0') { 
            unsigned long currentMillis = millis();
            if (command == 'l') {
                LOGS_ENABLED = !LOGS_ENABLED;   
            } else {
                Wheels.get_command_wheels(command);
                Wheels.get_command_other(command);
            }
            if (LOGS_ENABLED && command != 'l') {
                Wheels.write_logs(command, currentMillis);
            }
            Wheels.clear_serial_buffer();
        }
    }
    delay(100);  
}
