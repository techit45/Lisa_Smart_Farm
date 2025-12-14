#include <AccelStepper.h>
#include <ArduinoJson.h>

// =========================================
// 1. ตั้งค่า Serial Communication
// =========================================
// ใช้ Serial เพื่อสื่อสารกับคอมพิวเตอร์
// Command Format: JSON
// {"cmd":"tree","id":2}
// {"cmd":"pump","type":"water"}
// {"cmd":"status"}

// =========================================
// 2. ตั้งค่า Hardware
// =========================================
#define STEPS_PER_REVOLUTION 1600

// แกน X
#define DIR_PIN_X 26
#define PUL_PIN_X 27
#define LIMIT_SWITCH_PIN_X_HOME 14
#define LIMIT_SWITCH_PIN_X_END 5

// แกน Y
#define DIR_PIN_Y 32
#define PUL_PIN_Y 33
#define LIMIT_SWITCH_PIN_Y_HOME 13
#define LIMIT_SWITCH_PIN_Y_END 15

// --- เพิ่ม: Relay ---
#define RELAY_WATER_PIN 22  // ปั๊มน้ำเปล่า
#define RELAY_FERT_PIN 23   // ปั๊มน้ำปุ๋ย

// --- เพิ่ม: Soil Moisture (ใช้ ADC1 เพื่อไม่ชนกับ WiFi) ---
#define SOIL_PIN_2 34  // กระถาง 2
#define SOIL_PIN_5 36  // กระถาง 5
#define SOIL_PIN_8 35  // กระถาง 8

AccelStepper stepperX(AccelStepper::DRIVER, PUL_PIN_X, DIR_PIN_X);
AccelStepper stepperY(AccelStepper::DRIVER, PUL_PIN_Y, DIR_PIN_Y);

// ตัวแปรเก็บระยะสูงสุด (วัดอัตโนมัติ)
long maxStepsX = 0;
long maxStepsY = 0;
int currentTargetTree = 0;
bool isCalibrated = false;

// ตัวแปรเก็บค่าความชื้น
int soilValues[3] = { 0, 0, 0 };

// Buffer สำหรับรับคำสั่งจาก Serial
String serialBuffer = "";

// พิกัดต้นไม้ 9 ต้น (0.0 - 1.0)
const float TREE_POS[9][2] = {
  { 0.2, 0.2 }, { 0.5, 0.2 }, { 0.8, 0.2 }, { 0.2, 0.5 }, { 0.5, 0.5 }, { 0.8, 0.5 }, { 0.2, 0.8 }, { 0.5, 0.8 }, { 0.8, 0.8 }
};

// =========================================
// 3. Helper Functions
// =========================================
bool isPressed(int pin) {
  // อ่านค่า 2 ครั้งเพื่อกันสัญญาณรบกวน (Debounce)
  if (digitalRead(pin) == HIGH) {
    delay(10);
    if (digitalRead(pin) == HIGH) return true;
  }
  return false;
}

// ฟังก์ชันอ่านค่าความชื้นแปลงเป็น %
int readSoil(int pin) {
  int val = analogRead(pin);
  // ค่า Analog ESP32 คือ 0-4095
  // เซนเซอร์ส่วนใหญ่: แห้ง=4095, เปียก=0 (ต้อง map กลับทาง)
  // *ควรปรับจูนเลข 4095 และ 1500 ตามค่าจริงที่วัดได้*
  int percent = map(val, 4095, 1000, 0, 100);

  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return percent;
}

// =========================================
// 4. ฟังก์ชัน Calibration (Full Auto)
// =========================================
void calibrateSystem() {
  Serial.println("\n--- Starting Full Auto Calibration ---");

  float calibSpeed = 800.0;
  stepperX.setMaxSpeed(1500);
  stepperX.setAcceleration(500);
  stepperY.setMaxSpeed(1500);
  stepperY.setAcceleration(500);

  // PART 1: แกน X
  Serial.println("1. Homing X...");
  stepperX.setSpeed(-calibSpeed);
  while (!isPressed(LIMIT_SWITCH_PIN_X_HOME)) { stepperX.runSpeed(); }
  stepperX.stop();
  stepperX.setCurrentPosition(0);
  stepperX.runToNewPosition(200);
  stepperX.setCurrentPosition(0);

  Serial.println("2. Measuring X...");
  stepperX.setSpeed(calibSpeed);
  while (!isPressed(LIMIT_SWITCH_PIN_X_END)) { stepperX.runSpeed(); }
  stepperX.stop();
  maxStepsX = stepperX.currentPosition();
  stepperX.runToNewPosition(0);  // กลับบ้าน

  // PART 2: แกน Y
  Serial.println("4. Homing Y...");
  stepperY.setSpeed(-calibSpeed);
  while (!isPressed(LIMIT_SWITCH_PIN_Y_HOME)) { stepperY.runSpeed(); }
  stepperY.stop();
  stepperY.setCurrentPosition(0);
  stepperY.runToNewPosition(200);
  stepperY.setCurrentPosition(0);

  Serial.println("5. Measuring Y...");
  stepperY.setSpeed(calibSpeed);
  while (!isPressed(LIMIT_SWITCH_PIN_Y_END)) { stepperY.runSpeed(); }
  stepperY.stop();
  maxStepsY = stepperY.currentPosition();
  stepperY.runToNewPosition(0);  // กลับบ้าน

  isCalibrated = true;

  // ตั้งความเร็วใช้งานจริง
  stepperX.setMaxSpeed(3000);
  stepperX.setAcceleration(1000);
  stepperY.setMaxSpeed(4000);
  stepperY.setAcceleration(1000);

  Serial.println("--- System Ready ---");
}

// =========================================
// 5. Serial Command Handlers
// =========================================
void processSerialCommand(String command) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, command);
  
  if (error) {
    Serial.print("{\"error\":\"Invalid JSON: ");
    Serial.print(error.c_str());
    Serial.println("\"}");
    return;
  }
  
  const char* cmd = doc["cmd"];
  
  if (strcmp(cmd, "tree") == 0) {
    handleTreeCommand(doc);
  } else if (strcmp(cmd, "pump") == 0) {
    handlePumpCommand(doc);
  } else if (strcmp(cmd, "status") == 0) {
    handleStatusCommand();
  } else if (strcmp(cmd, "home") == 0) {
    handleHomeCommand();
  } else if (strcmp(cmd, "recalibrate") == 0) {
    handleRecalibrateCommand();
  } else if (strcmp(cmd, "move") == 0) {
    handleMoveCommand(doc);
  } else {
    Serial.println("{\"error\":\"Unknown command\"}");
  }
}

void handleTreeCommand(JsonDocument& doc) {
  if (!isCalibrated) {
    Serial.println("{\"error\":\"Not calibrated\"}");
    return;
  }
  
  int id = doc["id"];
  if (id >= 1 && id <= 9) {
    currentTargetTree = id;
    long tx = maxStepsX * TREE_POS[id - 1][0];
    long ty = maxStepsY * TREE_POS[id - 1][1];
    stepperX.moveTo(tx);
    stepperY.moveTo(ty);
    Serial.print("{\"status\":\"moving\",\"tree\":");
    Serial.print(id);
    Serial.println("}");
  } else {
    Serial.println("{\"error\":\"Invalid tree ID\"}");
  }
}

void handlePumpCommand(JsonDocument& doc) {
  const char* type = doc["type"];
  
  if (strcmp(type, "water") == 0) {
    digitalWrite(RELAY_WATER_PIN, !digitalRead(RELAY_WATER_PIN));
    bool state = !digitalRead(RELAY_WATER_PIN);
    Serial.print("{\"pump\":\"water\",\"state\":");
    Serial.print(state ? "true" : "false");
    Serial.println("}");
  } else if (strcmp(type, "fert") == 0) {
    digitalWrite(RELAY_FERT_PIN, !digitalRead(RELAY_FERT_PIN));
    bool state = !digitalRead(RELAY_FERT_PIN);
    Serial.print("{\"pump\":\"fert\",\"state\":");
    Serial.print(state ? "true" : "false");
    Serial.println("}");
  } else {
    Serial.println("{\"error\":\"Invalid pump type\"}");
  }
}

void handleStatusCommand() {
  // อ่านค่า Soil Moisture
  soilValues[0] = readSoil(SOIL_PIN_2);
  soilValues[1] = readSoil(SOIL_PIN_5);
  soilValues[2] = readSoil(SOIL_PIN_8);

  bool w = !digitalRead(RELAY_WATER_PIN);
  bool f = !digitalRead(RELAY_FERT_PIN);
  bool run = (stepperX.distanceToGo() != 0 || stepperY.distanceToGo() != 0);

  Serial.print("{\"run\":");
  Serial.print(run ? "true" : "false");
  Serial.print(",\"soil\":[");
  Serial.print(soilValues[0]);
  Serial.print(",");
  Serial.print(soilValues[1]);
  Serial.print(",");
  Serial.print(soilValues[2]);
  Serial.print("],\"pWater\":");
  Serial.print(w ? "true" : "false");
  Serial.print(",\"pFert\":");
  Serial.print(f ? "true" : "false");
  Serial.println("}");
}

void handleHomeCommand() {
  stepperX.moveTo(0);
  stepperY.moveTo(0);
  Serial.println("{\"status\":\"homing\"}");
}

void handleRecalibrateCommand() {
  isCalibrated = false;
  Serial.println("{\"status\":\"calibrating\"}");
  calibrateSystem();
  Serial.println("{\"status\":\"calibrated\"}");
}

void handleMoveCommand(JsonDocument& doc) {
  float revsX = doc["revsX"] | 0.0;
  float revsY = doc["revsY"] | 0.0;
  
  long stepsX = revsX * STEPS_PER_REVOLUTION;
  long stepsY = revsY * STEPS_PER_REVOLUTION;
  
  stepperX.move(stepsX);
  stepperY.move(stepsY);
  
  Serial.print("{\"status\":\"moving\",\"stepsX\":");
  Serial.print(stepsX);
  Serial.print(",\"stepsY\":");
  Serial.print(stepsY);
  Serial.println("}");
}

// =========================================
// 6. Setup & Loop
// =========================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Smart Farm System Starting ===");

  // Limit Switches
  pinMode(LIMIT_SWITCH_PIN_X_HOME, INPUT_PULLUP);
  pinMode(LIMIT_SWITCH_PIN_X_END, INPUT_PULLUP);
  pinMode(LIMIT_SWITCH_PIN_Y_HOME, INPUT_PULLUP);
  pinMode(LIMIT_SWITCH_PIN_Y_END, INPUT_PULLUP);

  // Relay Setup
  pinMode(RELAY_WATER_PIN, OUTPUT);
  pinMode(RELAY_FERT_PIN, OUTPUT);
  digitalWrite(RELAY_WATER_PIN, HIGH);  // OFF
  digitalWrite(RELAY_FERT_PIN, HIGH);   // OFF

  // เริ่มต้น Calibration
  calibrateSystem();
  
  Serial.println("=== System Ready ===");
  Serial.println("Waiting for commands...");
  Serial.println("Format: {\"cmd\":\"command\",\"param\":value}");
}

void loop() {
  // อ่านคำสั่งจาก Serial
  while (Serial.available() > 0) {
    char c = Serial.read();
    
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        processSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }
  
  // รันมอเตอร์
  stepperX.run();
  stepperY.run();
}