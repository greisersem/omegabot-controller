#include <Arduino.h>
#include <Servo.h>
#include <DHT.h>
#include <math.h>

#define DHT_PIN                9
#define DHT_TYPE               DHT11

#define TRIG_PIN               10
#define ECHO_PIN               11
#define CRITICAL_DISTANCE      10    // centimeters

#define MOTOR_LEFT             6
#define MOTOR_RIGHT            5
#define MOTOR_DIR_LEFT         7
#define MOTOR_DIR_RIGHT        4

#define SPEED                  150
#define ROTATION_TIME          2000  // time to rotate 360 deg in millis

#define DISCONNECTION_DURATION 2000  // time to move backward when disconnected

#define INSPECTION_FORWARD     2000  // time to move forward in inspection
#define INSPECTION_BACKWARD    2000  // time to move backward in inspection

DHT dht_sensor(DHT_PIN, DHT_TYPE);

const unsigned long OBSTACLE_LOG_INTERVAL = 2000;


class Driver {
public:
    Driver(int left_pin, int right_pin, int left_dir_pin, int right_dir_pin, int motor_speed, int trig_pin, int echo_pin, int rotation_time); 
    ~Driver();
    
    void set_motors(const int velo_left, const int velo_right);
    void connection_lost_case();
    void inspection();
    void turn_on_degree(const int degree);

    char read_command();
    void get_command_wheels(char command);
    void get_command_other(char command);

    long int get_distance();
    double get_temperature();
    double get_humidity();

    void check_obstacle();
    void check_flags();
    void interrupt_actions();

    char last_command = '0';
    unsigned long last_command_time = 0;

    bool obstacle = false;
    unsigned long last_obstacle_log_time = 0;

    bool rotating = false;
    unsigned int current_rotation_duration; 

    bool inspecting = false;
    
    bool connection = true;

    unsigned long inspection_start_time = 0;
    unsigned long inspection_pause_time = 0;
    int inspection_state = 0;

private:
    int pin_motor_left;
    int pin_motor_right;
    int pin_motor_dir_left;
    int pin_motor_dir_right;
    int speed;

    int pin_trig;
    int pin_echo;

    unsigned long rotation_start_time = 0;
    unsigned long disconnect_start_time = 0;
    unsigned int connection_lost_duration = DISCONNECTION_DURATION;
    
private:
    int rotation_speed;
    unsigned int rotation_duration; 
    
    unsigned long inspection_forward_duration = INSPECTION_FORWARD;
    unsigned long inspection_backward_duration = INSPECTION_BACKWARD;
};


Driver::Driver(int left_pin, int right_pin, int left_dir_pin, int right_dir_pin, int motor_speed, int trig_pin, int echo_pin, int rotation_time) 
    : pin_motor_left(left_pin), pin_motor_right(right_pin), 
      pin_motor_dir_left(left_dir_pin), pin_motor_dir_right(right_dir_pin), 
      speed(motor_speed), pin_trig(trig_pin), pin_echo(echo_pin), rotation_duration(rotation_time)
{
    pinMode(pin_motor_left, OUTPUT);
    pinMode(pin_motor_right, OUTPUT);
    pinMode(pin_motor_dir_left, OUTPUT);
    pinMode(pin_motor_dir_right, OUTPUT);
    dht_sensor.begin();

    pinMode(pin_trig, OUTPUT);
    pinMode(pin_echo, INPUT);
    rotation_speed = 0.5 * speed;

    set_motors(0, 0);
}


Driver::~Driver()
{
    set_motors(0, 0);
}


char Driver::read_command()
{
    if (Serial.available() > 0) {
        char command = Serial.read();
        int current_time = millis();

        if (command == '\n' || command == '\r') {
            return '0';
        }

        if (last_command != command) {
            last_command = command;
            last_command_time = current_time;
        }
        return command;
    }

    return '0';
}


void Driver::set_motors(const int velo_left, const int velo_right)
{
    int motor_dir_left  = (velo_left  >= 0) ? HIGH : LOW;
    int motor_dir_right = (velo_right >= 0) ? HIGH : LOW;

    digitalWrite(pin_motor_dir_left, motor_dir_left);
    digitalWrite(pin_motor_dir_right, motor_dir_right);

    analogWrite(pin_motor_left,  constrain(abs(velo_left),  0, 255));
    analogWrite(pin_motor_right, constrain(abs(velo_right), 0, 255));
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
    digitalWrite(pin_trig, LOW);
    delayMicroseconds(2);
    digitalWrite(pin_trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(pin_trig, LOW);

    long duration = pulseIn(pin_echo, HIGH, 30000);
    
    if (duration == 0) {
        return -1;
    } else {
        return duration * 0.034 / 2;
    }
}


void Driver::turn_on_degree(const int degree)
{
    if (!rotating)
    {
        Serial.println("Rotation started");
        rotation_start_time = millis();
        current_rotation_duration = (ROTATION_TIME / 360.0) * abs(degree);

        int rotation_left_speed  = (degree >= 0) ? -speed : speed;
        int rotation_right_speed = (degree >= 0) ?  speed : -speed;

        set_motors(rotation_left_speed, rotation_right_speed);
        rotating = true;
        return;
    }

    if (millis() - rotation_start_time >= current_rotation_duration)
    {
        set_motors(0, 0);
        rotating = false;
        last_command = '0';
    }
}


void Driver::connection_lost_case()
{
    if (connection)
    {
        Serial.println("Connection lost. Moving backward");
        connection = false;
        disconnect_start_time = millis();
        set_motors(-speed, -speed);
        return;
    }

    if (millis() - disconnect_start_time >= connection_lost_duration)
    {
        set_motors(0, 0);
        connection = true;
    }
}


void Driver::inspection()
{
    if (!inspecting)
    {
        Serial.println("Inspection started");
        inspecting = true;
        inspection_state = 0;
        inspection_start_time = millis();
        return;
    }

    unsigned long now = millis();

    switch (inspection_state)
    {
        case 0:
            if (obstacle)
            {
                set_motors(0, 0);
                inspection_pause_time = now;
                inspection_backward_duration = now - inspection_start_time;
                inspection_state = 1;
                break;
            }

            set_motors(speed, speed);

            if (now - inspection_start_time >= inspection_forward_duration)
            {
                set_motors(0, 0);
                inspection_pause_time = now;
                inspection_backward_duration = inspection_forward_duration;
                inspection_state = 1;
            }
            break;

        case 1:
            if (now - inspection_pause_time >= 100)
            {
                turn_on_degree(180);
                inspection_state = 2;
            }
            break;

        case 2:
            if (!rotating)
            {
                inspection_start_time = millis();
                inspection_state = 3;
            }
            break;

        case 3:
            set_motors(-speed, -speed);

            if (now - inspection_start_time >= inspection_backward_duration)
            {
                set_motors(0, 0);
                inspecting = false;
                inspection_state = 0;
            }
            break;
    }
}


void Driver::get_command_wheels(char command)
{
    if ((command == 'w' || command == 'a' || command == 's' ||
        command == 'd' || command == 'x' || command == 'q' || command == 'e') &&
        (rotating || inspecting || !connection)) {
        interrupt_actions();
    }
    
    switch (command) {
        case 'w':
            if (obstacle) {
                set_motors(0, 0);
                Serial.println("Forward blocked by obstacle");
            } else {
                set_motors(speed, speed);
            }
            break;
        case 's': 
            set_motors(0, 0);
            break;
        case 'x': 
            set_motors(-speed, -speed);
            break;
        case 'd':  
            set_motors(speed, -speed);
            break;
        case 'a': 
            set_motors(-speed, speed);
            break;
        case 'q': 
            turn_on_degree(-180);
            break;
        case 'e': 
            turn_on_degree(180);
            break;
    }
}


void Driver::get_command_other(char command)
{
    switch (command) {
        case 'o':
            connection_lost_case();
            break;
        case 'c':
            inspection();
            break;
        case 'f':
            {
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
            }
            break;
    }
}


void Driver::check_obstacle()
{
    long distance = get_distance();
    unsigned long current_time = millis();

    if (distance > 0 && distance < CRITICAL_DISTANCE) {
        if (!obstacle) {
            obstacle = true;
            last_obstacle_log_time = current_time;
            Serial.println("Obstacle detected! Forward blocked");
            
            if (last_command == 'w') {
                set_motors(0, 0);
                Serial.println("Stopping forward motion due to obstacle");
            }
        } 
        else if (current_time - last_obstacle_log_time >= OBSTACLE_LOG_INTERVAL) {
            Serial.println("Obstacle still present");
            last_obstacle_log_time = current_time;
        }
    } else {
        if (obstacle) {
            obstacle = false;
            Serial.println("Obstacle cleared");
        }
    }
}


void Driver::check_flags()
{
    if (!connection) connection_lost_case();
    if (rotating) turn_on_degree(0);
    if (inspecting) inspection(); 
}


void Driver::interrupt_actions()
{
    Serial.println("Action interrupted");

    rotating = false;
    inspecting = false;
    connection = true;

    inspection_state = 0;

    set_motors(0, 0);
}


Driver Wheels(
    MOTOR_LEFT,
    MOTOR_RIGHT,
    MOTOR_DIR_LEFT,
    MOTOR_DIR_RIGHT,
    SPEED,
    TRIG_PIN,
    ECHO_PIN,
    ROTATION_TIME
);


void setup()
{
    Serial.begin(115200); 
    delay(1000);
    Serial.println("Arduino started");
}


void loop()
{    
    Wheels.check_obstacle();
    Wheels.check_flags();
    
    char command = Wheels.read_command();
    if (command != '0') {
        Wheels.get_command_wheels(command);
        Wheels.get_command_other(command);
    } else {
        if (!Wheels.rotating && !Wheels.inspecting && Wheels.connection)
        {
            Wheels.set_motors(0, 0);
        }
    }

    delay(10);
}