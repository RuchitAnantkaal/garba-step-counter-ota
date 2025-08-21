#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <espnow.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// ADXL345 I2C address and registers
#define ADXL345_ADDRESS 0x53
#define ADXL345_POWER_CTL 0x2D
#define ADXL345_DATA_FORMAT 0x31
#define ADXL345_DATAX0 0x32

// Built-in LED pin
#define LED_PIN LED_BUILTIN

// EEPROM Configuration
#define EEPROM_SIZE 512
#define VERSION_ADDRESS 0
#define VERSION_MAX_LENGTH 10

// WiFi credentials for OTA updates
const char* updateSSID = "Anantkaal_4G";
const char* updatePassword = "Setupdev@123";

// GitHub OTA Configuration
const char* GITHUB_USER = "RuchitAnantkaal";
const char* GITHUB_REPO = "garba-step-counter-ota";
const char* FIRMWARE_VERSION = "V3";
const char* DEVICE_TYPE = "transmitter";

// Current running version (loaded from EEPROM or firmware default)
String currentRunningVersion = "";

// GitHub URLs for version check and firmware download
String versionCheckURL = "https://api.github.com/repos/" + String(GITHUB_USER) + "/" + String(GITHUB_REPO) + "/contents/firmware/transmitter/latest/versions.json";
String firmwareBaseURL = "https://raw.githubusercontent.com/" + String(GITHUB_USER) + "/" + String(GITHUB_REPO) + "/main/firmware/transmitter/latest/build/esp8266.esp8266.generic/";

// Hardcoded step detection parameters
const float magnitudeThreshold = 30.0;
const float restPosition = 250.0;
const float forwardPosition = 220.0;
const unsigned long stepDelay = 300; // Minimum time between steps (ms)
const unsigned long transmitInterval = 60000; // 1 minute in milliseconds

// Debug mode configuration
const bool DEBUG_MODE = true;
unsigned long lastDebugPrint = 0;
const unsigned long debugPrintInterval = 2000; // Print debug info every 2 seconds

// ESP-NOW broadcast address
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Movement states
enum MovementState {
  STATE_REST,
  STATE_FORWARD
};

// Data structure for ESP-NOW transmission
typedef struct {
  char deviceId[18];
  unsigned long stepCount;
  float batteryLevel;
} StepData;

StepData stepData;

// Step counting variables
unsigned long stepCount = 0;
unsigned long lastStepTime = 0;
unsigned long lastTransmitTime = 0;

// Movement detection variables
MovementState currentState = STATE_REST;
float currentX, currentY, currentZ;
float magnitude;
bool hasMovedToForward = false;

// OTA Update variables
bool otaUpdateAvailable = false;
bool otaUpdateCompleted = false;
String latestVersionFromGitHub = "";

// Function declarations
void loadCurrentVersion();
void saveCurrentVersion(String version);
bool checkGitHubVersion();
void performOTAUpdate();
String base64Decode(String input);
void checkForOTAUpdate();
void initADXL345();
void readADXL345();
void detectStep();
void initESPNow();
void transmitStepData();
void printDebugInfo();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off LED initially
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Load current version from EEPROM or use firmware default
  loadCurrentVersion();
  
  Serial.println("\n========================================");
  Serial.println("    Transmitter with GitHub OTA Test    ");
  Serial.println("========================================");
  Serial.print("Running Version: ");
  Serial.println(currentRunningVersion);
  Serial.print("Device MAC: ");
  Serial.println(WiFi.macAddress());
  
  // Get MAC address and store as device ID
  String macStr = WiFi.macAddress();
  macStr.toCharArray(stepData.deviceId, 18);
  
  // Check for OTA updates first
  checkForOTAUpdate();
  
  // Initialize I2C
  Wire.begin();
  
  // Initialize ADXL345
  initADXL345();
  
  // Initialize ESP-NOW for normal operation
  initESPNow();
  
  Serial.println("========================================");
  Serial.println("Ready for step detection and transmission");
  Serial.print("Debug Mode: ");
  Serial.println(DEBUG_MODE ? "ENABLED" : "DISABLED");
  Serial.println("========================================\n");
  
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

void loadCurrentVersion() {
  Serial.println("📖 Loading version from EEPROM...");
  
  // Read version string from EEPROM
  String storedVersion = "";
  for (int i = 0; i < VERSION_MAX_LENGTH; i++) {
    char c = EEPROM.read(VERSION_ADDRESS + i);
    if (c == '\0' || c == 0xFF) break; // End of string or uninitialized
    storedVersion += c;
  }
  
  // Check if we have a valid stored version
  if (storedVersion.length() > 0 && storedVersion.length() < VERSION_MAX_LENGTH && storedVersion != "") {
    // We have a valid version in EEPROM - use it
    currentRunningVersion = storedVersion;
    Serial.println("✅ Loaded version from EEPROM: " + currentRunningVersion);
    Serial.println("🔄 Device was previously updated via OTA");
  } else {
    // EEPROM is empty or corrupted - this is first boot with this firmware
    currentRunningVersion = FIRMWARE_VERSION;
    Serial.println("🆕 First boot detected - using firmware version: " + currentRunningVersion);
    Serial.println("💾 Saving firmware version to EEPROM for tracking");
    saveCurrentVersion(currentRunningVersion);
  }
  
  Serial.print("🏃 Device will run as version: ");
  Serial.println(currentRunningVersion);
}

void saveCurrentVersion(String version) {
  Serial.println("💾 Saving version to EEPROM: " + version);
  
  // Clear the version area first
  for (int i = 0; i < VERSION_MAX_LENGTH; i++) {
    EEPROM.write(VERSION_ADDRESS + i, 0);
  }
  
  // Write new version string
  for (int i = 0; i < version.length() && i < VERSION_MAX_LENGTH - 1; i++) {
    EEPROM.write(VERSION_ADDRESS + i, version[i]);
  }
  
  // Ensure null termination
  EEPROM.write(VERSION_ADDRESS + version.length(), '\0');
  
  // Commit to EEPROM
  EEPROM.commit();
  
  // Update running version
  currentRunningVersion = version;
  
  Serial.println("✅ Version saved successfully");
}

void checkForOTAUpdate() {
  Serial.println("\n🔍 Checking for OTA updates...");
  
  // Connect to WiFi for update check
  WiFi.begin(updateSSID, updatePassword);
  Serial.print("Connecting to WiFi for OTA check");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ WiFi connection failed. Skipping OTA check.");
    WiFi.disconnect();
    return;
  }
  
  Serial.println("\n✅ WiFi connected for OTA check");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  
  // Check for new version
  if (checkGitHubVersion()) {
    if (otaUpdateAvailable) {
      Serial.println("🔄 New version available! Starting update...");
      performOTAUpdate();
    } else {
      Serial.println("✅ Device is up to date!");
    }
  } else {
    Serial.println("❌ Failed to check for updates");
  }
  
  // Disconnect WiFi after update check
  WiFi.disconnect();
  Serial.println("📶 WiFi disconnected. Returning to normal operation.\n");
  delay(1000);
}

bool checkGitHubVersion() {
  WiFiClientSecure client;
  client.setInsecure(); // For GitHub API
  HTTPClient http;
  
  Serial.println("📡 Checking GitHub for latest version...");
  Serial.println("URL: " + versionCheckURL);
  
  http.begin(client, versionCheckURL);
  http.addHeader("User-Agent", "ESP8266-OTA-Client");
  
  int httpCode = http.GET();
  
  if (httpCode != 200) {
    Serial.print("❌ HTTP Error: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }
  
  String payload = http.getString();
  http.end();
  
  // Parse GitHub API response
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.print("❌ JSON Parse Error: ");
    Serial.println(error.c_str());
    return false;
  }
  
  // Decode base64 content
  String encodedContent = doc["content"];
  String decodedContent = base64Decode(encodedContent);
  
  // Parse version file content
  DynamicJsonDocument versionDoc(1024);
  error = deserializeJson(versionDoc, decodedContent);
  
  if (error) {
    Serial.print("❌ Version JSON Parse Error: ");
    Serial.println(error.c_str());
    return false;
  }
  
  String latestVersion = versionDoc["version"];
  latestVersionFromGitHub = latestVersion; // Store for later use
  
  Serial.print("📦 Running Version: ");
  Serial.println(currentRunningVersion);
  Serial.print("📦 Latest GitHub Version: ");
  Serial.println(latestVersion);
  
  // Compare versions
  if (latestVersion != currentRunningVersion) {
    otaUpdateAvailable = true;
    Serial.println("🆕 New version available!");
    return true;
  } else {
    otaUpdateAvailable = false;
    Serial.println("✅ Already up to date!");
    return true;
  }
}

void performOTAUpdate() {
  Serial.println("\n🔄 Starting OTA update process...");
  
  // Construct dynamic firmware download URL based on latest version
  String firmwareURL = firmwareBaseURL + "Transmitter_" + latestVersionFromGitHub + ".ino.bin";
  Serial.println("🔗 Firmware URL: " + firmwareURL);
  Serial.println("📦 Downloading version: " + latestVersionFromGitHub);
  
  // Blink LED during update
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(200);
    digitalWrite(LED_PIN, HIGH);
    delay(200);
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  
  Serial.println("📥 Starting firmware download...");
  
  // Configure HTTP update
  ESPhttpUpdate.setLedPin(LED_PIN, LOW);
  ESPhttpUpdate.onStart([]() {
    Serial.println("🚀 OTA Update Started!");
  });
  
  ESPhttpUpdate.onEnd([]() {
    Serial.println("✅ OTA Update Completed!");
    // Update version in EEPROM after successful update
    saveCurrentVersion(latestVersionFromGitHub);
    Serial.println("💾 Version updated in EEPROM: " + latestVersionFromGitHub);
  });
  
  ESPhttpUpdate.onProgress([](int cur, int total) {
    Serial.printf("📊 Progress: %u%%\n", (cur * 100) / total);
  });
  
  ESPhttpUpdate.onError([](int error) {
    Serial.printf("❌ Update Error[%u]: %s\n", error, ESPhttpUpdate.getLastErrorString().c_str());
  });
  
  // Perform the update
  t_httpUpdate_return result = ESPhttpUpdate.update(client, firmwareURL);
  
  switch (result) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("❌ Update Failed: Error[%u]: %s\n", 
                   ESPhttpUpdate.getLastError(), 
                   ESPhttpUpdate.getLastErrorString().c_str());
      break;
      
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("ℹ️  No updates available");
      break;
      
    case HTTP_UPDATE_OK:
      Serial.println("✅ Update completed! Device will restart...");
      // Note: Version is already saved in onEnd callback
      otaUpdateCompleted = true;
      break;
      
    default:
      Serial.println("❓ Unknown update result");
      break;
  }
}

String base64Decode(String input) {
  // Simple base64 decoder for GitHub API content
  // Remove newlines and spaces
  input.replace("\n", "");
  input.replace("\r", "");
  input.replace(" ", "");
  
  const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String result = "";
  
  for (int i = 0; i < input.length(); i += 4) {
    uint32_t sextet_a = input[i] == '=' ? 0 & i++ : strchr(chars, input[i]) - chars;
    uint32_t sextet_b = input[i + 1] == '=' ? 0 & i++ : strchr(chars, input[i + 1]) - chars;
    uint32_t sextet_c = input[i + 2] == '=' ? 0 & i++ : strchr(chars, input[i + 2]) - chars;
    uint32_t sextet_d = input[i + 3] == '=' ? 0 & i++ : strchr(chars, input[i + 3]) - chars;
    
    uint32_t triple = (sextet_a << 3 * 6) + (sextet_b << 2 * 6) + (sextet_c << 1 * 6) + (sextet_d << 0 * 6);
    
    if (i + 1 < input.length()) result += char((triple >> 2 * 8) & 0xFF);
    if (i + 2 < input.length()) result += char((triple >> 1 * 8) & 0xFF);
    if (i + 3 < input.length()) result += char((triple >> 0 * 8) & 0xFF);
  }
  
  return result;
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
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
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