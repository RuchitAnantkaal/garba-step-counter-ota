#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <esp_wifi.h>
#include <Update.h>

// ADXL345 I2C address and registers
#define ADXL345_ADDRESS 0x53
#define ADXL345_POWER_CTL 0x2D
#define ADXL345_DATA_FORMAT 0x31
#define ADXL345_DATAX0 0x32

// Built-in LED pin (ESP32 typically uses GPIO 2)
#define LED_PIN 2

// EEPROM Configuration
#define EEPROM_SIZE 512
#define VERSION_ADDRESS 0
#define VERSION_MAX_LENGTH 10

// WiFi credentials for OTA updates (primary)
const char* otaSSID = "Anantkaal_4G";
const char* otaPassword = "Setupdev@123";

// WiFi credentials for security check (fallback)
const char* securitySSID = "garba";
const char* securityPassword = "12345678";

// WiFi Multi object for power-efficient connection
WiFiMulti wifiMulti;

// Fast WiFi connection timeout
const int wifiConnectionTimeout = 8; // 8 seconds max for multi-WiFi connection

// Deep sleep configuration
const unsigned long PERMANENT_DEEP_SLEEP = 0; // 0 = sleep indefinitely until manual reset
bool securityCheckPassed = false;

// GitHub OTA Configuration
const char* GITHUB_USER = "RuchitAnantkaal";
const char* GITHUB_REPO = "garba-step-counter-ota";
const char* FIRMWARE_VERSION = "V7";
const char* DEVICE_TYPE = "transmitter";

// Current running version (loaded from EEPROM or firmware default)
String currentRunningVersion = "";

// GitHub URLs for version check and firmware download
String versionCheckURL = "https://api.github.com/repos/" + String(GITHUB_USER) + "/" + String(GITHUB_REPO) + "/contents/firmware/Transmitter_ESP32/latest/versions.json";
String firmwareBaseURL = "https://raw.githubusercontent.com/" + String(GITHUB_USER) + "/" + String(GITHUB_REPO) + "/main/firmware/Transmitter_ESP32/latest/build/esp32.esp32.esp32/";

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

// ESP-NOW peer info
esp_now_peer_info_t peerInfo;

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
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status);

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
  Serial.println("    ESP32 Transmitter with GitHub OTA    ");
  Serial.println("========================================");
  Serial.print("Running Version: ");
  Serial.println(currentRunningVersion);
  Serial.print("Device MAC: ");
  Serial.println(WiFi.macAddress());
  
  // Get MAC address and store as device ID
  String macStr = WiFi.macAddress();
  macStr.toCharArray(stepData.deviceId, 18);
  
  // Check for OTA updates first (includes security check)
  checkForOTAUpdate();
  
  // Only proceed if security check passed
  if (!securityCheckPassed) {
    // Device is already in deep sleep if security failed
    return;
  }
  
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
  Serial.println("\n⚡ POWER-OPTIMIZED MULTI-WIFI CHECK ⚡");
  
  // Setup WiFi Multi with both networks
  setupWiFiMulti();
  
  // Try to connect to best available network
  Serial.println("🔍 Connecting to best available network...");
  if (connectToWiFiMulti()) {
    String connectedSSID = WiFi.SSID();
    Serial.println("✅ Connected to: " + connectedSSID);
    Serial.print("📶 Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.print("🌐 IP: ");
    Serial.println(WiFi.localIP());
    
    // Check which network we connected to
    if (connectedSSID == otaSSID) {
      Serial.println("🔄 Connected to OTA network - checking for updates");
      
      // Only check for updates when connected to Anantkaal network
      if (checkGitHubVersion()) {
        if (otaUpdateAvailable) {
          Serial.println("🆕 New version available! Starting update...");
          performOTAUpdate();
          return; // Exit after update (device will restart)
        } else {
          Serial.println("✅ Firmware up to date!");
        }
      } else {
        Serial.println("❌ GitHub check failed");
      }
      
      securityCheckPassed = true; // OTA network also provides security
      
    } else if (connectedSSID == securitySSID) {
      Serial.println("🔒 Connected to security network only");
      Serial.println("⭐ Skipping OTA check (not on OTA network)");
      securityCheckPassed = true;
      delay(500); // Brief security check delay
      
    } else {
      Serial.println("❓ Connected to unknown network");
      securityCheckPassed = false;
    }
    
    // Disconnect WiFi
    WiFi.disconnect();
    Serial.println("📶 WiFi disconnected");
    
  } else {
    Serial.println("❌ No authorized networks available");
    securityCheckPassed = false;
  }
  
  // Check if security failed - go to permanent deep sleep
  if (!securityCheckPassed) {
    Serial.println("\n🚨 SECURITY CHECK FAILED 🚨");
    Serial.println("🔋 Entering PERMANENT deep sleep");
    Serial.println("⚠️ Device will NOT wake up automatically");
    Serial.println("🔴 Manual reset required to wake up");
    Serial.println("💤 Going to sleep now...");
    Serial.flush(); // Ensure all serial output is sent
    
    // Blink LED to indicate security failure
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, LOW);
      delay(100);
      digitalWrite(LED_PIN, HIGH);
      delay(100);
    }
    
    // Enter permanent deep sleep (never wake up automatically)
    esp_deep_sleep_start();
  }
  
  Serial.println("🔓 Security check passed - entering normal operation\n");
  delay(500); // Brief pause before normal operation
}

void setupWiFiMulti() {
  Serial.println("📡 Setting up Multi-WiFi networks...");
  
  // Add networks in priority order (OTA first, then security)
  wifiMulti.addAP(otaSSID, otaPassword);      // Priority 1: OTA network
  wifiMulti.addAP(securitySSID, securityPassword); // Priority 2: Security network
  
  Serial.println("✅ Added OTA network: " + String(otaSSID));
  Serial.println("✅ Added Security network: " + String(securitySSID));
}

bool connectToWiFiMulti() {
  Serial.print("🔗 Multi-WiFi connecting");
  
  unsigned long startTime = millis();
  unsigned long timeout = wifiConnectionTimeout * 1000; // Convert to milliseconds
  
  // Try to connect using WiFi Multi
  while (wifiMulti.run() != WL_CONNECTED) {
    if (millis() - startTime > timeout) {
      Serial.println(" ❌ Timeout");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  
  Serial.println(" ✅");
  return true;
}

bool checkGitHubVersion() {
  WiFiClientSecure client;
  client.setInsecure(); // For GitHub API
  client.setTimeout(10000); // 10 second timeout for speed
  HTTPClient http;
  
  Serial.println("📡 Fast GitHub version check...");
  
  http.begin(client, versionCheckURL);
  http.addHeader("User-Agent", "ESP32-OTA-Client");
  http.setTimeout(10000); // 10 second timeout
  
  int httpCode = http.GET();
  
  if (httpCode != 200) {
    Serial.print("❌ HTTP Error: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }
  
  String payload = http.getString();
  http.end();
  
  // Fast JSON parsing
  DynamicJsonDocument doc(1024); // Smaller buffer for speed
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.print("❌ JSON Error: ");
    Serial.println(error.c_str());
    return false;
  }
  
  // Quick base64 decode and version check
  String encodedContent = doc["content"];
  String decodedContent = base64Decode(encodedContent);
  
  DynamicJsonDocument versionDoc(512); // Smaller buffer
  error = deserializeJson(versionDoc, decodedContent);
  
  if (error) {
    Serial.print("❌ Version Error: ");
    Serial.println(error.c_str());
    return false;
  }
  
  String latestVersion = versionDoc["version"];
  latestVersionFromGitHub = latestVersion;
  
  Serial.print("📦 Current: ");
  Serial.print(currentRunningVersion);
  Serial.print(" | GitHub: ");
  Serial.println(latestVersion);
  
  // Quick version comparison
  if (latestVersion != currentRunningVersion) {
    otaUpdateAvailable = true;
    Serial.println("🆕 Update available!");
    return true;
  } else {
    otaUpdateAvailable = false;
    Serial.println("✅ Up to date!");
    return true;
  }
}

void performOTAUpdate() {
  Serial.println("\n⚡ FAST OTA UPDATE ⚡");
  
  // Construct dynamic firmware download URL
  String firmwareURL = firmwareBaseURL + "Transmitter_ESP32_" + latestVersionFromGitHub + ".ino.bin";
  Serial.println("🔗 URL: " + firmwareURL);
  
  // Quick LED blink
  digitalWrite(LED_PIN, LOW);
  delay(100);
  digitalWrite(LED_PIN, HIGH);
  
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30000); // 30 second timeout for download
  
  Serial.println("📥 Fast download starting...");
  
  HTTPClient http;
  http.begin(client, firmwareURL);
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    int contentLength = http.getSize();
    if (contentLength > 0) {
      bool canBegin = Update.begin(contentLength);
      if (canBegin) {
        Serial.println("🚀 Fast Update Started!");
        
        WiFiClient * stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);
        
        if (written == contentLength) {
          Serial.println("✅ Fast Update Complete!");
          saveCurrentVersion(latestVersionFromGitHub);
        } else {
          Serial.println("❌ Update size mismatch");
        }
        
        if (Update.end()) {
          if (Update.isFinished()) {
            Serial.println("🔄 Update finished, restarting...");
            ESP.restart();
          } else {
            Serial.println("❌ Update not finished");
          }
        } else {
          Serial.println("❌ Update error");
        }
      } else {
        Serial.println("❌ Not enough space to begin OTA");
      }
    } else {
      Serial.println("❌ No content length");
    }
  } else {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
  }
  
  http.end();
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
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  Serial.println("ESP-NOW initialized successfully");
  
  // Register send callback
  esp_now_register_send_cb(OnDataSent);
  
  // Add broadcast peer
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
}

void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // Optional: Handle send status
  if (DEBUG_MODE) {
    Serial.print("Last Packet Send Status: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
  }
}

void transmitStepData() {
  Serial.println("\n========== ESP-NOW TRANSMISSION ==========");
  
  // Prepare data
  stepData.stepCount = stepCount;
  stepData.batteryLevel = 100.0; // Placeholder for battery monitoring
  
  // Turn on WiFi for transmission
  WiFi.mode(WIFI_STA);
  
  // Send data via ESP-NOW broadcast to all nearby receivers
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &stepData, sizeof(stepData));
  
  if (result == ESP_OK) {
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
  
  Serial.print("⏱️ Next transmission in: ");
  unsigned long timeUntilTransmit = transmitInterval - (millis() - lastTransmitTime);
  Serial.print(timeUntilTransmit / 1000);
  Serial.println(" seconds");
  
  Serial.print("⚡ WiFi Status: ");
  Serial.println(WiFi.getMode() == WIFI_OFF ? "OFF (Power Saving)" : "ON");
  
  Serial.println("----------------------------------------------\n");
}