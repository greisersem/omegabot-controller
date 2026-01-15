#define LEFT_SPEED
#define LEFT_DIR
#define RIGHT_SPEED
#define RIGHT_DIR


void stop() 
{
    digitalWrite(LEFT_SPEED, LOW);
    digitalWrite(RIGHT_SPEED, LOW);
}


void turn_right()
{
    digitalWrite(LEFT_DIR, HIGH);
    digitalWrite(RIGHT_DIR, LOW);
    analogWrite(LEFT_SPEED, speed);
    analogWrite(RIGHT_SPEED, speed);
}


void turn_left()
{
    digitalWrite(LEFT_DIR, LOW);
    digitalWrite(RIGHT_DIR, HIGH);
    analogWrite(LEFT_SPEED, speed);
    analogWrite(RIGHT_SPEED, speed);
}


void forward()
{
    digitalWrite(LEFT_DIR, LOW);
    digitalWrite(RIGHT_DIR, LOW);
    analogWrite(LEFT_SPEED, speed);
    analogWrite(RIGHT_SPEED, speed);
}


void backward()
{
    digitalWrite(LEFT_DIR, HIGH);
    digitalWrite(RIGHT_DIR, HIGH);
    analogWrite(LEFT_SPEED, speed);
    analogWrite(RIGHT_SPEED, speed);
}


void setup() 
{
    pinMode(LEFT_SPEED, OUTPUT);
    pinMode(LEFT_DIR, OUTPUT);
    pinMode(RIGHT_SPEED, OUTPUT);
    pinMode(RIGHT_DIR, OUTPUT);
    Serial.begin(115200);
}


void loop() 
{
    if (Serial.available()) {
        char c = Serial.read();

        switch (c) {
        case 'W': forward(); break;
        case 'S': backward(); break;
        case 'A': left(); break;
        case 'D': right(); break;
        case 'X': stopMotors(); break;
        }
    }
}