#include <Arduino.h>
#include <Servo.h>
#include <DHT.h>

#define SENSOR_DIST_FRONT       A3
#define SENSOR_DIST_SIDE        A0

#define DHTPIN                  9
#define DHTTYPE                 DHT11

#define TRIG_PIN                10
#define ECHO_PIN                11
#define CRITICAL_DISTANCE       30

DHT dht_sensor(DHTPIN, DHTTYPE);

volatile bool STOP_COMMAND = false;

bool obstacle = false;
unsigned long last_obstacle_log_time = 0;
const unsigned long OBSTACLE_LOG_INTERVAL = 2000;

volatile bool LOGS_ENABLED = true;

bool emergency_back = false;
unsigned long emergency_start = 0;
const unsigned long emergency_duration = 3000;


class Driver {
public:
    Driver(int left_pin, int right_pin, int left_dir_pin, int right_dir_pin,
           int servo_arm_pin, int servo_hand_pin, int motor_speed = 250);

    void set_motors(int velo_left, int velo_right);
    void stop_motors();
    void connection_lost_case();
    void turn_on_degree(int degree);
    char read_command();
    void clear_serial_buffer();
    void execute_wheel_command(char command);
    void execute_other_command(char command);
    long get_distance();
    void write_logs(const char* command);
    double get_temperature();
    double get_humidity();
    void start_emergency_back();

    bool rotating = false;
    unsigned long rotate_start = 0;
    int rotate_duration = 0;
    int rotate_left_speed = 0;
    int rotate_right_speed = 0;

private:
    int pin_motor_left;
    int pin_motor_right;
    int pin_motor_dir_left;
    int pin_motor_dir_right;
    int speed;

    char prev_command = '0';
    unsigned long last_command_time = 0;
    int rotation_time = 1500;
};

Driver::Driver(int left_pin, int right_pin, int left_dir_pin, int right_dir_pin,
               int servo_arm_pin, int servo_hand_pin, int motor_speed)
    : pin_motor_left(left_pin), pin_motor_right(right_pin),
      pin_motor_dir_left(left_dir_pin), pin_motor_dir_right(right_dir_pin),
      speed(motor_speed), rotation_time(1120)
{
    pinMode(pin_motor_left, OUTPUT);
    pinMode(pin_motor_right, OUTPUT);
    pinMode(pin_motor_dir_left, OUTPUT);
    pinMode(pin_motor_dir_right, OUTPUT);

    pinMode(SENSOR_DIST_FRONT, INPUT);
    pinMode(SENSOR_DIST_SIDE, INPUT);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    dht_sensor.begin();

    set_motors(0, 0);
}

void Driver::clear_serial_buffer() {
    while (Serial.available() > 0) Serial.read();
}

char Driver::read_command() {
    if (Serial.available() == 0) return '0';

    char command = Serial.read();
    unsigned long current_time = millis();

    if (command == '\n' || command == '\r') return '0';

    if (command == prev_command && (current_time - last_command_time) < 100) {
        clear_serial_buffer();
        return '0';
    }

    prev_command = command;
    last_command_time = current_time;

    return command;
}

void Driver::write_logs(const char* action) {
    Serial.print("Time: ");
    Serial.print(millis());
    Serial.print(" -> ");
    Serial.println(action);
}

void Driver::set_motors(int velo_left, int velo_right) {
    int dir_left  = (velo_left >= 0) ? HIGH : LOW;
    int dir_right = (velo_right >= 0) ? HIGH : LOW;

    digitalWrite(pin_motor_dir_left, dir_left);
    digitalWrite(pin_motor_dir_right, dir_right);

    if (STOP_COMMAND) {
        analogWrite(pin_motor_left, 0);
        analogWrite(pin_motor_right, 0);
        return;
    }

    analogWrite(pin_motor_left, constrain(abs(velo_left), 0, 255));
    analogWrite(pin_motor_right, constrain(abs(velo_right), 0, 255));
}

void Driver::stop_motors() {
    set_motors(0, 0);
}

long Driver::get_distance() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration == 0) return -1;

    return duration * 0.034 / 2;
}

void Driver::turn_on_degree(int degree) {
    rotate_duration = rotation_time / 360 * abs(degree);
    rotate_left_speed  = (degree >= 0) ? -speed : speed;
    rotate_right_speed = (degree >= 0) ? speed : -speed;
    rotate_start = millis();
    rotating = true;
}


double Driver::get_temperature() {
    float temp = dht_sensor.readTemperature();
    if (isnan(temp)) return -1.0;
    return (double)temp;
}


double Driver::get_humidity() {
    float hum = dht_sensor.readHumidity();
    if (isnan(hum)) return -1.0;
    return (double)hum;
}


void Driver::connection_lost_case() {
    stop_motors();
}


void Driver::start_emergency_back() {
    emergency_back = true;
    emergency_start = millis();
    STOP_COMMAND = false;   // снимаем стоп если был
}



void Driver::execute_wheel_command(char command) {

    STOP_COMMAND = false;

    switch (command) {

        case 'w':
            set_motors(speed, speed);
            write_logs("Moving forward");
            break;

        case 's':
            set_motors(-speed, -speed);
            write_logs("Moving backward");
            break;

        case 'd':
            set_motors(speed, -speed);
            write_logs("Turning right");
            break;

        case 'a':
            set_motors(-speed, speed);
            write_logs("Turning left");
            break;

        case 'q':
            turn_on_degree(-360);
            write_logs("Rotating 360 degrees left");
            break;

        case 'e':
            turn_on_degree(360);
            write_logs("Rotating 360 degrees right");
            break;

        default:
            stop_motors();
            write_logs("Motors stopped");
            break;
    }
}


void Driver::execute_other_command(char command) {
    if (command == 'e') connection_lost_case();
    else if (command == 'o') turn_on_degree(360);
    else if (command == 'x') {STOP_COMMAND = true; stop_motors();}
    else if (command == 'f') {
        float temp = get_temperature();
        float hum  = get_humidity();
        long dist  = get_distance();

        Serial.print("Sensor Data -> Temp: ");
        Serial.print(temp);
        Serial.print(" C, Humidity: ");
        Serial.print(hum);
        Serial.print(" %, Distance: ");
        Serial.print(dist);
        Serial.println(" cm");
    }
}


Driver Wheels(6, 5, 7, 4, A1, A2, 200);

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Arduino started. Logging enabled...");
}


void loop() {
    unsigned long current_millis = millis();


    if (Wheels.rotating) {
        if (current_millis - Wheels.rotate_start < Wheels.rotate_duration) {
            Wheels.set_motors(Wheels.rotate_left_speed, Wheels.rotate_right_speed);
        } else {
            Wheels.rotating = false;
            Wheels.stop_motors();
            Wheels.write_logs("Rotation complete");
        }
    } else {
        char command = Wheels.read_command();
        if (command != '0') {
            Wheels.execute_wheel_command(command);
            Wheels.execute_other_command(command);
            Wheels.clear_serial_buffer();
        }
    }
}
