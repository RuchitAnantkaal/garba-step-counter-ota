#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <espnow.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <map>

// WiFi credentials for connecting to router (your phone hotspot)
const char* ssid = "Anantkaal_4G";     // Your hotspot name
const char* password = "Setupdev@123";  // Your hotspot password

// Server configuration for sending data
const char* serverHost = "garba.local";  // mDNS hostname
const int serverPort = 80;
const char* serverEndpoint = "/api/steps";  // Endpoint to receive step data

// Web server
ESP8266WebServer server(80);
WiFiClient client;
HTTPClient http;

// Data structure for receiving ESP-NOW data
typedef struct {
  char deviceId[18];
  unsigned long stepCount;
  float batteryLevel;
} StepData;

// Device tracking
std::map<String, unsigned long> deviceSteps;
std::map<String, unsigned long> deviceLastSeen;
unsigned long totalSteps = 0;
unsigned long lastStatusUpdate = 0;
const unsigned long statusUpdateInterval = 1000; // Print status every 1 second

// Reset button pin (optional - you can use a physical button)
#define RESET_BUTTON_PIN 0 // GPIO0 (FLASH button on most ESP8266 boards)

void setup() {
  Serial.begin(115200);
  
  // Initialize EEPROM
  EEPROM.begin(512);
  loadTotalSteps();
  
  // Initialize reset button
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  
  // Set custom hostname and start mDNS
  WiFi.hostname("garba");
  
  // Initialize ESP-NOW first
  initESPNow();
  
  // Connect to WiFi router
  connectToWiFi();
  
  // Start mDNS service
  if (MDNS.begin("garba")) {
    Serial.println("mDNS responder started");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error setting up mDNS responder!");
  }
  
  // Start web server
  setupWebServer();
  
  Serial.println("\n=== Step Counter Repeater Started ===");
  Serial.print("Connected to WiFi: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.println("Web interface available at:");
  Serial.println("  http://garba.local/stepcounter  <-- Use this URL");
  Serial.print("  http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stepcounter");
  Serial.println("Ready to receive step data from transmitters...");
  Serial.println("Status updates will be printed every 1 second");
  Serial.println("================================================");
}

void loop() {
  // Handle web server
  server.handleClient();
  
  // Handle mDNS
  MDNS.update();
  
  // Check reset button
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    delay(50); // Debounce
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
      resetTotalSteps();
      delay(1000); // Prevent multiple resets
    }
  }
  
  // Print status update every second
  if (millis() - lastStatusUpdate >= statusUpdateInterval) {
    printStatusUpdate();
    lastStatusUpdate = millis();
  }
  
  // Clean up old device entries (remove devices not seen for 5 minutes)
  cleanupOldDevices();
  
  delay(100);
}

void initESPNow() {
  // Set device as WiFi station
  WiFi.mode(WIFI_STA);
  
  // Initialize ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  Serial.println("ESP-NOW initialized successfully");
  
  // Set ESP-NOW role
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  
  // Register callback for receiving data
  esp_now_register_recv_cb(onDataReceived);
}

// Callback function for ESP-NOW data reception
void onDataReceived(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  StepData receivedData;
  memcpy(&receivedData, incomingData, sizeof(receivedData));
  
  String deviceId = String(receivedData.deviceId);
  unsigned long newStepCount = receivedData.stepCount;
  
  Serial.println("\n🔥 ESP-NOW DATA RECEIVED 🔥");
  Serial.print("📱 Device ID: ");
  Serial.println(deviceId);
  Serial.print("👟 Step Count: ");
  Serial.println(newStepCount);
  Serial.print("🔋 Battery: ");
  Serial.print(receivedData.batteryLevel);
  Serial.println("%");
  
  // Update device data
  unsigned long previousSteps = deviceSteps[deviceId];
  deviceSteps[deviceId] = newStepCount;
  deviceLastSeen[deviceId] = millis();
  
  // Calculate step difference and update total
  if (newStepCount > previousSteps) {
    unsigned long stepDifference = newStepCount - previousSteps;
    totalSteps += stepDifference;
    
    Serial.print("✨ New steps from this device: ");
    Serial.println(stepDifference);
    Serial.print("🎯 TOTAL STEPS NOW: ");
    Serial.println(totalSteps);
    
    // Save to EEPROM
    saveTotalSteps();
  } else if (newStepCount == previousSteps) {
    Serial.println("📊 No new steps from this device");
  }
  
  Serial.println("===============================\n");
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected successfully!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("");
    Serial.println("Failed to connect to WiFi!");
    Serial.println("Operating in ESP-NOW only mode...");
  }
}

void setupWebServer() {
  // Main page at custom route
  server.on("/stepcounter", handleRoot);
  server.on("/", handleRoot); // Also respond to root
  
  // API endpoints
  server.on("/api/data", handleGetData);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/devices", handleGetDevices);
  
  server.begin();
  Serial.println("Web server started on port 80");
  Serial.println("Main interface: http://garba.local/stepcounter");
}

void handleRoot() {
  String html = "<!DOCTYPE html>";
  html += "<html><head>";
  html += "<title>Step Counter Repeater</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; text-align: center; }";
  html += ".total-counter { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 30px; border-radius: 10px; text-align: center; margin: 20px 0; }";
  html += ".total-counter .count { font-size: 72px; font-weight: bold; }";
  html += ".devices-section { background: #f9f9f9; padding: 20px; border-radius: 8px; margin: 20px 0; }";
  html += ".device-item { background: white; padding: 15px; margin: 10px 0; border-radius: 5px; border-left: 4px solid #2196F3; }";
  html += ".device-id { font-weight: bold; color: #333; }";
  html += ".device-steps { color: #2196F3; font-size: 18px; }";
  html += ".device-time { color: #666; font-size: 12px; }";
  html += "button { background: #f44336; color: white; padding: 15px 30px; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; }";
  html += "button:hover { background: #d32f2f; }";
  html += ".status-info { background: #e8f5e8; padding: 15px; border-radius: 8px; margin: 15px 0; }";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='container'>";
  html += "<h1>Step Counter Repeater</h1>";
  
  html += "<div class='total-counter'>";
  html += "<div class='count' id='totalSteps'>" + String(totalSteps) + "</div>";
  html += "<div>Total Steps from All Devices</div>";
  html += "</div>";
  
  html += "<div class='status-info'>";
  html += "<strong>WiFi Status:</strong> " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "<br>";
  html += "<strong>Network:</strong> " + String(WiFi.SSID()) + "<br>";
  html += "<strong>Active Devices:</strong> <span id='deviceCount'>" + String(deviceSteps.size()) + "</span><br>";
  html += "<strong>Local URL:</strong> http://garba.local/stepcounter<br>";
  html += "<strong>Last Update:</strong> <span id='lastUpdate'>Loading...</span>";
  html += "</div>";
  
  html += "<div class='devices-section'>";
  html += "<h3>Connected Devices</h3>";
  html += "<div id='devicesList'>";
  html += "Loading device information...";
  html += "</div>";
  html += "</div>";
  
  html += "<div style='text-align: center; margin: 30px 0;'>";
  html += "<button onclick='resetSteps()'>RESET ALL STEPS</button>";
  html += "</div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "function updateData() {";
  html += "fetch('/api/data').then(r => r.json()).then(data => {";
  html += "document.getElementById('totalSteps').textContent = data.totalSteps;";
  html += "document.getElementById('deviceCount').textContent = data.deviceCount;";
  html += "document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();";
  html += "});";
  html += "fetch('/api/devices').then(r => r.json()).then(devices => {";
  html += "let html = '';";
  html += "if(devices.length === 0) html = '<p>No devices connected yet...</p>';";
  html += "devices.forEach(device => {";
  html += "html += '<div class=\"device-item\">';";
  html += "html += '<div class=\"device-id\">Device: ' + device.id + '</div>';";
  html += "html += '<div class=\"device-steps\">Steps: ' + device.steps + '</div>';";
  html += "html += '<div class=\"device-time\">Last seen: ' + device.lastSeen + '</div>';";
  html += "html += '</div>';";
  html += "});";
  html += "document.getElementById('devicesList').innerHTML = html;";
  html += "});";
  html += "}";
  html += "function resetSteps() {";
  html += "if(confirm('Reset all step counts? This cannot be undone!')) {";
  html += "fetch('/api/reset', {method: 'POST'}).then(() => updateData());";
  html += "}";
  html += "}";
  html += "setInterval(updateData, 2000);";
  html += "updateData();";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleGetData() {
  DynamicJsonDocument doc(200);
  doc["totalSteps"] = totalSteps;
  doc["deviceCount"] = deviceSteps.size();
  doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  doc["uptime"] = millis();
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleGetDevices() {
  DynamicJsonDocument doc(1024);
  JsonArray devices = doc.createNestedArray("devices");
  
  for (auto& pair : deviceSteps) {
    JsonObject device = devices.createNestedObject();
    device["id"] = pair.first;
    device["steps"] = pair.second;
    
    unsigned long lastSeen = deviceLastSeen[pair.first];
    unsigned long timeSince = (millis() - lastSeen) / 1000;
    device["lastSeen"] = String(timeSince) + "s ago";
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleReset() {
  resetTotalSteps();
  server.send(200, "text/plain", "All steps reset successfully");
}

void resetTotalSteps() {
  totalSteps = 0;
  deviceSteps.clear();
  deviceLastSeen.clear();
  saveTotalSteps();
  
  Serial.println("\n🔄 RESET PERFORMED 🔄");
  Serial.println("👟 Total steps: 0");
  Serial.println("📱 All device data cleared");
  Serial.println("💾 Saved to EEPROM");
  Serial.println("===================\n");
}

void printStatusUpdate() {
  Serial.println("\n========== REPEATER STATUS ==========");
  Serial.print("🕐 Uptime: ");
  Serial.print(millis() / 1000);
  Serial.println(" seconds");
  
  Serial.print("👟 Total Steps: ");
  Serial.println(totalSteps);
  
  Serial.print("📱 Active Devices: ");
  Serial.println(deviceSteps.size());
  
  if (deviceSteps.size() > 0) {
    Serial.println("📊 Device Details:");
    for (auto& pair : deviceSteps) {
      unsigned long timeSince = (millis() - deviceLastSeen[pair.first]) / 1000;
      Serial.print("   Device ");
      Serial.print(pair.first.substring(12)); // Show last 6 chars of MAC
      Serial.print(": ");
      Serial.print(pair.second);
      Serial.print(" steps (");
      Serial.print(timeSince);
      Serial.println("s ago)");
    }
  } else {
    Serial.println("📡 Waiting for transmitter devices...");
  }
  
  Serial.print("🌐 WiFi: ");
  Serial.print(WiFi.status() == WL_CONNECTED ? "Connected to " : "Disconnected");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(WiFi.SSID());
  } else {
    Serial.println();
  }
  
  Serial.print("🔗 Web Interface: http://garba.local/stepcounter");
  Serial.println();
  Serial.println("====================================\n");
}

void cleanupOldDevices() {
  const unsigned long maxAge = 300000; // 5 minutes
  
  for (auto it = deviceLastSeen.begin(); it != deviceLastSeen.end();) {
    if (millis() - it->second > maxAge) {
      String deviceId = it->first;
      Serial.print("Removing old device: ");
      Serial.println(deviceId);
      
      deviceSteps.erase(deviceId);
      it = deviceLastSeen.erase(it);
    } else {
      ++it;
    }
  }
}

void saveTotalSteps() {
  EEPROM.put(0, totalSteps);
  EEPROM.commit();
}

void loadTotalSteps() {
  EEPROM.get(0, totalSteps);
  
  // Validate loaded value
  if (totalSteps > 1000000) { // Sanity check
    totalSteps = 0;
  }
  
  Serial.print("Loaded total steps from EEPROM: ");
  Serial.println(totalSteps);
}