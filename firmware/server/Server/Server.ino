#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <map>
#include <set>

// WiFi credentials
const char* ssid = "Anantkaal_4G";
const char* password = "Setupdev@123";

// Web server
WebServer server(80);

// Data structures for tracking all devices across all receivers
struct DeviceInfo {
  unsigned long stepCount;
  float batteryLevel;
  String receiverId;
  unsigned long lastSeen;
};

std::map<String, DeviceInfo> allDevices; // deviceId -> DeviceInfo
std::map<String, unsigned long> receiverLastSeen; // receiverId -> timestamp
std::set<String> activeReceivers;

// Statistics
unsigned long totalStepsAllDevices = 0;
unsigned long lastStatsUpdate = 0;
const unsigned long statsUpdateInterval = 5000; // Update stats every 5 seconds

// Reset button pin (GPIO0 on most ESP32 boards)
#define RESET_BUTTON_PIN 0

void setup() {
  Serial.begin(115200);
  
  // Initialize EEPROM
  EEPROM.begin(512);
  
  // Initialize reset button
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  
  // Connect to WiFi
  connectToWiFi();
  
  // Start mDNS service
  if (MDNS.begin("garba")) {
    Serial.println("mDNS responder started");
    MDNS.addService("http", "tcp", 80);
  }
  
  // Setup web server routes
  setupWebServer();
  
  Serial.println("\n=== ESP32 Central Step Counter Server ===");
  Serial.print("Server IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("Web interface available at:");
  Serial.println("  http://garba.local/stepcounter");
  Serial.print("  http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stepcounter");
  Serial.println("Ready to receive data from 100 receivers");
  Serial.println("========================================");
}

void loop() {
  // Handle web server requests
  server.handleClient();
  
  // Check reset button
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    delay(50); // Debounce
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
      resetAllData();
      delay(1000);
    }
  }
  
  // Update statistics periodically
  if (millis() - lastStatsUpdate >= statsUpdateInterval) {
    updateStatistics();
    lastStatsUpdate = millis();
  }
  
  // Clean up old receivers and devices
  cleanupOldData();
  
  delay(100);
}

void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
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
    ESP.restart(); // Restart if can't connect
  }
}

void setupWebServer() {
  // Main interface
  server.on("/stepcounter", handleRoot);
  server.on("/", handleRoot);
  
  // API endpoints
  server.on("/api/receiver-data", HTTP_POST, handleReceiverData);
  server.on("/api/dashboard-data", HTTP_GET, handleDashboardData);
  server.on("/api/receivers", HTTP_GET, handleReceiversData);
  server.on("/api/reset", HTTP_POST, handleReset);
  
  // Enable CORS for all requests
  server.enableCORS(true);
  
  server.begin();
  Serial.println("Web server started on port 80");
}

void handleReceiverData() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  // Parse JSON data from receiver
  DynamicJsonDocument doc(4096); // Large buffer for multiple devices
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  // Extract receiver info
  String receiverId = doc["receiverId"].as<String>();
  unsigned long timestamp = doc["timestamp"];
  int deviceCount = doc["deviceCount"];
  
  // Update receiver last seen
  receiverLastSeen[receiverId] = millis();
  activeReceivers.insert(receiverId);
  
  // Process all devices from this receiver
  JsonArray devices = doc["devices"];
  for (JsonObject device : devices) {
    String deviceId = device["deviceId"].as<String>();
    unsigned long stepCount = device["stepCount"];
    float batteryLevel = device["batteryLevel"];
    unsigned long lastSeen = device["lastSeen"];
    
    // Update device info
    DeviceInfo& info = allDevices[deviceId];
    info.stepCount = stepCount;
    info.batteryLevel = batteryLevel;
    info.receiverId = receiverId;
    info.lastSeen = millis() - (lastSeen * 1000); // Convert back to absolute time
  }
  
  // Log reception (but not too frequently)
  static unsigned long lastLogTime = 0;
  if (millis() - lastLogTime > 10000) { // Log every 10 seconds
    Serial.print("📡 Data from receiver ");
    Serial.print(receiverId.substring(12));
    Serial.print(" (");
    Serial.print(deviceCount);
    Serial.println(" devices)");
    lastLogTime = millis();
  }
  
  server.send(200, "text/plain", "Data received successfully");
}

void handleRoot() {
  String html = "<!DOCTYPE html>";
  html += "<html><head>";
  html += "<title>Garba Step Counter Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Arial, sans-serif; margin: 0; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; }";
  html += ".container { max-width: 1200px; margin: 0 auto; padding: 20px; }";
  html += "h1 { color: white; text-align: center; font-size: 3em; margin-bottom: 30px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }";
  html += ".stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; margin: 30px 0; }";
  html += ".stat-card { background: white; padding: 25px; border-radius: 15px; box-shadow: 0 8px 25px rgba(0,0,0,0.1); text-align: center; }";
  html += ".stat-value { font-size: 3em; font-weight: bold; color: #667eea; margin: 10px 0; }";
  html += ".stat-label { font-size: 1.1em; color: #666; text-transform: uppercase; letter-spacing: 1px; }";
  html += ".total-steps { background: linear-gradient(135deg, #ff6b6b, #ffd93d); color: white; }";
  html += ".total-steps .stat-value { color: white; font-size: 4em; }";
  html += ".receivers-section { background: white; border-radius: 15px; padding: 30px; margin: 30px 0; box-shadow: 0 8px 25px rgba(0,0,0,0.1); }";
  html += ".receiver-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap: 15px; margin-top: 20px; }";
  html += ".receiver-card { background: #f8f9fa; padding: 15px; border-radius: 10px; border-left: 4px solid #667eea; }";
  html += ".receiver-id { font-weight: bold; color: #333; }";
  html += ".receiver-stats { color: #666; margin-top: 5px; }";
  html += ".reset-section { text-align: center; margin: 30px 0; }";
  html += ".reset-btn { background: #ff4757; color: white; padding: 15px 30px; border: none; border-radius: 25px; font-size: 1.1em; cursor: pointer; box-shadow: 0 4px 15px rgba(255,71,87,0.3); }";
  html += ".reset-btn:hover { background: #ff3742; transform: translateY(-2px); }";
  html += ".status-bar { background: rgba(255,255,255,0.9); padding: 15px; border-radius: 10px; margin-bottom: 20px; }";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='container'>";
  html += "<h1>🎭 Garba Step Counter Dashboard</h1>";
  
  html += "<div class='status-bar'>";
  html += "<strong>System Status:</strong> <span id='systemStatus'>Loading...</span> | ";
  html += "<strong>Last Update:</strong> <span id='lastUpdate'>Loading...</span>";
  html += "</div>";
  
  html += "<div class='stats-grid'>";
  html += "<div class='stat-card total-steps'>";
  html += "<div class='stat-value' id='totalSteps'>0</div>";
  html += "<div class='stat-label'>Total Steps</div>";
  html += "</div>";
  html += "<div class='stat-card'>";
  html += "<div class='stat-value' id='totalDevices'>0</div>";
  html += "<div class='stat-label'>Active Devices</div>";
  html += "</div>";
  html += "<div class='stat-card'>";
  html += "<div class='stat-value' id='activeReceivers'>0</div>";
  html += "<div class='stat-label'>Active Receivers</div>";
  html += "</div>";
  html += "<div class='stat-card'>";
  html += "<div class='stat-value' id='avgStepsPerDevice'>0</div>";
  html += "<div class='stat-label'>Avg Steps/Device</div>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='receivers-section'>";
  html += "<h2>📡 Receiver Status</h2>";
  html += "<div class='receiver-grid' id='receiversGrid'>";
  html += "Loading receiver data...";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='reset-section'>";
  html += "<button class='reset-btn' onclick='resetAllData()'>🔄 RESET ALL DATA</button>";
  html += "</div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "function updateDashboard() {";
  html += "fetch('/api/dashboard-data').then(r => r.json()).then(data => {";
  html += "document.getElementById('totalSteps').textContent = data.totalSteps.toLocaleString();";
  html += "document.getElementById('totalDevices').textContent = data.totalDevices;";
  html += "document.getElementById('activeReceivers').textContent = data.activeReceivers;";
  html += "document.getElementById('avgStepsPerDevice').textContent = data.avgStepsPerDevice;";
  html += "document.getElementById('systemStatus').textContent = data.systemStatus;";
  html += "document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();";
  html += "});";
  html += "fetch('/api/receivers').then(r => r.json()).then(data => {";
  html += "let html = '';";
  html += "if(data.receivers.length === 0) html = '<p>No receivers connected</p>';";
  html += "data.receivers.forEach(receiver => {";
  html += "html += '<div class=\"receiver-card\">';";
  html += "html += '<div class=\"receiver-id\">Receiver: ' + receiver.id + '</div>';";
  html += "html += '<div class=\"receiver-stats\">';";
  html += "html += 'Devices: ' + receiver.deviceCount + ' | ';";
  html += "html += 'Steps: ' + receiver.totalSteps.toLocaleString() + ' | ';";
  html += "html += 'Last seen: ' + receiver.lastSeen;";
  html += "html += '</div></div>';";
  html += "});";
  html += "document.getElementById('receiversGrid').innerHTML = html;";
  html += "});";
  html += "}";
  html += "function resetAllData() {";
  html += "if(confirm('Reset ALL step data from ALL devices and receivers?\\nThis cannot be undone!')) {";
  html += "fetch('/api/reset', {method: 'POST'}).then(() => {";
  html += "alert('All data has been reset!');";
  html += "updateDashboard();";
  html += "});";
  html += "}";
  html += "}";
  html += "setInterval(updateDashboard, 2000);";
  html += "updateDashboard();";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleDashboardData() {
  DynamicJsonDocument doc(512);
  
  doc["totalSteps"] = totalStepsAllDevices;
  doc["totalDevices"] = allDevices.size();
  doc["activeReceivers"] = activeReceivers.size();
  doc["avgStepsPerDevice"] = allDevices.size() > 0 ? totalStepsAllDevices / allDevices.size() : 0;
  doc["systemStatus"] = WiFi.status() == WL_CONNECTED ? "Online" : "Offline";
  doc["uptime"] = millis() / 1000;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleReceiversData() {
  DynamicJsonDocument doc(4096);
  JsonArray receivers = doc.createNestedArray("receivers");
  
  // Group devices by receiver
  std::map<String, int> receiverDeviceCount;
  std::map<String, unsigned long> receiverStepCount;
  
  for (auto& pair : allDevices) {
    String receiverId = pair.second.receiverId;
    receiverDeviceCount[receiverId]++;
    receiverStepCount[receiverId] += pair.second.stepCount;
  }
  
  for (auto& pair : receiverLastSeen) {
    JsonObject receiver = receivers.createNestedObject();
    String receiverId = pair.first;
    
    receiver["id"] = receiverId.substring(12); // Last 6 chars
    receiver["deviceCount"] = receiverDeviceCount[receiverId];
    receiver["totalSteps"] = receiverStepCount[receiverId];
    
    unsigned long timeSince = (millis() - pair.second) / 1000;
    receiver["lastSeen"] = String(timeSince) + "s ago";
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleReset() {
  resetAllData();
  server.send(200, "text/plain", "All data reset successfully");
}

void updateStatistics() {
  totalStepsAllDevices = 0;
  for (auto& pair : allDevices) {
    totalStepsAllDevices += pair.second.stepCount;
  }
  
  // Clean up activeReceivers set
  activeReceivers.clear();
  for (auto& pair : receiverLastSeen) {
    if (millis() - pair.second < 30000) { // Active if seen in last 30 seconds
      activeReceivers.insert(pair.first);
    }
  }
  
  // Print status every minute
  static unsigned long lastStatusPrint = 0;
  if (millis() - lastStatusPrint > 60000) {
    Serial.println("\n========== SERVER STATUS ==========");
    Serial.print("🎯 Total Steps: ");
    Serial.println(totalStepsAllDevices);
    Serial.print("📱 Active Devices: ");
    Serial.println(allDevices.size());
    Serial.print("📡 Active Receivers: ");
    Serial.println(activeReceivers.size());
    Serial.print("🌐 Web Access: http://garba.local/stepcounter");
    Serial.println();
    Serial.println("==================================\n");
    lastStatusPrint = millis();
  }
}

void cleanupOldData() {
  const unsigned long maxDeviceAge = 300000; // 5 minutes
  const unsigned long maxReceiverAge = 60000; // 1 minute
  
  // Clean up old devices
  for (auto it = allDevices.begin(); it != allDevices.end();) {
    if (millis() - it->second.lastSeen > maxDeviceAge) {
      it = allDevices.erase(it);
    } else {
      ++it;
    }
  }
  
  // Clean up old receivers
  for (auto it = receiverLastSeen.begin(); it != receiverLastSeen.end();) {
    if (millis() - it->second > maxReceiverAge) {
      activeReceivers.erase(it->first);
      it = receiverLastSeen.erase(it);
    } else {
      ++it;
    }
  }
}

void resetAllData() {
  allDevices.clear();
  receiverLastSeen.clear();
  activeReceivers.clear();
  totalStepsAllDevices = 0;
  
  Serial.println("\n🔄 ALL DATA RESET 🔄");
  Serial.println("All devices, receivers, and step counts cleared");
  Serial.println("===================\n");
}