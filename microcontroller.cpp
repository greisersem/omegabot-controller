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
volatile bool LOGS_ENABLED = true;

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
    void write_logs(char command, unsigned long time);

private:
    int pin_motor_left;
    int pin_motor_right;
    int pin_motor_dir_left;
    int pin_motor_dir_right;
    int speed;

    Servo servo_arm;
    Servo servo_hand;
    char prev_command = '0';
    unsigned long last_command_time = 0;
    int rotation_time = 1500;

    bool rotating = false;
    unsigned long rotate_start = 0;
    int rotate_duration = 0;
    int rotate_left_speed = 0;
    int rotate_right_speed = 0;
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
    servo_arm.write(0);
    servo_hand.write(150);
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

void Driver::write_logs(char command, unsigned long time) {
    Serial.print("Time: ");
    Serial.print(time);
    Serial.print("; Command: ");
    Serial.println(command);
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

void Driver::connection_lost_case() {
    stop_motors();
}

void Driver::execute_wheel_command(char command) {
    if (command == '0') {
        stop_motors();
        return;
    }

    STOP_COMMAND = false;

    if (obstacle && command == '1') {
        set_motors(0, 0);
        return;
    }

    switch (command) {
        case 'w': set_motors(speed, speed); break; 
        case 's': set_motors(-speed, -speed); break; 
        case 'd': set_motors(speed, -speed); break; 
        case 'a': set_motors(-speed, speed); break;   
        case 'q': turn_on_degree(-360); break;
        case 'e': turn_on_degree(360); break;
        default: stop_motors(); break;
    }
}

void Driver::execute_other_command(char command) {
    if (command == 'e') connection_lost_case();
    else if (command == 'o') turn_on_degree(360);
    else if (command == 's') { STOP_COMMAND = true; stop_motors(); }
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
    Serial.begin(9600);
    delay(1000);
    Serial.println("Arduino started. Logging enabled...");
}

void loop() {
    unsigned long currentMillis = millis();

    long distance = Wheels.get_distance();
    if (distance > 0 && distance < CRITICAL_DISTANCE) {
        if (!obstacle) Serial.println("Obstacle detected! Motors stopped");
        obstacle = true;
        Wheels.stop_motors();
    } else {
        obstacle = false;
    }

    if (Wheels.rotating) {
        if (currentMillis - Wheels.rotate_start < Wheels.rotate_duration) {
            Wheels.set_motors(Wheels.rotate_left_speed, Wheels.rotate_right_speed);
        } else {
            Wheels.rotating = false;
            Wheels.stop_motors();
        }
    } else {
        char command = Wheels.read_command();
        if (command != '0') {
            Wheels.execute_wheel_command(command);
            Wheels.execute_other_command(command);
            if (LOGS_ENABLED) Wheels.write_logs(command, currentMillis);
            Wheels.clear_serial_buffer();
        }
    }
}
