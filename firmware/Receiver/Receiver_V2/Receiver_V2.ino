#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <espnow.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <map>

// WiFi credentials
const char* ssid = "Anantkaal_4G";
const char* password = "Setupdev@123";

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

void setup() {
  Serial.begin(115200);
  
  // Get receiver ID from MAC address
  receiverId = WiFi.macAddress();
  
  // Initialize ESP-NOW first
  initESPNow();
  
  // Connect to WiFi
  connectToWiFi();
  
  Serial.println("\n=== ESP8266 Step Counter Receiver ===");
  Serial.print("Receiver ID: ");
  Serial.println(receiverId);
  Serial.print("Connected to: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Server: ");
  Serial.println(serverHost);
  Serial.println("Ready to receive ESP-NOW data and forward to server");
  Serial.println("=====================================");
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
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" Connected!");
  } else {
    Serial.println(" Failed!");
    delay(5000); // Wait before retry
  }
}

void sendDataToServer() {
  if (WiFi.status() != WL_CONNECTED || deviceSteps.empty()) {
    return;
  }
  
  // Create JSON payload with all device data
  DynamicJsonDocument doc(2048); // Larger buffer for multiple devices
  doc["receiverId"] = receiverId;
  doc["timestamp"] = millis();
  doc["deviceCount"] = deviceSteps.size();
  
  JsonArray devices = doc.createNestedArray("devices");
  
  for (auto& pair : deviceSteps) {
    JsonObject device = devices.createNestedObject();
    device["deviceId"] = pair.first;
    device["stepCount"] = pair.second;
    device["batteryLevel"] = deviceBattery[pair.first];
    device["lastSeen"] = (millis() - deviceLastSeen[pair.first]) / 1000; // seconds ago
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