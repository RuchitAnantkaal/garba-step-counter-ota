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
const unsigned long statsUpdateInterval = 1000; // Update stats every 5 seconds

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
  Serial.println("  http://garba.local/debugger");
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
  // Main interfaces
  server.on("/stepcounter", handleRoot);
  server.on("/debugger", handleDebugger);
  server.on("/", handleRoot);
  
  // API endpoints
  server.on("/api/receiver-data", HTTP_POST, handleReceiverData);
  server.on("/api/dashboard-data", HTTP_GET, handleDashboardData);
  server.on("/api/receivers", HTTP_GET, handleReceiversData);
  server.on("/api/debug-data", HTTP_GET, handleDebugData);
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
    Serial.print("Data from receiver ");
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
  html += "<div class='step-label'>Total Steps</div>";
  html += "</div>";
  
  html += "<div class='decorative-border'></div>";
  
  html += "<div class='status-info'>";
  html += "<div class='status-item'>System: <span id='systemStatus'>Loading...</span></div>";
  html += "<div class='status-item'>Device: <span id='totalDevices'>0</span></div>";
  html += "<div class='status-item'>Receivers: <span id='activeReceivers'>0</span></div>";
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

void handleDebugger() {
  String html = "<!DOCTYPE html>";
  html += "<html><head>";
  html += "<title>Debug Console - Garba Step Counter</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  html += "body { font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace; background: #0f172a; color: #e2e8f0; }";
  html += ".header { background: #1e293b; color: white; padding: 1.5rem 0; text-align: center; border-bottom: 1px solid #334155; }";
  html += ".header h1 { font-size: 1.8rem; font-weight: 600; margin-bottom: 0.5rem; }";
  html += ".header p { font-size: 0.9rem; opacity: 0.7; }";
  html += ".container { max-width: 1400px; margin: 0 auto; padding: 1.5rem; }";
  html += ".navigation { background: #1e293b; border-radius: 8px; padding: 1rem; margin-bottom: 1.5rem; text-align: center; }";
  html += ".nav-link { display: inline-block; padding: 0.5rem 1rem; margin: 0 0.25rem; background: #374151; color: #d1d5db; text-decoration: none; border-radius: 6px; font-size: 0.875rem; transition: all 0.2s; }";
  html += ".nav-link:hover { background: #4b5563; }";
  html += ".nav-link.active { background: #0ea5e9; color: white; }";
  html += ".debug-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1.5rem; margin-bottom: 1.5rem; }";
  html += ".debug-panel { background: #1e293b; border-radius: 8px; padding: 1.5rem; }";
  html += ".panel-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; padding-bottom: 0.75rem; border-bottom: 1px solid #334155; }";
  html += ".panel-title { font-size: 1.1rem; font-weight: 600; color: #f1f5f9; }";
  html += ".panel-count { background: #0ea5e9; color: white; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; }";
  html += ".receiver-list { max-height: 400px; overflow-y: auto; }";
  html += ".receiver-item { background: #0f172a; padding: 1rem; margin-bottom: 0.75rem; border-radius: 6px; border-left: 3px solid #0ea5e9; cursor: pointer; transition: all 0.2s; }";
  html += ".receiver-item:hover { background: #1e293b; transform: translateX(4px); }";
  html += ".receiver-item.selected { background: #1e293b; border-left-color: #fbbf24; }";
  html += ".receiver-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.75rem; }";
  html += ".receiver-id { font-weight: 600; color: #60a5fa; }";
  html += ".receiver-status { padding: 0.125rem 0.5rem; background: #059669; color: white; border-radius: 10px; font-size: 0.625rem; }";
  html += ".receiver-meta { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 0.75rem; margin-bottom: 0.75rem; font-size: 0.8rem; }";
  html += ".meta-item { text-align: center; }";
  html += ".meta-label { color: #94a3b8; margin-bottom: 0.25rem; }";
  html += ".meta-value { color: #f1f5f9; font-weight: 500; }";
  html += ".device-list { max-height: 200px; overflow-y: auto; background: #020617; border-radius: 4px; padding: 0.75rem; }";
  html += ".device-item { display: grid; grid-template-columns: 2fr 1fr 1fr 1fr; gap: 1rem; padding: 0.5rem 0; border-bottom: 1px solid #1e293b; font-size: 0.75rem; }";
  html += ".device-item:last-child { border-bottom: none; }";
  html += ".device-header { display: grid; grid-template-columns: 2fr 1fr 1fr 1fr; gap: 1rem; padding: 0.5rem 0; font-weight: 600; color: #94a3b8; border-bottom: 1px solid #334155; margin-bottom: 0.5rem; font-size: 0.75rem; }";
  html += ".device-id { color: #fbbf24; font-family: monospace; }";
  html += ".device-steps { color: #34d399; }";
  html += ".device-battery { color: #60a5fa; }";
  html += ".device-time { color: #a78bfa; }";
  html += ".controls { background: #1e293b; border-radius: 8px; padding: 1.5rem; margin-bottom: 1.5rem; }";
  html += ".controls h3 { margin-bottom: 1rem; color: #f1f5f9; }";
  html += ".control-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; }";
  html += ".control-item { text-align: center; }";
  html += ".control-button { width: 100%; padding: 0.75rem; background: #dc2626; color: white; border: none; border-radius: 6px; font-size: 0.875rem; cursor: pointer; transition: background 0.2s; }";
  html += ".control-button:hover { background: #b91c1c; }";
  html += ".control-button.refresh { background: #059669; }";
  html += ".control-button.refresh:hover { background: #047857; }";
  html += ".stats-bar { background: #1e293b; border-radius: 8px; padding: 1rem; display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 1rem; }";
  html += ".stat-item { text-align: center; }";
  html += ".stat-value { font-size: 1.5rem; font-weight: 700; color: #60a5fa; }";
  html += ".stat-label { font-size: 0.75rem; color: #94a3b8; margin-top: 0.25rem; }";
  html += ".empty-state { text-align: center; padding: 2rem; color: #64748b; }";
  html += ".loading { text-align: center; padding: 1rem; color: #64748b; }";
  html += "@media (max-width: 1024px) {";
  html += ".debug-grid { grid-template-columns: 1fr; }";
  html += ".control-grid { grid-template-columns: 1fr; }";
  html += ".stats-bar { grid-template-columns: repeat(2, 1fr); }";
  html += "}";
  html += "@media (max-width: 640px) {";
  html += ".receiver-meta { grid-template-columns: 1fr; gap: 0.5rem; }";
  html += ".device-item, .device-header { grid-template-columns: 1fr; gap: 0.25rem; text-align: center; }";
  html += ".stats-bar { grid-template-columns: 1fr; }";
  html += "}";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='header'>";
  html += "<h1>Debug Console</h1>";
  html += "<p>Detailed receiver and device monitoring</p>";
  html += "</div>";
  
  html += "<div class='container'>";
  
  html += "<div class='navigation'>";
  html += "<a href='/stepcounter' class='nav-link'>Main Dashboard</a>";
  html += "<a href='/debugger' class='nav-link active'>Debug Console</a>";
  html += "</div>";
  
  html += "<div class='stats-bar'>";
  html += "<div class='stat-item'>";
  html += "<div class='stat-value' id='totalStepsDebug'>0</div>";
  html += "<div class='stat-label'>Total Steps</div>";
  html += "</div>";
  html += "<div class='stat-item'>";
  html += "<div class='stat-value' id='totalDevicesDebug'>0</div>";
  html += "<div class='stat-label'>Total Devices</div>";
  html += "</div>";
  html += "<div class='stat-item'>";
  html += "<div class='stat-value' id='activeReceiversDebug'>0</div>";
  html += "<div class='stat-label'>Active Receivers</div>";
  html += "</div>";
  html += "<div class='stat-item'>";
  html += "<div class='stat-value' id='lastUpdateDebug'>--</div>";
  html += "<div class='stat-label'>Last Update</div>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='controls'>";
  html += "<h3>System Controls</h3>";
  html += "<div class='control-grid'>";
  html += "<div class='control-item'>";
  html += "<button class='control-button' onclick='resetAllData()'>Reset All Data</button>";
  html += "</div>";
  html += "<div class='control-item'>";
  html += "<button class='control-button refresh' onclick='refreshData()'>Refresh Data</button>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='debug-grid'>";
  html += "<div class='debug-panel'>";
  html += "<div class='panel-header'>";
  html += "<div class='panel-title'>Active Receivers</div>";
  html += "<div class='panel-count' id='receiverCount'>0</div>";
  html += "</div>";
  html += "<div class='receiver-list' id='receiverList'>";
  html += "<div class='loading'>Loading receiver data...</div>";
  html += "</div>";
  html += "</div>";
  html += "<div class='debug-panel'>";
  html += "<div class='panel-header'>";
  html += "<div class='panel-title'>Device Details</div>";
  html += "<div class='panel-count' id='deviceCount'>0</div>";
  html += "</div>";
  html += "<div id='deviceDetails'>";
  html += "<div class='loading'>Select a receiver to view devices</div>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "let selectedReceiver = null;";
  html += "function formatNumber(num) { return num.toLocaleString(); }";
  html += "function formatTime(seconds) {";
  html += "if (seconds < 60) return seconds + 's';";
  html += "if (seconds < 3600) return Math.floor(seconds/60) + 'm';";
  html += "return Math.floor(seconds/3600) + 'h';";
  html += "}";
  html += "function updateDebugDashboard() {";
  html += "fetch('/api/dashboard-data').then(r => r.json()).then(data => {";
  html += "document.getElementById('totalStepsDebug').textContent = formatNumber(data.totalSteps);";
  html += "document.getElementById('totalDevicesDebug').textContent = data.totalDevices;";
  html += "document.getElementById('activeReceiversDebug').textContent = data.activeReceivers;";
  html += "document.getElementById('lastUpdateDebug').textContent = new Date().toLocaleTimeString();";
  html += "}).catch(() => {";
  html += "document.getElementById('totalStepsDebug').textContent = 'Error';";
  html += "});";
  html += "}";
  html += "function updateReceivers() {";
  html += "fetch('/api/debug-data').then(r => r.json()).then(data => {";
  html += "document.getElementById('receiverCount').textContent = data.receivers.length;";
  html += "let html = '';";
  html += "if(data.receivers.length === 0) {";
  html += "html = '<div class=\"empty-state\">No receivers connected</div>';";
  html += "} else {";
  html += "data.receivers.forEach(receiver => {";
  html += "const isSelected = selectedReceiver === receiver.id;";
  html += "html += '<div class=\"receiver-item' + (isSelected ? ' selected' : '') + '\" onclick=\"selectReceiver(\\'' + receiver.id + '\\')\">';";
  html += "html += '<div class=\"receiver-header\">';";
  html += "html += '<div class=\"receiver-id\">' + receiver.id + '</div>';";
  html += "html += '<div class=\"receiver-status\">Online</div>';";
  html += "html += '</div>';";
  html += "html += '<div class=\"receiver-meta\">';";
  html += "html += '<div class=\"meta-item\"><div class=\"meta-label\">Devices</div><div class=\"meta-value\">' + receiver.deviceCount + '</div></div>';";
  html += "html += '<div class=\"meta-item\"><div class=\"meta-label\">Steps</div><div class=\"meta-value\">' + formatNumber(receiver.totalSteps) + '</div></div>';";
  html += "html += '<div class=\"meta-item\"><div class=\"meta-label\">Last Seen</div><div class=\"meta-value\">' + receiver.lastSeen + '</div></div>';";
  html += "html += '</div>';";
  html += "html += '</div>';";
  html += "});";
  html += "}";
  html += "document.getElementById('receiverList').innerHTML = html;";
  html += "}).catch(() => {";
  html += "document.getElementById('receiverList').innerHTML = '<div class=\"empty-state\">Connection Error</div>';";
  html += "});";
  html += "}";
  html += "function selectReceiver(receiverId) {";
  html += "selectedReceiver = receiverId;";
  html += "updateReceivers();";
  html += "updateDeviceDetails();";
  html += "}";
  html += "function updateDeviceDetails() {";
  html += "if (!selectedReceiver) {";
  html += "document.getElementById('deviceDetails').innerHTML = '<div class=\"loading\">Select a receiver to view devices</div>';";
  html += "return;";
  html += "}";
  html += "fetch('/api/debug-data').then(r => r.json()).then(data => {";
html += "const receiver = data.receivers.find(r => r.id === selectedReceiver);";
html += "if (!receiver || !receiver.devices || receiver.devices.length === 0) {";
html += "document.getElementById('deviceDetails').innerHTML = '<div class=\"empty-state\">No devices found for this receiver</div>';";
html += "document.getElementById('deviceCount').textContent = '0';";
html += "return;";
html += "}";
html += "document.getElementById('deviceCount').textContent = receiver.devices.length;";
html += "let html = '<div class=\"device-list\">';";
html += "html += '<div class=\"device-header\">';";
html += "html += '<div>Device ID</div>';";
html += "html += '<div>Steps</div>';";
html += "html += '<div>Battery</div>';";
html += "html += '<div>Last Seen</div>';";
html += "html += '</div>';";
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
html += "}).catch(() => {";
html += "document.getElementById('deviceDetails').innerHTML = '<div class=\"empty-state\">Error loading device data</div>';";
html += "});";
html += "}";
html += "function resetAllData() {";
html += "if(confirm('Reset ALL step data from ALL devices and receivers?\\nThis action cannot be undone!')) {";
html += "fetch('/api/reset', {method: 'POST'}).then(() => {";
html += "alert('All data has been reset successfully');";
html += "selectedReceiver = null;";
html += "refreshData();";
html += "}).catch(() => {";
html += "alert('Error resetting data');";
html += "});";
html += "}";
html += "}";
html += "function refreshData() {";
html += "updateDebugDashboard();";
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
  
  doc["totalSteps"] = totalStepsAllDevices;
  doc["totalDevices"] = allDevices.size();
  doc["activeReceivers"] = activeReceivers.size();
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

void handleDebugData() {
  DynamicJsonDocument doc(8192); // Larger buffer for detailed data
  JsonArray receivers = doc.createNestedArray("receivers");
  
  // Group devices by receiver with full details
  std::map<String, JsonArray> receiverDevices;
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
    receiver["fullId"] = receiverId;
    receiver["deviceCount"] = receiverDeviceCount[receiverId];
    receiver["totalSteps"] = receiverStepCount[receiverId];
    
    unsigned long timeSince = (millis() - pair.second) / 1000;
    receiver["lastSeen"] = String(timeSince) + "s ago";
    
    // Add device details for this receiver
    JsonArray devices = receiver.createNestedArray("devices");
    for (auto& devicePair : allDevices) {
      if (devicePair.second.receiverId == receiverId) {
        JsonObject device = devices.createNestedObject();
        device["deviceId"] = devicePair.first;
        device["stepCount"] = devicePair.second.stepCount;
        device["batteryLevel"] = devicePair.second.batteryLevel;
        
        unsigned long deviceTimeSince = (millis() - devicePair.second.lastSeen) / 1000;
        device["lastSeen"] = deviceTimeSince;
      }
    }
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
    Serial.print("Total Steps: ");
    Serial.println(totalStepsAllDevices);
    Serial.print("Active Devices: ");
    Serial.println(allDevices.size());
    Serial.print("Active Receivers: ");
    Serial.println(activeReceivers.size());
    Serial.print("Web Access: http://garba.local/stepcounter");
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
  
  Serial.println("\nALL DATA RESET");
  Serial.println("All devices, receivers, and step counts cleared");
  Serial.println("===================\n");
}