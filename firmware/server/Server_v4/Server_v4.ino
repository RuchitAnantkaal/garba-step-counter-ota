#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <map>
#include <set>

// EEPROM Configuration
#define EEPROM_SIZE 512
#define VERSION_ADDRESS 0
#define VERSION_MAX_LENGTH 10

// WiFi credentials
const char* primarySSID = "Anantkaal_4G";
const char* primaryPassword = "Setupdev@123";

// GitHub OTA Configuration
const char* GITHUB_USER = "RuchitAnantkaal";
const char* GITHUB_REPO = "garba-step-counter-ota";
const char* FIRMWARE_VERSION = "v3";
const char* DEVICE_TYPE = "server";

// Current running version (loaded from EEPROM or firmware default)
String currentRunningVersion = "";

// WiFi Multi object for power-efficient connection
WiFiMulti wifiMulti;

// Fast WiFi connection timeout
const int wifiConnectionTimeout = 10; // 10 seconds max for WiFi connection

// GitHub URLs for version check and firmware download
String versionCheckURL = "https://api.github.com/repos/" + String(GITHUB_USER) + "/" + String(GITHUB_REPO) + "/contents/firmware/server/latest/versions.json";
String firmwareBaseURL = "https://raw.githubusercontent.com/" + String(GITHUB_USER) + "/" + String(GITHUB_REPO) + "/main/firmware/server/latest/build/esp32.esp32.esp32/";

// Web server
WebServer server(80);

// Data structures for tracking all devices across all receivers
struct DeviceInfo {
  unsigned long stepCount;
  float batteryLevel;
  String bestReceiverId;
  unsigned long lastSeen;
  int signalStrength;
};

struct ReceiverData {
  unsigned long stepCount;
  float batteryLevel;
  unsigned long timestamp;
  int signalStrength;
};

// Global Data Storage
std::map<String, DeviceInfo> devices;                                    // deviceId -> best device info
std::map<String, std::map<String, ReceiverData>> deviceReceiverMap;     // deviceId -> receiverId -> data
std::map<String, unsigned long> receiverLastSeen;                       // receiverId -> last seen time
std::map<String, int> receiverDeviceCount;                              // receiverId -> device count
std::map<String, unsigned long> receiverStepSum;                        // receiverId -> total steps

// Statistics
unsigned long totalSteps = 0;
unsigned long totalDevices = 0;
unsigned long activeReceivers = 0;

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
void setupWiFiMulti();
bool connectToWiFiMulti();
void connectToWiFi();
void setupWebServer();
void handleReceiverData();
void updateBestDeviceData(const String& deviceId);
void updateStatistics();
void cleanupOldData();
void printSystemStatus();
void handleMainDashboard();
void handleDebugConsole();
void handleDashboardData();
void handleReceiversData();
void handleDebugData();
void handleReset();
void resetAllData();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Load current version from EEPROM or use firmware default
  loadCurrentVersion();
  
  Serial.println("\n========================================");
  Serial.println("     ESP32 Server with GitHub OTA      ");
  Serial.println("========================================");
  Serial.print("Firmware Version: ");
  Serial.println(FIRMWARE_VERSION);
  Serial.print("Running Version: ");
  Serial.println(currentRunningVersion);
  Serial.print("Device MAC: ");
  Serial.println(WiFi.macAddress());
  
  // Check for OTA updates first (no security check needed for server)
  checkForOTAUpdate();
  
  // Start mDNS
  if (MDNS.begin("garba")) {
    Serial.println("mDNS started successfully");
    MDNS.addService("http", "tcp", 80);
  }
  
  // Setup web server
  setupWebServer();
  
  Serial.println("========================================");
  Serial.println("    Garba Step Counter Server Started    ");
  Serial.println("========================================");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.println("Main Dashboard: http://garba.local/stepcounter");
  Serial.println("Debug Console:  http://garba.local/debugger");
  Serial.println("Ready to receive data from receivers...");
  Serial.println("========================================\n");
}

void loop() {
  server.handleClient();
  
  // Update statistics every 5 seconds
  static unsigned long lastStatsUpdate = 0;
  if (millis() - lastStatsUpdate >= 5000) {
    updateStatistics();
    lastStatsUpdate = millis();
  }
  
  // Cleanup old data every 30 seconds
  static unsigned long lastCleanup = 0;
  if (millis() - lastCleanup >= 30000) {
    cleanupOldData();
    lastCleanup = millis();
  }
  
  // Print status every minute
  static unsigned long lastStatusPrint = 0;
  if (millis() - lastStatusPrint >= 60000) {
    printSystemStatus();
    lastStatusPrint = millis();
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
    Serial.println("🔄 Server was previously updated via OTA");
  } else {
    // EEPROM is empty or corrupted - this is first boot with this firmware
    currentRunningVersion = FIRMWARE_VERSION;
    Serial.println("🆕 First boot detected - using firmware version: " + currentRunningVersion);
    Serial.println("💾 Saving firmware version to EEPROM for tracking");
    saveCurrentVersion(currentRunningVersion);
  }
  
  Serial.print("🏃 Server will run as version: ");
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
  Serial.println("\n⚡ SERVER OTA CHECK ⚡");
  
  // Setup WiFi Multi
  setupWiFiMulti();
  
  // Try to connect to WiFi
  Serial.println("🔍 Connecting to WiFi for OTA check...");
  if (connectToWiFiMulti()) {
    String connectedSSID = WiFi.SSID();
    Serial.println("✅ Connected to: " + connectedSSID);
    Serial.print("📶 Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.print("🌐 IP: ");
    Serial.println(WiFi.localIP());
    
    // Check for server updates
    Serial.println("🔄 Checking for server updates...");
    if (checkGitHubVersion()) {
      if (otaUpdateAvailable) {
        Serial.println("🆕 New server version available! Starting update...");
        performOTAUpdate();
        return; // Exit after update (device will restart)
      } else {
        Serial.println("✅ Server firmware up to date!");
      }
    } else {
      Serial.println("❌ GitHub check failed");
    }
    
  } else {
    Serial.println("❌ WiFi connection failed - continuing without OTA check");
    Serial.println("⚠️  Server will run with current firmware");
  }
  
  Serial.println("⚡ Server OTA check completed\n");
  delay(500);
}

void setupWiFiMulti() {
  Serial.println("📡 Setting up WiFi...");
  
  // Add primary network
  wifiMulti.addAP(primarySSID, primaryPassword);
  Serial.println("✅ Added network: " + String(primarySSID));
}

bool connectToWiFiMulti() {
  Serial.print("🔗 WiFi connecting");
  
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
  client.setTimeout(15000); // 15 second timeout
  HTTPClient http;
  
  Serial.println("📡 Checking GitHub for latest server version...");
  
  http.begin(client, versionCheckURL);
  http.addHeader("User-Agent", "ESP32-Server-OTA-Client");
  http.setTimeout(15000); // 15 second timeout
  
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
    Serial.print("❌ JSON Error: ");
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
  
  // Compare versions
  if (latestVersion != currentRunningVersion) {
    otaUpdateAvailable = true;
    Serial.println("🆕 Server update available!");
    return true;
  } else {
    otaUpdateAvailable = false;
    Serial.println("✅ Server up to date!");
    return true;
  }
}

void performOTAUpdate() {
  Serial.println("\n⚡ SERVER OTA UPDATE ⚡");
  
  // Construct dynamic firmware download URL
  String firmwareURL = firmwareBaseURL + "Server_" + latestVersionFromGitHub + ".ino.bin";
  Serial.println("🔗 URL: " + firmwareURL);
  
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(60000); // 60 second timeout for server download
  
  Serial.println("📥 Server firmware download starting...");
  
  // Configure HTTP update
  httpUpdate.rebootOnUpdate(true); // Auto reboot
  
  httpUpdate.onStart([]() {
    Serial.println("🚀 Server Update Started!");
  });
  
  httpUpdate.onEnd([]() {
    Serial.println("✅ Server Update Complete!");
    saveCurrentVersion(latestVersionFromGitHub);
  });
  
  httpUpdate.onProgress([](int cur, int total) {
    // Progress output every 10%
    static int lastPercent = -1;
    int percent = (cur * 100) / total;
    if (percent != lastPercent && percent % 10 == 0) {
      Serial.printf("📊 %u%%\n", percent);
      lastPercent = percent;
    }
  });
  
  httpUpdate.onError([](int error) {
    Serial.printf("❌ Error[%u]: %s\n", error, httpUpdate.getLastErrorString().c_str());
  });
  
  // Perform update
  t_httpUpdate_return result = httpUpdate.update(client, firmwareURL);
  
  switch (result) {
    case HTTP_UPDATE_FAILED:
      Serial.println("❌ Server Update Failed");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("ℹ️  No Server Updates");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("✅ Server Update OK - Restarting...");
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

void setupWebServer() {
  // Main interfaces
  server.on("/stepcounter", handleMainDashboard);
  server.on("/debugger", handleDebugConsole);
  server.on("/", handleMainDashboard);
  
  // API endpoints
  server.on("/api/receiver-data", HTTP_POST, handleReceiverData);
  server.on("/api/dashboard-data", HTTP_GET, handleDashboardData);
  server.on("/api/debug-data", HTTP_GET, handleDebugData);
  server.on("/api/reset", HTTP_POST, handleReset);
  
  server.enableCORS(true);
  server.begin();
  Serial.println("Web server started on port 80");
}

void handleReceiverData() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  // Extract receiver info
  String receiverId = doc["receiverId"].as<String>();
  unsigned long timestamp = doc["timestamp"];
  int deviceCount = doc["deviceCount"];
  int receiverPriority = doc["receiverPriority"].as<int>();
  
  // Update receiver status
  receiverLastSeen[receiverId] = millis();
  
  // Process all devices from this receiver
  JsonArray devices_array = doc["devices"];
  for (JsonObject device : devices_array) {
    String deviceId = device["deviceId"].as<String>();
    unsigned long stepCount = device["stepCount"];
    float batteryLevel = device["batteryLevel"];
    unsigned long lastSeen = device["lastSeen"];
    int signalStrength = device["signalStrength"].as<int>();
    
    // Store data for this receiver-device combination
    ReceiverData receiverData;
    receiverData.stepCount = stepCount;
    receiverData.batteryLevel = batteryLevel;
    receiverData.timestamp = millis() - (lastSeen * 1000);
    receiverData.signalStrength = signalStrength;
    
    deviceReceiverMap[deviceId][receiverId] = receiverData;
    
    // Update best device data
    updateBestDeviceData(deviceId);
  }
  
  // Update receiver statistics
  receiverDeviceCount[receiverId] = deviceCount;
  
  server.send(200, "text/plain", "Data processed successfully");
}

void updateBestDeviceData(const String& deviceId) {
  if (deviceReceiverMap[deviceId].empty()) return;
  
  String bestReceiver = "";
  unsigned long latestTime = 0;
  int bestSignal = -200;
  
  // Find receiver with most recent data and best signal
  for (auto& receiverPair : deviceReceiverMap[deviceId]) {
    String receiverId = receiverPair.first;
    ReceiverData& data = receiverPair.second;
    
    bool isBetter = false;
    
    // Prefer more recent data
    if (data.timestamp > latestTime) {
      isBetter = true;
    }
    // If timestamps are close (within 10 seconds), prefer better signal
    else if (abs((long)(data.timestamp - latestTime)) < 10000) {
      if (data.signalStrength > bestSignal) {
        isBetter = true;
      }
    }
    
    if (isBetter) {
      bestReceiver = receiverId;
      latestTime = data.timestamp;
      bestSignal = data.signalStrength;
    }
  }
  
  // Update main device record with best data
  if (!bestReceiver.isEmpty()) {
    ReceiverData& bestData = deviceReceiverMap[deviceId][bestReceiver];
    
    DeviceInfo& deviceInfo = devices[deviceId];
    deviceInfo.stepCount = bestData.stepCount;
    deviceInfo.batteryLevel = bestData.batteryLevel;
    deviceInfo.bestReceiverId = bestReceiver;
    deviceInfo.lastSeen = bestData.timestamp;
    deviceInfo.signalStrength = bestData.signalStrength;
  }
}

void updateStatistics() {
  // Calculate total steps and devices
  totalSteps = 0;
  totalDevices = devices.size();
  
  for (auto& devicePair : devices) {
    totalSteps += devicePair.second.stepCount;
  }
  
  // Count active receivers (seen in last 30 seconds)
  activeReceivers = 0;
  unsigned long currentTime = millis();
  for (auto& receiverPair : receiverLastSeen) {
    if (currentTime - receiverPair.second < 30000) {
      activeReceivers++;
    }
  }
}

void cleanupOldData() {
  unsigned long currentTime = millis();
  const unsigned long deviceTimeout = 300000;  // 5 minutes
  const unsigned long receiverTimeout = 60000; // 1 minute
  
  // Clean up old devices
  for (auto it = devices.begin(); it != devices.end();) {
    if (currentTime - it->second.lastSeen > deviceTimeout) {
      String deviceId = it->first;
      deviceReceiverMap.erase(deviceId);
      it = devices.erase(it);
    } else {
      ++it;
    }
  }
  
  // Clean up old receiver data
  for (auto& devicePair : deviceReceiverMap) {
    for (auto it = devicePair.second.begin(); it != devicePair.second.end();) {
      if (currentTime - it->second.timestamp > deviceTimeout) {
        it = devicePair.second.erase(it);
      } else {
        ++it;
      }
    }
  }
  
  // Clean up old receivers
  for (auto it = receiverLastSeen.begin(); it != receiverLastSeen.end();) {
    if (currentTime - it->second > receiverTimeout) {
      String receiverId = it->first;
      receiverDeviceCount.erase(receiverId);
      it = receiverLastSeen.erase(it);
    } else {
      ++it;
    }
  }
}

void printSystemStatus() {
  Serial.println("\n=================== SYSTEM STATUS ===================");
  Serial.printf("Total Steps: %lu\n", totalSteps);
  Serial.printf("Active Devices: %lu\n", totalDevices);
  Serial.printf("Active Receivers: %lu\n", activeReceivers);
  Serial.printf("Running Version: %s\n", currentRunningVersion.c_str());
  Serial.printf("WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Uptime: %lu seconds\n", millis() / 1000);
  Serial.println("=====================================================\n");
}

// Include the same dashboard and debug console functions from previous server code
void handleMainDashboard() {
  // Use the same Navratri-themed dashboard from previous code
  String html = "<!DOCTYPE html>";
  // ... (include the complete dashboard HTML from previous server code)
  server.send(200, "text/html", "Dashboard HTML here - use previous server dashboard code");
}

void handleDebugConsole() {
  // Use the same debug console from previous code
  String html = "<!DOCTYPE html>";
  // ... (include the complete debug console HTML from previous server code)
  server.send(200, "text/html", "Debug console HTML here - use previous server debug code");
}

void handleDashboardData() {
  DynamicJsonDocument doc(512);
  
  doc["totalSteps"] = totalSteps;
  doc["totalDevices"] = totalDevices;
  doc["activeReceivers"] = activeReceivers;
  doc["systemStatus"] = WiFi.status() == WL_CONNECTED ? "Online" : "Offline";
  doc["uptime"] = millis() / 1000;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleDebugData() {
  DynamicJsonDocument doc(8192);
  JsonArray receivers = doc.createNestedArray("receivers");
  
  // Create receiver summaries with device details
  for (auto& receiverPair : receiverLastSeen) {
    String receiverId = receiverPair.first;
    unsigned long lastSeen = receiverPair.second;
    
    // Skip receivers not seen recently
    if (millis() - lastSeen > 60000) continue;
    
    JsonObject receiverObj = receivers.createNestedObject();
    receiverObj["id"] = receiverId.substring(12);
    receiverObj["fullId"] = receiverId;
    receiverObj["deviceCount"] = receiverDeviceCount[receiverId];
    
    unsigned long totalStepsForReceiver = 0;
    for (auto& devicePair : devices) {
      if (devicePair.second.bestReceiverId == receiverId) {
        totalStepsForReceiver += devicePair.second.stepCount;
      }
    }
    receiverObj["totalSteps"] = totalStepsForReceiver;
    
    unsigned long timeSince = (millis() - lastSeen) / 1000;
    receiverObj["lastSeen"] = String(timeSince) + "s ago";
    
    // Add devices for this receiver
    JsonArray devicesArray = receiverObj.createNestedArray("devices");
    for (auto& devicePair : devices) {
      if (devicePair.second.bestReceiverId == receiverId) {
        JsonObject deviceObj = devicesArray.createNestedObject();
        deviceObj["deviceId"] = devicePair.first;
        deviceObj["stepCount"] = devicePair.second.stepCount;
        deviceObj["batteryLevel"] = devicePair.second.batteryLevel;
        
        unsigned long deviceTimeSince = (millis() - devicePair.second.lastSeen) / 1000;
        deviceObj["lastSeen"] = deviceTimeSince;
      }
    }
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleReset() {
  resetAllData();
  server.send(200, "text/plain", "System reset successfully");
}

void resetAllData() {
  // Clear all data structures
  devices.clear();
  deviceReceiverMap.clear();
  receiverLastSeen.clear();
  receiverDeviceCount.clear();
  
  // Reset statistics
  totalSteps = 0;
  totalDevices = 0;
  activeReceivers = 0;
  
  Serial.println("\n========================================");
  Serial.println("           SYSTEM RESET COMPLETE        ");
  Serial.println("========================================");
  Serial.println("All data cleared successfully");
  Serial.println("Ready for new data collection");
  Serial.println("========================================\n");
}