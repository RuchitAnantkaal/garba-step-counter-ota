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
  if (millis() - lastStatsUpdate >= 1000) {
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
  if (millis() - lastStatusPrint >= 10000) {
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
  client.setTimeout(5000); // 15 second timeout
  HTTPClient http;
  
  Serial.println("📡 Checking GitHub for latest server version...");
  
  http.begin(client, versionCheckURL);
  http.addHeader("User-Agent", "ESP32-Server-OTA-Client");
  http.setTimeout(5000); // 15 second timeout
  
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

void handleMainDashboard() {
  String html = "<!DOCTYPE html>";
  html += "<html><head>";
  html += "<title>Total Number of Garba Steps</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  html += "body { font-family: 'Georgia', serif; background: linear-gradient(135deg, #8B5A2B 0%, #CD853F 25%, #DAA520 50%, #FF6347 75%, #DC143C 100%); min-height: 100vh; display: flex; align-items: center; justify-content: center; position: relative; overflow: hidden; }";
  html += "body::before { content: ''; position: absolute; top: 0; left: 0; right: 0; bottom: 0; background: radial-gradient(circle at 20% 20%, rgba(255, 215, 0, 0.3) 0%, transparent 50%), radial-gradient(circle at 80% 80%, rgba(255, 69, 0, 0.3) 0%, transparent 50%), radial-gradient(circle at 40% 60%, rgba(220, 20, 60, 0.2) 0%, transparent 50%); }";
  html += ".container { position: relative; z-index: 1; text-align: center; padding: 3rem; background: rgba(255, 255, 255, 0.95); border-radius: 20px; box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3); backdrop-filter: blur(10px); border: 2px solid rgba(218, 165, 32, 0.5); max-width: 800px; width: 90%; }";
  html += ".title { font-size: 3.5rem; font-weight: bold; background: linear-gradient(45deg, #8B4513, #DAA520, #FF6347, #DC143C); background-size: 400% 400%; -webkit-background-clip: text; -webkit-text-fill-color: transparent; background-clip: text; margin-bottom: 2rem; text-shadow: 2px 2px 4px rgba(0,0,0,0.1); animation: gradientShift 3s ease-in-out infinite; }";
  html += "@keyframes gradientShift { 0%, 100% { background-position: 0% 50%; } 50% { background-position: 100% 50%; } }";
  html += ".step-display { background: linear-gradient(135deg, #FF6347, #DC143C); padding: 4rem 2rem; border-radius: 15px; margin: 2rem 0; position: relative; overflow: hidden; }";
  html += ".step-display::before { content: ''; position: absolute; top: -2px; left: -2px; right: -2px; bottom: -2px; background: linear-gradient(45deg, #DAA520, #FF6347, #DAA520, #FF6347); border-radius: 15px; z-index: -1; animation: borderGlow 2s linear infinite; }";
  html += "@keyframes borderGlow { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }";
  html += ".step-count { font-size: 6rem; font-weight: 900; color: white; text-shadow: 3px 3px 6px rgba(0,0,0,0.5); margin-bottom: 1rem; font-family: 'Impact', sans-serif; letter-spacing: 2px; }";
  html += ".step-label { font-size: 1.8rem; color: white; font-weight: 600; text-transform: uppercase; letter-spacing: 3px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }";
  html += ".decorative-border { height: 4px; background: linear-gradient(90deg, #DAA520, #FF6347, #DC143C, #DAA520); margin: 2rem 0; border-radius: 2px; }";
  html += ".status-info { background: rgba(139, 69, 19, 0.1); padding: 1.5rem; border-radius: 10px; margin-top: 2rem; border: 1px solid rgba(218, 165, 32, 0.3); }";
  html += ".status-item { display: inline-block; margin: 0 1rem; padding: 0.5rem 1rem; background: rgba(218, 165, 32, 0.2); border-radius: 20px; color: #8B4513; font-weight: 600; }";
  html += ".festival-pattern { position: absolute; width: 100px; height: 100px; border-radius: 50%; opacity: 0.1; }";
  html += ".pattern-1 { top: 10%; left: 10%; background: radial-gradient(circle, #DAA520, transparent); animation: float 6s ease-in-out infinite; }";
  html += ".pattern-2 { top: 20%; right: 10%; background: radial-gradient(circle, #FF6347, transparent); animation: float 8s ease-in-out infinite reverse; }";
  html += ".pattern-3 { bottom: 10%; left: 15%; background: radial-gradient(circle, #DC143C, transparent); animation: float 7s ease-in-out infinite; }";
  html += ".pattern-4 { bottom: 20%; right: 15%; background: radial-gradient(circle, #DAA520, transparent); animation: float 9s ease-in-out infinite reverse; }";
  html += "@keyframes float { 0%, 100% { transform: translateY(0px) rotate(0deg); } 50% { transform: translateY(-20px) rotate(180deg); } }";
  html += ".connection-status { font-size: 0.9rem; color: #8B4513; margin-top: 1rem; }";
  html += ".last-update { color: #CD853F; font-style: italic; }";
  html += "@media (max-width: 768px) {";
  html += ".title { font-size: 2.5rem; }";
  html += ".step-count { font-size: 4rem; }";
  html += ".step-label { font-size: 1.4rem; }";
  html += ".container { padding: 2rem; }";
  html += ".status-item { display: block; margin: 0.5rem 0; }";
  html += "}";
  html += "@media (max-width: 480px) {";
  html += ".title { font-size: 2rem; }";
  html += ".step-count { font-size: 3rem; }";
  html += ".step-label { font-size: 1.2rem; letter-spacing: 1px; }";
  html += "}";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='festival-pattern pattern-1'></div>";
  html += "<div class='festival-pattern pattern-2'></div>";
  html += "<div class='festival-pattern pattern-3'></div>";
  html += "<div class='festival-pattern pattern-4'></div>";
  
  html += "<div class='container'>";
  html += "<h1 class='title'>Garba Steps</h1>";
  html += "<div class='decorative-border'></div>";
  
  html += "<div class='step-display'>";
  html += "<div class='step-count' id='totalSteps'>0</div>";
  html += "<div class='step-label'>Steps Counted</div>";
  html += "</div>";
  
  html += "<div class='decorative-border'></div>";
  
  html += "<div class='status-info'>";
  html += "<div class='status-item'>System: <span id='systemStatus'>Loading...</span></div>";
  html += "<div class='status-item'>Device: <span id='totalDevices'>0</span></div>";
  html += "<div class='status-item'>Stations: <span id='activeReceivers'>0</span></div>";
  html += "</div>";
  
  html += "<div class='connection-status'>";
  html += "Last Updated: <span class='last-update' id='lastUpdate'>Loading...</span>";
  html += "</div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "function formatNumber(num) { return num.toLocaleString(); }";
  html += "function updateDashboard() {";
  html += "fetch('/api/dashboard-data').then(r => r.json()).then(data => {";
  html += "document.getElementById('totalSteps').textContent = formatNumber(data.totalSteps);";
  html += "document.getElementById('totalDevices').textContent = data.totalDevices;";
  html += "document.getElementById('activeReceivers').textContent = data.activeReceivers;";
  html += "document.getElementById('systemStatus').textContent = data.systemStatus;";
  html += "document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();";
  html += "}).catch(() => {";
  html += "document.getElementById('systemStatus').textContent = 'Error';";
  html += "document.getElementById('lastUpdate').textContent = 'Connection Failed';";
  html += "});";
  html += "}";
  html += "setInterval(updateDashboard, 2000);";
  html += "updateDashboard();";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleDebugConsole() {
  String html = "<!DOCTYPE html>";
  html += "<html><head>";
  html += "<title>Debug Console - Garba Step Counter</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  html += "body { font-family: 'Consolas', 'Monaco', monospace; background: #0d1117; color: #c9d1d9; }";
  html += ".header { background: #21262d; padding: 1.5rem; border-bottom: 1px solid #30363d; }";
  html += ".header h1 { color: #58a6ff; font-size: 1.8rem; margin-bottom: 0.5rem; }";
  html += ".header p { color: #8b949e; }";
  html += ".container { max-width: 1400px; margin: 0 auto; padding: 1.5rem; }";
  html += ".nav-bar { background: #21262d; border-radius: 6px; padding: 1rem; margin-bottom: 1.5rem; text-align: center; }";
  html += ".nav-link { display: inline-block; padding: 0.5rem 1rem; margin: 0 0.5rem; background: #30363d; color: #c9d1d9; text-decoration: none; border-radius: 6px; transition: background 0.2s; }";
  html += ".nav-link:hover { background: #484f58; }";
  html += ".nav-link.active { background: #1f6feb; }";
  html += ".stats-bar { background: #21262d; border-radius: 6px; padding: 1rem; margin-bottom: 1.5rem; display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 1rem; }";
  html += ".stat-item { text-align: center; }";
  html += ".stat-value { font-size: 1.5rem; font-weight: bold; color: #58a6ff; }";
  html += ".stat-label { font-size: 0.75rem; color: #8b949e; margin-top: 0.25rem; }";
  html += ".controls { background: #21262d; border-radius: 6px; padding: 1.5rem; margin-bottom: 1.5rem; }";
  html += ".controls h3 { color: #f0f6fc; margin-bottom: 1rem; }";
  html += ".control-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; }";
  html += ".btn { padding: 0.75rem 1rem; border: none; border-radius: 6px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; }";
  html += ".btn-danger { background: #da3633; color: white; }";
  html += ".btn-danger:hover { background: #b92124; }";
  html += ".btn-success { background: #238636; color: white; }";
  html += ".btn-success:hover { background: #196127; }";
  html += ".debug-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1.5rem; }";
  html += ".debug-panel { background: #21262d; border-radius: 6px; padding: 1.5rem; }";
  html += ".panel-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; border-bottom: 1px solid #30363d; padding-bottom: 0.75rem; }";
  html += ".panel-title { color: #f0f6fc; font-size: 1.1rem; font-weight: 600; }";
  html += ".panel-badge { background: #1f6feb; color: white; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; }";
  html += ".receiver-list { max-height: 400px; overflow-y: auto; }";
  html += ".receiver-item { background: #0d1117; padding: 1rem; margin-bottom: 0.75rem; border-radius: 6px; border-left: 3px solid #1f6feb; cursor: pointer; transition: all 0.2s; }";
  html += ".receiver-item:hover { background: #161b22; transform: translateX(2px); }";
  html += ".receiver-item.selected { border-left-color: #f85149; background: #161b22; }";
  html += ".receiver-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem; }";
  html += ".receiver-id { color: #58a6ff; font-weight: 600; }";
  html += ".receiver-status { background: #238636; color: white; padding: 0.125rem 0.5rem; border-radius: 10px; font-size: 0.625rem; }";
  html += ".receiver-meta { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 0.5rem; font-size: 0.8rem; }";
  html += ".meta-item { text-align: center; }";
  html += ".meta-label { color: #8b949e; }";
  html += ".meta-value { color: #c9d1d9; font-weight: 500; }";
  html += ".device-details { max-height: 400px; overflow-y: auto; background: #0d1117; border-radius: 6px; padding: 0.75rem; }";
  html += ".device-header { display: grid; grid-template-columns: 2fr 1fr 1fr 1fr; gap: 1rem; font-size: 0.75rem; color: #8b949e; border-bottom: 1px solid #30363d; padding-bottom: 0.5rem; margin-bottom: 0.75rem; }";
  html += ".device-item { display: grid; grid-template-columns: 2fr 1fr 1fr 1fr; gap: 1rem; padding: 0.5rem 0; border-bottom: 1px solid #30363d; font-size: 0.75rem; }";
  html += ".device-item:last-child { border-bottom: none; }";
  html += ".device-id { color: #f79c42; font-family: monospace; }";
  html += ".device-steps { color: #56d364; }";
  html += ".device-battery { color: #58a6ff; }";
  html += ".device-time { color: #bc8cff; }";
  html += ".empty-state { text-align: center; padding: 2rem; color: #8b949e; }";
  html += "@media (max-width: 1024px) { .debug-grid { grid-template-columns: 1fr; } }";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='header'>";
  html += "<h1>Debug Console</h1>";
  html += "<p>Detailed system monitoring and device tracking</p>";
  html += "</div>";
  
  html += "<div class='container'>";
  
  html += "<div class='nav-bar'>";
  html += "<a href='/stepcounter' class='nav-link'>Main Dashboard</a>";
  html += "<a href='/debugger' class='nav-link active'>Debug Console</a>";
  html += "</div>";
  
  html += "<div class='stats-bar'>";
  html += "<div class='stat-item'><div class='stat-value' id='debugTotalSteps'>0</div><div class='stat-label'>Total Steps</div></div>";
  html += "<div class='stat-item'><div class='stat-value' id='debugTotalDevices'>0</div><div class='stat-label'>Active Devices</div></div>";
  html += "<div class='stat-item'><div class='stat-value' id='debugActiveReceivers'>0</div><div class='stat-label'>Active Receivers</div></div>";
  html += "<div class='stat-item'><div class='stat-value' id='debugLastUpdate'>--</div><div class='stat-label'>Last Update</div></div>";
  html += "</div>";
  
  html += "<div class='controls'>";
  html += "<h3>System Controls</h3>";
  html += "<div class='control-grid'>";
  html += "<button class='btn btn-danger' onclick='resetSystem()'>Reset All Data</button>";
  html += "<button class='btn btn-success' onclick='refreshData()'>Refresh Data</button>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='debug-grid'>";
  html += "<div class='debug-panel'>";
  html += "<div class='panel-header'>";
  html += "<div class='panel-title'>Active Receivers</div>";
  html += "<div class='panel-badge' id='receiverCount'>0</div>";
  html += "</div>";
  html += "<div class='receiver-list' id='receiverList'>";
  html += "<div class='empty-state'>Loading receiver data...</div>";
  html += "</div>";
  html += "</div>";
  html += "<div class='debug-panel'>";
  html += "<div class='panel-header'>";
  html += "<div class='panel-title'>Device Details</div>";
  html += "<div class='panel-badge' id='deviceCount'>0</div>";
  html += "</div>";
  html += "<div id='deviceDetails'>";
  html += "<div class='empty-state'>Select a receiver to view connected devices</div>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "let selectedReceiver = null;";
  html += "function formatNumber(num) { return num.toLocaleString(); }";
  html += "function formatTime(seconds) { return seconds < 60 ? seconds + 's' : seconds < 3600 ? Math.floor(seconds/60) + 'm' : Math.floor(seconds/3600) + 'h'; }";
  html += "function updateDebugStats() {";
 html += "fetch('/api/dashboard-data').then(r => r.json()).then(data => {";
html += "document.getElementById('debugTotalSteps').textContent = formatNumber(data.totalSteps);";
html += "document.getElementById('debugTotalDevices').textContent = data.totalDevices;";
html += "document.getElementById('debugActiveReceivers').textContent = data.activeReceivers;";
html += "document.getElementById('debugLastUpdate').textContent = new Date().toLocaleTimeString();";
html += "}).catch(() => document.getElementById('debugTotalSteps').textContent = 'Error');";
html += "}";
html += "function updateReceivers() {";
html += "fetch('/api/debug-data').then(r => r.json()).then(data => {";
html += "document.getElementById('receiverCount').textContent = data.receivers.length;";
html += "let html = '';";
html += "if (data.receivers.length === 0) {";
html += "html = '<div class=\"empty-state\">No receivers connected</div>';";
html += "} else {";
html += "data.receivers.forEach(receiver => {";
html += "const isSelected = selectedReceiver === receiver.id;";
html += "html += '<div class=\"receiver-item' + (isSelected ? ' selected' : '') + '\" onclick=\"selectReceiver(\\'' + receiver.id + '\\')\">';";
html += "html += '<div class=\"receiver-header\">';";
html += "html += '<div class=\"receiver-id\">Receiver ' + receiver.id + '</div>';";
html += "html += '<div class=\"receiver-status\">Online</div>';";
html += "html += '</div>';";
html += "html += '<div class=\"receiver-meta\">';";
html += "html += '<div class=\"meta-item\"><div class=\"meta-label\">Devices</div><div class=\"meta-value\">' + receiver.deviceCount + '</div></div>';";
html += "html += '<div class=\"meta-item\"><div class=\"meta-label\">Steps</div><div class=\"meta-value\">' + formatNumber(receiver.totalSteps) + '</div></div>';";
html += "html += '<div class=\"meta-item\"><div class=\"meta-label\">Last Seen</div><div class=\"meta-value\">' + receiver.lastSeen + '</div></div>';";
html += "html += '</div></div>';";
html += "});";
html += "}";
html += "document.getElementById('receiverList').innerHTML = html;";
html += "}).catch(() => document.getElementById('receiverList').innerHTML = '<div class=\"empty-state\">Connection Error</div>');";
html += "}";
html += "function selectReceiver(receiverId) {";
html += "selectedReceiver = receiverId;";
html += "updateReceivers();";
html += "updateDeviceDetails();";
html += "}";
html += "function updateDeviceDetails() {";
html += "if (!selectedReceiver) {";
html += "document.getElementById('deviceDetails').innerHTML = '<div class=\"empty-state\">Select a receiver to view devices</div>';";
html += "return;";
html += "}";
html += "fetch('/api/debug-data').then(r => r.json()).then(data => {";
html += "const receiver = data.receivers.find(r => r.id === selectedReceiver);";
html += "if (!receiver || !receiver.devices || receiver.devices.length === 0) {";
html += "document.getElementById('deviceDetails').innerHTML = '<div class=\"empty-state\">No devices found</div>';";
html += "document.getElementById('deviceCount').textContent = '0';";
html += "return;";
html += "}";
html += "document.getElementById('deviceCount').textContent = receiver.devices.length;";
html += "let html = '<div class=\"device-details\">';";
html += "html += '<div class=\"device-header\"><div>Device ID</div><div>Steps</div><div>Battery</div><div>Last Seen</div></div>';";
html += "receiver.devices.forEach(device => {";
html += "html += '<div class=\"device-item\">';";
html += "html += '<div class=\"device-id\">' + device.deviceId.substring(9) + '</div>';";
html += "html += '<div class=\"device-steps\">' + formatNumber(device.stepCount) + '</div>';";
html += "html += '<div class=\"device-battery\">' + device.batteryLevel.toFixed(1) + '%</div>';";
html += "html += '<div class=\"device-time\">' + formatTime(device.lastSeen) + '</div>';";
html += "html += '</div>';";
html += "});";
html += "html += '</div>';";
html += "document.getElementById('deviceDetails').innerHTML = html;";
html += "}).catch(() => document.getElementById('deviceDetails').innerHTML = '<div class=\"empty-state\">Error loading devices</div>');";
html += "}";
html += "function resetSystem() {";
html += "if (confirm('Reset ALL step data? This cannot be undone!')) {";
html += "fetch('/api/reset', {method: 'POST'}).then(() => {";
html += "alert('System reset successfully');";
html += "selectedReceiver = null;";
html += "refreshData();";
html += "}).catch(() => alert('Reset failed'));";
html += "}";
html += "}";
html += "function refreshData() {";
html += "updateDebugStats();";
html += "updateReceivers();";
html += "updateDeviceDetails();";
html += "}";
html += "setInterval(refreshData, 3000);";
html += "refreshData();";
html += "</script>";
html += "</body></html>";

server.send(200, "text/html", html);
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