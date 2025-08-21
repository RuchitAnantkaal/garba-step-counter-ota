#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Wire.h>
#include <ArduinoJson.h>

// ADXL345 I2C address and registers
#define ADXL345_ADDRESS 0x53
#define ADXL345_POWER_CTL 0x2D
#define ADXL345_DATA_FORMAT 0x31
#define ADXL345_DATAX0 0x32

// Built-in LED pin
#define LED_PIN LED_BUILTIN

// Hardcoded step detection parameters
const float magnitudeThreshold = 30.0;
const float restPosition = 250.0;
const float forwardPosition = 220.0;
const unsigned long stepDelay = 300; // Minimum time between steps (ms)
const unsigned long transmitInterval = 60000; // 1 minute in milliseconds

// Debug mode configuration
const bool DEBUG_MODE = true; // Set to false to disable debug output
unsigned long lastDebugPrint = 0;
const unsigned long debugPrintInterval = 2000; // Print debug info every 2 seconds

// Movement states
enum MovementState {
  STATE_REST,
  STATE_FORWARD
};

// Step counting variables
unsigned long stepCount = 0;
unsigned long lastStepTime = 0;
unsigned long lastTransmitTime = 0;

// Movement detection variables
MovementState currentState = STATE_REST;
float currentX, currentY, currentZ;
float magnitude;
bool hasMovedToForward = false;

// ESP-NOW broadcast address (FF:FF:FF:FF:FF:FF for true broadcast)
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Data structure for ESP-NOW transmission
typedef struct {
  char deviceId[18]; // MAC address as string
  unsigned long stepCount;
  float batteryLevel; // Optional: add battery monitoring later
} StepData;

StepData stepData;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off LED initially
  
  // Initialize I2C
  Wire.begin();
  
  // Initialize ADXL345
  initADXL345();
  
  // Initialize ESP-NOW
  initESPNow();
  
  // Get MAC address and store as device ID
  String macStr = WiFi.macAddress();
  macStr.toCharArray(stepData.deviceId, 18);
  
  Serial.println("Step Counter Transmitter Initialized");
  Serial.print("Device ID (MAC): ");
  Serial.println(stepData.deviceId);
  Serial.print("Debug Mode: ");
  Serial.println(DEBUG_MODE ? "ENABLED" : "DISABLED");
  if (DEBUG_MODE) {
    Serial.println("Debug info will be printed every 2 seconds");
  }
  Serial.println("Starting step detection...");
  
  lastTransmitTime = millis();
  lastDebugPrint = millis();
}

void loop() {
  // Read accelerometer and detect steps
  readADXL345();
  detectStep();
  
  // Debug mode - print data every 2 seconds
  if (DEBUG_MODE && (millis() - lastDebugPrint >= debugPrintInterval)) {
    printDebugInfo();
    lastDebugPrint = millis();
  }
  
  // Check if it's time to transmit
  if (millis() - lastTransmitTime >= transmitInterval) {
    transmitStepData();
    lastTransmitTime = millis();
  }
  
  delay(50); // Small delay for stability
}

void initADXL345() {
  // Set power control register
  Wire.beginTransmission(ADXL345_ADDRESS);
  Wire.write(ADXL345_POWER_CTL);
  Wire.write(0x08); // Enable measurement
  Wire.endTransmission();
  
  // Set data format register
  Wire.beginTransmission(ADXL345_ADDRESS);
  Wire.write(ADXL345_DATA_FORMAT);
  Wire.write(0x08); // Full resolution, +/-2g
  Wire.endTransmission();
  
  delay(100);
}

void readADXL345() {
  Wire.beginTransmission(ADXL345_ADDRESS);
  Wire.write(ADXL345_DATAX0);
  Wire.endTransmission();
  
  Wire.requestFrom(ADXL345_ADDRESS, 6);
  
  if (Wire.available() >= 6) {
    int16_t x = Wire.read() | (Wire.read() << 8);
    int16_t y = Wire.read() | (Wire.read() << 8);
    int16_t z = Wire.read() | (Wire.read() << 8);
    
    // Store raw values
    currentX = x;
    currentY = y;
    currentZ = z;
    
    // Calculate magnitude
    magnitude = sqrt(currentX * currentX + currentY * currentY + currentZ * currentZ);
  }
}

void detectStep() {
  // Check magnitude threshold
  if (magnitude < magnitudeThreshold) {
    return;
  }
  
  // Minimum movement threshold
  if (abs(currentY - restPosition) < 20.0) {
    return;
  }
  
  // Calculate thresholds based on hardcoded positions
  float movementRange = abs(forwardPosition - restPosition); // Should be 30
  float forwardThreshold = movementRange * 0.5; // 15
  float restThreshold = movementRange * 0.3;    // 9
  
  // Ensure minimum thresholds
  forwardThreshold = max(forwardThreshold, 15.0f);
  restThreshold = max(restThreshold, 9.0f);
  
  // Determine current state (forward is less than rest: 220 < 250)
  MovementState newState = STATE_REST;
  if (currentY <= (restPosition - forwardThreshold)) {
    newState = STATE_FORWARD;
  }
  
  // Detect state transitions for step counting
  if (newState != currentState) {
    if (currentState == STATE_REST && newState == STATE_FORWARD) {
      hasMovedToForward = true;
      if (DEBUG_MODE) {
        Serial.println("→ Moved to FORWARD position");
      }
    }
    
    if (currentState == STATE_FORWARD && newState == STATE_REST && hasMovedToForward) {
      // Complete step detected: Rest -> Forward -> Rest
      if (millis() - lastStepTime >= stepDelay) {
        stepCount++;
        lastStepTime = millis();
        
        // Blink LED
        digitalWrite(LED_PIN, LOW);
        delay(100);
        digitalWrite(LED_PIN, HIGH);
        
        Serial.println("🎯 STEP DETECTED!");
        Serial.print("   Total Steps: ");
        Serial.println(stepCount);
        Serial.println("   Pattern: REST→FORWARD→REST completed");
        
        hasMovedToForward = false;
      }
    }
    
    currentState = newState;
  }
}

void initESPNow() {
  // Set device as WiFi station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  // Initialize ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  Serial.println("ESP-NOW initialized successfully");
  
  // Set ESP-NOW role
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  
  // Add broadcast peer for transmitting to all nearby receivers
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
}

void transmitStepData() {
  Serial.println("\n========== ESP-NOW TRANSMISSION ==========");
  
  // Prepare data
  stepData.stepCount = stepCount;
  stepData.batteryLevel = 100.0; // Placeholder for battery monitoring
  
  // Turn on WiFi for transmission
  WiFi.mode(WIFI_STA);
  
  // Send data via ESP-NOW broadcast to all nearby receivers
  uint8_t result = esp_now_send(broadcastAddress, (uint8_t *) &stepData, sizeof(stepData));
  
  if (result == 0) {
    Serial.println("✅ ESP-NOW transmission SUCCESS");
    Serial.print("📤 Device ID: ");
    Serial.println(stepData.deviceId);
    Serial.print("👟 Step Count Sent: ");
    Serial.println(stepData.stepCount);
    Serial.print("🔋 Battery Level: ");
    Serial.println(stepData.batteryLevel);
    Serial.print("📡 Broadcast to all nearby receivers");
    Serial.println();
  } else {
    Serial.println("❌ ESP-NOW transmission FAILED");
    Serial.print("Error Code: ");
    Serial.println(result);
  }
  
  // Turn off WiFi to save power
  WiFi.mode(WIFI_OFF);
  
  Serial.println("🔌 WiFi turned OFF for power saving");
  Serial.println("==========================================\n");
}

void printDebugInfo() {
  Serial.println("\n---------- DEBUG INFO (2s interval) ----------");
  Serial.print("📊 Raw Sensor Data - X: ");
  Serial.print(currentX);
  Serial.print(", Y: ");
  Serial.print(currentY);
  Serial.print(", Z: ");
  Serial.print(currentZ);
  Serial.print(" | Magnitude: ");
  Serial.println(magnitude);
  
  Serial.print("👟 Current Step Count: ");
  Serial.println(stepCount);
  
  Serial.print("🎯 Movement State: ");
  Serial.println(currentState == STATE_REST ? "REST" : "FORWARD");
  
  Serial.print("📈 Y-Axis Analysis:");
  Serial.print(" Current=");
  Serial.print(currentY);
  Serial.print(", Rest=");
  Serial.print(restPosition);
  Serial.print(", Forward=");
  Serial.print(forwardPosition);
  Serial.print(", Diff from Rest=");
  Serial.println(abs(currentY - restPosition));
  
  Serial.print("⏱️  Next transmission in: ");
  unsigned long timeUntilTransmit = transmitInterval - (millis() - lastTransmitTime);
  Serial.print(timeUntilTransmit / 1000);
  Serial.println(" seconds");
  
  Serial.print("⚡ WiFi Status: ");
  Serial.println(WiFi.getMode() == WIFI_OFF ? "OFF (Power Saving)" : "ON");
  
  Serial.println("----------------------------------------------\n");
}