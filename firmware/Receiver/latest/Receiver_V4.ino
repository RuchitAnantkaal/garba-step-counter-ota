#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <espnow.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <map>

// Built-in LED pin
#define LED_PIN LED_BUILTIN

// EEPROM Configuration
#define EEPROM_SIZE 512
#define VERSION_ADDRESS 0
#define VERSION_MAX_LENGTH 10

// WiFi credentials
const char* ssid = "Anantkaal_4G";
const char* password = "Setupdev@123";

// GitHub OTA Configuration
const char* GITHUB_USER = "RuchitAnantkaal";
const char* GITHUB_REPO = "garba-step-counter-ota";
const char* FIRMWARE_VERSION = "V3";
const char* DEVICE_TYPE = "receiver";

// Current running version (loaded from EEPROM or firmware default)
String currentRunningVersion = "";

// GitHub URLs for version check and firmware download
String versionCheckURL = "https://api.github.com/repos/" + String(GITHUB_USER) + "/" + String(GITHUB_REPO) + "/contents/firmware/Receiver/latest/versions.json";
String firmwareBaseURL = "https://raw.githubusercontent.com/" + String(GITHUB_USER) + "/" + String(GITHUB_REPO) + "/main/firmware/Receiver/latest/build/esp8266.esp8266.generic/";

// Central server configuration
const char* serverHost = "garba.local";
const char* serverEndpoint = "/api/receiver-data";

// HTTP client
WiFiClient client;
HTTPClient http;

// Data structure for receiving ESP-NOW data
typedef struct {
  char deviceId[18];
  unsigned long stepCount;
  float batteryLevel;
} StepData;

// Device tracking - store individual device data
std::map<String, unsigned long> deviceSteps;
std::map<String, float> deviceBattery;
std::map<String, unsigned long> deviceLastSeen;
String receiverId;
unsigned long lastServerUpdate = 0;
const unsigned long serverUpdateInterval = 1000; // Send to server every 1 second

// Status tracking
unsigned long lastStatusPrint = 0;
const unsigned long statusPrintInterval = 10000; // Print status every 10 seconds

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
void connectToWiFi();
void initESPNow();
void onDataReceived(uint8_t *mac, uint8_t *incomingData, uint8_t len);
void sendDataToServer();
void printStatus();
void cleanupOldDevices();

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
  Serial.println("     Receiver with GitHub OTA Test     ");
  Serial.println("========================================");
  Serial.print("Firmware Version: ");
  Serial.println(FIRMWARE_VERSION);
  Serial.print("Running Version: ");
  Serial.println(currentRunningVersion);
  
  // Get receiver ID from MAC address
  receiverId = WiFi.macAddress();
  Serial.print("Receiver ID: ");
  Serial.println(receiverId);
  
  // Check for OTA updates first (before normal operation)
  checkForOTAUpdate();
  
  // Initialize ESP-NOW
  initESPNow();
  
  // Connect to WiFi for normal operation
  connectToWiFi();
  
  Serial.println("========================================");
  Serial.println("Ready to receive ESP-NOW data and forward to server");
  Serial.print("Server: ");
  Serial.println(serverHost);
  Serial.println("========================================\n");
}

void loop() {
  // Send data to server every 1 second
  if (millis() - lastServerUpdate >= serverUpdateInterval) {
    sendDataToServer();
    lastServerUpdate = millis();
  }
  
  // Print status every 10 seconds
  if (millis() - lastStatusPrint >= statusPrintInterval) {
    printStatus();
    lastStatusPrint = millis();
  }
  
  // Clean up old devices (not seen for 5 minutes)
  cleanupOldDevices();
  
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected! Attempting reconnect...");
    connectToWiFi();
  }
  
  delay(100);
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
    Serial.println("🔄 Receiver was previously updated via OTA");
  } else {
    // EEPROM is empty or corrupted - this is first boot with this firmware
    currentRunningVersion = FIRMWARE_VERSION;
    Serial.println("🆕 First boot detected - using firmware version: " + currentRunningVersion);
    Serial.println("💾 Saving firmware version to EEPROM for tracking");
    saveCurrentVersion(currentRunningVersion);
  }
  
  Serial.print("🏃 Receiver will run as version: ");
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
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi for OTA check");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ WiFi connection failed. Skipping OTA check.");
    Serial.println("📶 Continuing with normal operation...");
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
      Serial.println("✅ Receiver is up to date!");
    }
  } else {
    Serial.println("❌ Failed to check for updates");
  }
  
  Serial.println("📶 OTA check completed. WiFi stays connected for normal operation.\n");
  delay(1000);
}

bool checkGitHubVersion() {
  WiFiClientSecure client;
  client.setInsecure(); // For GitHub API
  HTTPClient http;
  
  Serial.println("📡 Checking GitHub for latest receiver version...");
  Serial.println("URL: " + versionCheckURL);
  
  http.begin(client, versionCheckURL);
  http.addHeader("User-Agent", "ESP8266-Receiver-OTA-Client");
  
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
    Serial.println("🆕 New version available for receiver!");
    return true;
  } else {
    otaUpdateAvailable = false;
    Serial.println("✅ Receiver already up to date!");
    return true;
  }
}

void performOTAUpdate() {
  Serial.println("\n🔄 Starting Receiver OTA update process...");
  
  // Construct dynamic firmware download URL based on latest version
  String firmwareURL = firmwareBaseURL + "Receiver_" + latestVersionFromGitHub + ".ino.bin";
  Serial.println("🔗 Firmware URL: " + firmwareURL);
  Serial.println("📦 Downloading receiver version: " + latestVersionFromGitHub);
  
  // Blink LED during update
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(200);
    digitalWrite(LED_PIN, HIGH);
    delay(200);
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  
  Serial.println("📥 Starting receiver firmware download...");
  
  // Configure HTTP update
  ESPhttpUpdate.setLedPin(LED_PIN, LOW);
  ESPhttpUpdate.onStart([]() {
    Serial.println("🚀 Receiver OTA Update Started!");
  });
  
  ESPhttpUpdate.onEnd([]() {
    Serial.println("✅ Receiver OTA Update Completed!");
    Serial.println("💾 Saving new receiver version to EEPROM: " + latestVersionFromGitHub);
    // Update version in EEPROM after successful update
    saveCurrentVersion(latestVersionFromGitHub);
    Serial.println("🔄 Next boot will use receiver version: " + latestVersionFromGitHub);
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
      Serial.printf("❌ Receiver Update Failed: Error[%u]: %s\n", 
                   ESPhttpUpdate.getLastError(), 
                   ESPhttpUpdate.getLastErrorString().c_str());
      break;
      
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("ℹ️  No receiver updates available");
      break;
      
    case HTTP_UPDATE_OK:
      Serial.println("✅ Receiver update completed! Device will restart...");
      // Note: Version is already saved in onEnd callback
      otaUpdateCompleted = true;
      break;
      
    default:
      Serial.println("❓ Unknown receiver update result");
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

void initESPNow() {
  WiFi.mode(WIFI_STA);
  
  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(onDataReceived);
  Serial.println("ESP-NOW initialized successfully");
}

void onDataReceived(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  StepData receivedData;
  memcpy(&receivedData, incomingData, sizeof(receivedData));
  
  String deviceId = String(receivedData.deviceId);
  unsigned long newStepCount = receivedData.stepCount;
  float batteryLevel = receivedData.batteryLevel;
  
  // Update device data
  deviceSteps[deviceId] = newStepCount;
  deviceBattery[deviceId] = batteryLevel;
  deviceLastSeen[deviceId] = millis();
  
  Serial.println("📡 ESP-NOW Data Received:");
  Serial.print("  Device: ");
  Serial.println(deviceId.substring(12)); // Last 6 chars
  Serial.print("  Steps: ");
  Serial.println(newStepCount);
  Serial.print("  Battery: ");
  Serial.print(batteryLevel);
  Serial.println("%");
}

void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return; // Already connected
  }
  
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi for normal operation");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" Failed!");
    delay(5000); // Wait before retry
  }
}

void sendDataToServer() {
  if (WiFi.status() != WL_CONNECTED || deviceSteps.empty()) {
    return;
  }
  
  // Create JSON payload with all device data and receiver priority
  DynamicJsonDocument doc(2048); // Larger buffer for multiple devices
  doc["receiverId"] = receiverId;
  doc["timestamp"] = millis();
  doc["deviceCount"] = deviceSteps.size();
  doc["receiverPriority"] = WiFi.RSSI(); // Use signal strength as priority indicator
  
  JsonArray devices = doc.createNestedArray("devices");
  
  for (auto& pair : deviceSteps) {
    JsonObject device = devices.createNestedObject();
    device["deviceId"] = pair.first;
    device["stepCount"] = pair.second;
    device["batteryLevel"] = deviceBattery[pair.first];
    device["lastSeen"] = (millis() - deviceLastSeen[pair.first]) / 1000; // seconds ago
    device["signalStrength"] = -50; // Placeholder - could implement RSSI tracking
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  // Send HTTP POST to server
  http.begin(client, "http://" + String(serverHost) + serverEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000); // 5 second timeout
  
  int httpResponseCode = http.POST(jsonString);
  
  if (httpResponseCode > 0) {
    if (httpResponseCode == 200) {
      // Success - only print occasionally to avoid spam
      static unsigned long lastSuccessLog = 0;
      if (millis() - lastSuccessLog > 30000) { // Log success every 30 seconds
        Serial.println("✅ Data sent to server successfully");
        lastSuccessLog = millis();
      }
    } else {
      Serial.print("⚠️  Server response code: ");
      Serial.println(httpResponseCode);
    }
  } else {
    Serial.print("❌ Error sending to server: ");
    Serial.println(httpResponseCode);
  }
  
  http.end();
}

void printStatus() {
  Serial.println("\n========== RECEIVER STATUS ==========");
  Serial.print("🆔 Receiver ID: ");
  Serial.println(receiverId.substring(12)); // Last 6 chars
  Serial.print("📱 Connected Devices: ");
  Serial.println(deviceSteps.size());
  Serial.print("🌐 WiFi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  Serial.print("📊 Total Steps Managed: ");
  
  unsigned long totalSteps = 0;
  for (auto& pair : deviceSteps) {
    totalSteps += pair.second;
  }
  Serial.println(totalSteps);
  
  if (deviceSteps.size() > 0) {
    Serial.println("📋 Device Summary:");
    for (auto& pair : deviceSteps) {
      Serial.print("   ");
      Serial.print(pair.first.substring(12));
      Serial.print(": ");
      Serial.print(pair.second);
      Serial.print(" steps, ");
      Serial.print(deviceBattery[pair.first]);
      Serial.println("% battery");
    }
  }
  Serial.println("====================================\n");
}

void cleanupOldDevices() {
  const unsigned long maxAge = 300000; // 5 minutes
  
  for (auto it = deviceLastSeen.begin(); it != deviceLastSeen.end();) {
    if (millis() - it->second > maxAge) {
      String deviceId = it->first;
      Serial.print("🧹 Removing old device: ");
      Serial.println(deviceId.substring(12));
      
      deviceSteps.erase(deviceId);
      deviceBattery.erase(deviceId);
      it = deviceLastSeen.erase(it);
    } else {
      ++it;
    }
  }
}