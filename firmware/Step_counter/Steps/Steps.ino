#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
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

// WiFi AP settings
const char* ssid = "StepCounter";
const char* password = "12345678";
const IPAddress apIP(192, 168, 4, 1);
const IPAddress netMask(255, 255, 255, 0);

ESP8266WebServer server(80);

// Configurable variables (you can modify these before uploading)
float magnitudeThreshold = 50.0;         // Direct magnitude threshold (raw values)
float xThreshold = 30.0;                 // Direct X threshold (raw values)
float yThreshold = 50.0;                 // Direct Y threshold (raw values)

// Rest position values (will be calibrated to raw ADXL345 values)
float restX = -32.0;  // Based on your readings
float restY = -264.0; // Based on your readings
float restZ = 0.0;    // Will be calibrated

// Step counting variables
unsigned long stepCount = 0;
bool stepDetected = false;
unsigned long lastStepTime = 0;
const unsigned long stepDelay = 300; // Minimum time between steps (ms)

// Sensor data
float currentX, currentY, currentZ;
float magnitude;
bool ledState = false;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off LED initially (inverted logic)
  
  // Initialize I2C
  Wire.begin();
  
  // Initialize ADXL345
  initADXL345();
  
  // Initialize EEPROM for storing configuration
  EEPROM.begin(512);
  loadConfiguration();
  
  // Calculate initial rest position (calibration)
  calibrateSensor();
  
  // Setup WiFi Access Point
  setupWiFi();
  
  // Setup web server routes
  setupWebServer();
  
  Serial.println("Step Counter initialized!");
  Serial.print("Access web interface at: http://");
  Serial.println(apIP);
}

void loop() {
  server.handleClient();
  
  // Read accelerometer data
  readADXL345();
  
  // Calculate magnitude using raw values
  magnitude = sqrt(currentX * currentX + currentY * currentY + currentZ * currentZ);
  
  // Check for step detection
  detectStep();
  
  // Handle LED blinking
  handleLED();
  
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
    
    // Store raw values directly (no conversion)
    currentX = x;
    currentY = y;
    currentZ = z;
  }
}

void calibrateSensor() {
  Serial.println("Calibrating sensor... Keep device still for 3 seconds");
  
  float sumX = 0, sumY = 0, sumZ = 0;
  int samples = 60; // 3 seconds at 50ms intervals
  
  for (int i = 0; i < samples; i++) {
    readADXL345();
    sumX += currentX;
    sumY += currentY;
    sumZ += currentZ;
    delay(50);
  }
  
  restX = sumX / samples;
  restY = sumY / samples;
  restZ = sumZ / samples;
  
  Serial.println("Calibration complete!");
  Serial.print("Rest position - X: "); Serial.print(restX);
  Serial.print(", Y: "); Serial.print(restY);
  Serial.print(", Z: "); Serial.println(restZ);
}

void detectStep() {
  if (millis() - lastStepTime < stepDelay) {
    return; // Too soon for another step
  }
  
  // Calculate differences from rest position
  float deltaX = abs(currentX - restX);
  float deltaY = abs(currentY - restY);
  float restMagnitude = sqrt(restX * restX + restY * restY + restZ * restZ);
  float deltaMagnitude = abs(magnitude - restMagnitude);
  
  // Step detection logic: BOTH magnitude AND (X OR Y) changes must exceed thresholds
  bool magnitudeStep = deltaMagnitude > magnitudeThreshold;
  bool positionStep = (deltaX > xThreshold) || (deltaY > yThreshold);
  
  if (magnitudeStep && positionStep) {
    stepCount++;
    stepDetected = true;
    lastStepTime = millis();
    
    Serial.print("Step detected! Count: "); Serial.print(stepCount);
    Serial.print(" | X: "); Serial.print(currentX);
    Serial.print(" | Y: "); Serial.print(currentY);
    Serial.print(" | Mag: "); Serial.print(magnitude);
    Serial.print(" | DeltaX: "); Serial.print(deltaX);
    Serial.print(" | DeltaY: "); Serial.print(deltaY);
    Serial.print(" | DeltaMag: "); Serial.println(deltaMagnitude);
  }
}

void handleLED() {
  if (stepDetected) {
    digitalWrite(LED_PIN, LOW); // Turn on LED (inverted logic)
    delay(100);
    digitalWrite(LED_PIN, HIGH); // Turn off LED
    stepDetected = false;
  }
}

void setupWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMask);
  WiFi.softAP(ssid, password);
  
  Serial.println("WiFi Access Point created");
  Serial.print("SSID: "); Serial.println(ssid);
  Serial.print("Password: "); Serial.println(password);
  Serial.print("IP: "); Serial.println(apIP);
}

void setupWebServer() {
  // Serve main page
  server.on("/", handleRoot);
  
  // API endpoints
  server.on("/api/data", handleGetData);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handleSetConfig);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/calibrate", HTTP_POST, handleCalibrate);
  
  server.begin();
  Serial.println("Web server started");
}

void handleRoot() {
  String html = "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";
  html += "<title>Step Counter</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; text-align: center; }";
  html += ".data-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 15px; margin: 20px 0; }";
  html += ".data-item { background: #e8f4fd; padding: 15px; border-radius: 8px; text-align: center; }";
  html += ".data-value { font-size: 24px; font-weight: bold; color: #2196F3; }";
  html += ".data-label { font-size: 12px; color: #666; margin-top: 5px; }";
  html += ".config-section { background: #f9f9f9; padding: 20px; border-radius: 8px; margin: 20px 0; }";
  html += ".config-item { margin: 15px 0; }";
  html += "label { display: block; margin-bottom: 5px; font-weight: bold; }";
  html += "input { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }";
  html += "button { background: #2196F3; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; margin: 5px; }";
  html += "button:hover { background: #1976D2; }";
  html += ".reset-btn { background: #f44336; }";
  html += ".reset-btn:hover { background: #d32f2f; }";
  html += ".step-counter { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 20px; border-radius: 10px; text-align: center; margin: 20px 0; }";
  html += ".step-counter .count { font-size: 48px; font-weight: bold; }";
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='container'>";
  html += "<h1>ADXL345 Step Counter</h1>";
  
  html += "<div class='step-counter'>";
  html += "<div class='count' id='stepCount'>0</div>";
  html += "<div>Steps Today</div>";
  html += "</div>";
  
  html += "<div class='data-grid'>";
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='xValue'>0</div>";
  html += "<div class='data-label'>X-Axis (raw)</div>";
  html += "</div>";
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='yValue'>0</div>";
  html += "<div class='data-label'>Y-Axis (raw)</div>";
  html += "</div>";
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='zValue'>0</div>";
  html += "<div class='data-label'>Z-Axis (raw)</div>";
  html += "</div>";
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='magnitude'>0</div>";
  html += "<div class='data-label'>Magnitude</div>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='config-section'>";
  html += "<h3>Configuration</h3>";
  
  html += "<div class='config-item'>";
  html += "<h4>Calibrated Rest Position:</h4>";
  html += "<div style='background: #e8f5e8; padding: 10px; border-radius: 5px; margin: 10px 0;'>";
  html += "X: <span id='restX'>-32</span> | ";
  html += "Y: <span id='restY'>-264</span> | ";
  html += "Z: <span id='restZ'>0</span>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='config-item'>";
  html += "<label for='magThreshold'>Magnitude Threshold:</label>";
  html += "<input type='number' id='magThreshold' min='1' step='0.1' value='50'>";
  html += "</div>";
  html += "<div class='config-item'>";
  html += "<label for='xThreshold'>X Threshold:</label>";
  html += "<input type='number' id='xThreshold' min='1' step='0.1' value='30'>";
  html += "</div>";
  html += "<div class='config-item'>";
  html += "<label for='yThreshold'>Y Threshold:</label>";
  html += "<input type='number' id='yThreshold' min='1' step='0.1' value='50'>";
  html += "</div>";
  
  html += "<button onclick='updateConfig()'>Update Configuration</button>";
  html += "<button onclick='calibrate()' style='background: #FF9800;'>Calibrate Rest Position</button>";
  html += "<button onclick='resetSteps()' class='reset-btn'>Reset Steps</button>";
  html += "</div>";
  html += "</div>";

  html += "<script>";
  html += "function updateData() {";
  html += "fetch('/api/data')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('stepCount').textContent = data.steps;";
  html += "document.getElementById('xValue').textContent = data.x.toFixed(0);";
  html += "document.getElementById('yValue').textContent = data.y.toFixed(0);";
  html += "document.getElementById('zValue').textContent = data.z.toFixed(0);";
  html += "document.getElementById('magnitude').textContent = data.magnitude.toFixed(0);";
  html += "});";
  html += "}";

  html += "function loadConfig() {";
  html += "fetch('/api/config')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('magThreshold').value = data.magnitudeThreshold;";
  html += "document.getElementById('xThreshold').value = data.xThreshold;";
  html += "document.getElementById('yThreshold').value = data.yThreshold;";
  html += "document.getElementById('restX').textContent = data.restX.toFixed(1);";
  html += "document.getElementById('restY').textContent = data.restY.toFixed(1);";
  html += "document.getElementById('restZ').textContent = data.restZ.toFixed(1);";
  html += "});";
  html += "}";

  html += "function updateConfig() {";
  html += "const config = {";
  html += "magnitudeThreshold: parseFloat(document.getElementById('magThreshold').value),";
  html += "xThreshold: parseFloat(document.getElementById('xThreshold').value),";
  html += "yThreshold: parseFloat(document.getElementById('yThreshold').value)";
  html += "};";

  html += "fetch('/api/config', {";
  html += "method: 'POST',";
  html += "headers: { 'Content-Type': 'application/json' },";
  html += "body: JSON.stringify(config)";
  html += "})";
  html += ".then(response => response.text())";
  html += ".then(data => alert('Configuration updated!'));";
  html += "}";

  html += "function calibrate() {";
  html += "if(confirm('Keep device still and click OK to calibrate rest position')) {";
  html += "fetch('/api/calibrate', { method: 'POST' })";
  html += ".then(response => response.text())";
  html += ".then(data => {";
  html += "alert('Calibration complete! ' + data);";
  html += "loadConfig();";
  html += "});";
  html += "}";
  html += "}";

  html += "function resetSteps() {";
  html += "if(confirm('Are you sure you want to reset the step counter?')) {";
  html += "fetch('/api/reset', { method: 'POST' })";
  html += ".then(response => response.text())";
  html += ".then(data => {";
  html += "alert('Steps reset!');";
  html += "updateData();";
  html += "});";
  html += "}";
  html += "}";

  html += "setInterval(updateData, 500);";
  html += "loadConfig();";
  html += "updateData();";
  html += "</script>";
  html += "</body>";
  html += "</html>";

  server.send(200, "text/html", html);
}

void handleGetData() {
  DynamicJsonDocument doc(200);
  doc["steps"] = stepCount;
  doc["x"] = currentX;  // Raw values
  doc["y"] = currentY;  // Raw values
  doc["z"] = currentZ;  // Raw values
  doc["magnitude"] = magnitude;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleGetConfig() {
  DynamicJsonDocument doc(300);
  doc["magnitudeThreshold"] = magnitudeThreshold;
  doc["xThreshold"] = xThreshold;
  doc["yThreshold"] = yThreshold;
  doc["restX"] = restX;
  doc["restY"] = restY;
  doc["restZ"] = restZ;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSetConfig() {
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(200);
    deserializeJson(doc, server.arg("plain"));
    
    magnitudeThreshold = doc["magnitudeThreshold"];
    xThreshold = doc["xThreshold"];
    yThreshold = doc["yThreshold"];
    
    // Constrain minimum values only
    magnitudeThreshold = max(magnitudeThreshold, 1.0f);
    xThreshold = max(xThreshold, 1.0f);
    yThreshold = max(yThreshold, 1.0f);
    
    saveConfiguration();
    
    Serial.println("Configuration updated:");
    Serial.print("Mag Threshold: "); Serial.println(magnitudeThreshold);
    Serial.print("X Threshold: "); Serial.println(xThreshold);
    Serial.print("Y Threshold: "); Serial.println(yThreshold);
    
    server.send(200, "text/plain", "Configuration updated");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleReset() {
  stepCount = 0;
  server.send(200, "text/plain", "Steps reset");
  Serial.println("Step counter reset");
}

void handleCalibrate() {
  Serial.println("Calibrating sensor via web interface...");
  
  float sumX = 0, sumY = 0, sumZ = 0;
  int samples = 20; // 1 second at 50ms intervals
  
  for (int i = 0; i < samples; i++) {
    readADXL345();
    sumX += currentX;
    sumY += currentY;
    sumZ += currentZ;
    delay(50);
  }
  
  restX = sumX / samples;
  restY = sumY / samples;
  restZ = sumZ / samples;
  
  String response = "Rest position: X=" + String(restX) + " Y=" + String(restY) + " Z=" + String(restZ);
  
  Serial.println("Calibration complete!");
  Serial.print("Rest position - X: "); Serial.print(restX);
  Serial.print(", Y: "); Serial.print(restY);
  Serial.print(", Z: "); Serial.println(restZ);
  
  server.send(200, "text/plain", response);
}

void saveConfiguration() {
  EEPROM.put(0, magnitudeThreshold);
  EEPROM.put(4, xThreshold);
  EEPROM.put(8, yThreshold);
  EEPROM.put(12, restX);
  EEPROM.put(16, restY);
  EEPROM.put(20, restZ);
  EEPROM.commit();
}

void loadConfiguration() {
  float tempMagThreshold, tempXThreshold, tempYThreshold;
  float tempRestX, tempRestY, tempRestZ;
  
  EEPROM.get(0, tempMagThreshold);
  EEPROM.get(4, tempXThreshold);
  EEPROM.get(8, tempYThreshold);
  EEPROM.get(12, tempRestX);
  EEPROM.get(16, tempRestY);
  EEPROM.get(20, tempRestZ);
  
  // Validate loaded values
  if (tempMagThreshold >= 1) {
    magnitudeThreshold = tempMagThreshold;
  }
  if (tempXThreshold >= 1) {
    xThreshold = tempXThreshold;
  }
  if (tempYThreshold >= 1) {
    yThreshold = tempYThreshold;
  }
  if (tempRestX > -1000 && tempRestX < 1000) {
    restX = tempRestX;
  }
  if (tempRestY > -1000 && tempRestY < 1000) {
    restY = tempRestY;
  }
  if (tempRestZ > -1000 && tempRestZ < 1000) {
    restZ = tempRestZ;
  }
}