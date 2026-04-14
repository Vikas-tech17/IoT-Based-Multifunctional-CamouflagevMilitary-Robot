/* NEXORA — FINAL MASTER (Calibration-safe, software-workaround for wiring oddities)
   - Fixes directions so F = Forward, B = Back, L = Left, R = Right
   - Applies left-side polarity fix (based on your tests)
   - Re-asserts left pins after right-side changes to avoid cross-triggering
   - Softer turning PWM to reduce struggle
   - RFID security, sensors, GPS included
*/

#include <SPI.h>
#include <MFRC522.h>
#include <TinyGPSPlus.h>

// ======================
//      CONFIGURATION
// ======================

// Authorized UID
String authorizedUID = "13 74 0B 2D";
String authorizedName = "Sinan";

// motor timing / strength
const uint8_t DRIVE_FULL = 240;      // 0-255 full drive for straight
const uint8_t TURN_FULL  = 200;      // 0-255 for turning (reduced to avoid stall)
const uint16_t TURN_PRE_DELAY = 60;  // ms: tiny pre-drive to help gearbox engage

// If true, code will re-assert left pins after toggling right pins to cancel cross-effects.
const bool FORCE_LEFT_REASSERT_AFTER_RIGHT = true;

// ======================
//       PINS
// ======================

// Motors (L298N wiring)
const uint8_t ENA_pin = 5;
const uint8_t IN1_pin = 7;  // Left control A
const uint8_t IN2_pin = 6;  // Left control B

const uint8_t ENB_pin = 10;
const uint8_t IN3_pin = 8;  // Right control A
const uint8_t IN4_pin = 9;  // Right control B

// Sensors
#define TRIG 22
#define ECHO 23
#define IR_LEFT 24
#define IR_RIGHT 25
#define PIR_PIN 26
#define MQ2 A0
#define MQ135 A1
#define METAL A2
#define MIC A3

// RFID
#define SS_PIN 53
#define RST_PIN 49
MFRC522 rfid(SS_PIN, RST_PIN);

// GPS
TinyGPSPlus gps;

// ======================
//   INTERNAL STATE
// ======================
bool unlocked = false;
unsigned long unlockTime = 0;
unsigned long lastLockMsg = 0;
unsigned long lastSensor = 0;

// ======================
//   BASIC MOTOR HELPERS
// ======================

// Write left pins explicitly for "forward" or "back"
// Based on your tests, LEFT forward is: IN1 = LOW, IN2 = HIGH
void leftDriveForward(uint8_t pwm) {
  digitalWrite(IN1_pin, LOW);
  digitalWrite(IN2_pin, HIGH);
  analogWrite(ENA_pin, pwm);
}
void leftDriveBack(uint8_t pwm) {
  digitalWrite(IN1_pin, HIGH);
  digitalWrite(IN2_pin, LOW);
  analogWrite(ENA_pin, pwm);
}
void leftStop() {
  analogWrite(ENA_pin, 0);
  digitalWrite(IN1_pin, LOW);
  digitalWrite(IN2_pin, LOW);
}

// Right side helpers
// From tests: right forward is IN3=HIGH, IN4=LOW (but caused side-effects earlier).
void rightDriveForward(uint8_t pwm) {
  digitalWrite(IN3_pin, HIGH);
  digitalWrite(IN4_pin, LOW);
  analogWrite(ENB_pin, pwm);
}
void rightDriveBack(uint8_t pwm) {
  digitalWrite(IN3_pin, LOW);
  digitalWrite(IN4_pin, HIGH);
  analogWrite(ENB_pin, pwm);
}
void rightStop() {
  analogWrite(ENB_pin, 0);
  digitalWrite(IN3_pin, LOW);
  digitalWrite(IN4_pin, LOW);
}

// High-level moves that combine left/right and apply the workaround reassert
void moveForward() {
  // set left forward first (based on tested good pattern)
  leftDriveForward(DRIVE_FULL);
  delay(5);
  // set right forward
  rightDriveForward(DRIVE_FULL);
  if (FORCE_LEFT_REASSERT_AFTER_RIGHT) {
    // re-assert left pins to override stray influence
    delay(3);
    leftDriveForward(DRIVE_FULL);
  }
}

void moveBack() {
  leftDriveBack(DRIVE_FULL);
  delay(5);
  rightDriveBack(DRIVE_FULL);
  if (FORCE_LEFT_REASSERT_AFTER_RIGHT) {
    delay(3);
    leftDriveBack(DRIVE_FULL);
  }
}

void turnLeft() {
  // left backward, right forward
  leftDriveBack(TURN_FULL);
  delay(TURN_PRE_DELAY);
  rightDriveForward(TURN_FULL);
  // re-assert left to reduce contamination
  if (FORCE_LEFT_REASSERT_AFTER_RIGHT) {
    delay(3);
    leftDriveBack(TURN_FULL);
  }
}

void turnRight() {
  // left forward, right backward
  leftDriveForward(TURN_FULL);
  delay(TURN_PRE_DELAY);
  rightDriveBack(TURN_FULL);
  if (FORCE_LEFT_REASSERT_AFTER_RIGHT) {
    delay(3);
    leftDriveForward(TURN_FULL);
  }
}

void stopAll() {
  leftStop();
  rightStop();
}

// ======================
//   ULTRASONIC
// ======================
long readUltrasonic() {
  digitalWrite(TRIG, LOW); delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long duration = pulseIn(ECHO, HIGH, 20000UL);
  if (duration == 0) return -1;
  return duration / 58;
}

// ======================
//       SETUP
// ======================
void setup() {
  Serial.begin(115200);

  // motor pins
  pinMode(ENA_pin, OUTPUT);
  pinMode(IN1_pin, OUTPUT);
  pinMode(IN2_pin, OUTPUT);

  pinMode(ENB_pin, OUTPUT);
  pinMode(IN3_pin, OUTPUT);
  pinMode(IN4_pin, OUTPUT);

  // sensors
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(IR_LEFT, INPUT);
  pinMode(IR_RIGHT, INPUT);
  pinMode(PIR_PIN, INPUT);

  // RFID
  SPI.begin();
  rfid.PCD_Init();

  // GPS
  Serial1.begin(9600);

  // ensure motors stopped at boot
  stopAll();

  Serial.println("============== NEXORA BOOT ==============");
  Serial.println("Security: RFID locked until authorized card scanned");
  Serial.println("Use keys: F B L R S  (uppercase or lowercase)");
  Serial.println("-------------------------------------------");
}

// ======================
//         LOOP
// ======================
void loop() {

  // ---------- RFID handling ----------
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      String hexByte = String(rfid.uid.uidByte[i], HEX);
      hexByte.toUpperCase();
      if (hexByte.length() < 2) hexByte = "0" + hexByte;
      uid += hexByte;
      if (i < rfid.uid.size - 1) uid += " ";
    }
    Serial.print("RFID UID Detected: ");
    Serial.println(uid);

    if (uid == authorizedUID) {
      if (!unlocked) {
        unlocked = true;
        unlockTime = millis();
        Serial.println("ACCESS GRANTED");
        Serial.print("Welcome, ");
        Serial.println(authorizedName);
        Serial.println("NEXORA is now operational.");
      } else {
        Serial.println("Already unlocked.");
      }
    } else {
      Serial.println("ACCESS DENIED — Unauthorized card.");
      // remain locked; optionally you can add a lockout counter here
    }
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  // ---------- Locked behaviour (reduce spam) ----------
  if (!unlocked) {
    stopAll();
    if (millis() - lastLockMsg > 2000) {
      Serial.println("NEXORA LOCKED — Scan authorized RFID card.");
      lastLockMsg = millis();
    }
    delay(50);
    return;
  }

  // ---------- Serial command control ----------
  if (Serial.available()) {
    char c = Serial.read();
    if (c >= 'a' && c <= 'z') c -= 32; // uppercase

    if (c == 'F') {
      moveForward();
      Serial.println("[CMD] FORWARD");
    } else if (c == 'B') {
      moveBack();
      Serial.println("[CMD] BACK");
    } else if (c == 'L') {
      turnLeft();
      Serial.println("[CMD] LEFT");
    } else if (c == 'R') {
      turnRight();
      Serial.println("[CMD] RIGHT");
    } else if (c == 'S') {
      stopAll();
      Serial.println("[CMD] STOP");
    }
  }

  // ---------- Sensors output throttled ----------
  if (millis() - lastSensor >= 400) {
    lastSensor = millis();

    Serial.print("Ultrasonic: "); Serial.print(readUltrasonic()); Serial.println(" cm");
    Serial.print("IR L: "); Serial.print(digitalRead(IR_LEFT));
    Serial.print(" | IR R: "); Serial.println(digitalRead(IR_RIGHT));
    Serial.print("PIR: "); Serial.println(digitalRead(PIR_PIN) ? "MOTION" : "NO MOTION");
    Serial.print("MQ-2: "); Serial.print(analogRead(MQ2));
    Serial.print(" | MQ-135: "); Serial.println(analogRead(MQ135));
    Serial.print("Metal: "); Serial.println(analogRead(METAL));
    int micVal = analogRead(MIC);
    Serial.print("Mic: "); Serial.print(micVal);
    if (micVal > 600) Serial.print(" <--- LOUD!");
    Serial.println();

    while (Serial1.available()) gps.encode(Serial1.read());
    Serial.print("GPS: ");
    if (gps.location.isValid()) {
      Serial.print("Lat: "); Serial.print(gps.location.lat(), 6);
      Serial.print(" | Lon: "); Serial.println(gps.location.lng(), 6);
    } else {
      Serial.println("No Fix");
    }

    Serial.print("NEXORA Active For: ");
    Serial.print((millis() - unlockTime) / 1000);
    Serial.println(" sec");

    Serial.println("-------------------------------------------");
  }

  delay(8); // tiny yield
}
