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
String selectedAxis = "Y";        // Which axis to monitor: "X", "Y", or "Z"
float magnitudeThreshold = 50.0;  // Magnitude threshold (raw values)
float axisThreshold = 50.0;       // Selected axis threshold (raw values)

// Rest position values (will be calibrated to raw ADXL345 values)
float restX = -32.0;   // Based on your readings
float restY = -264.0;  // Based on your readings
float restZ = 0.0;     // Will be calibrated

// Step counting variables
unsigned long stepCount = 0;
bool stepDetected = false;
unsigned long lastStepTime = 0;
const unsigned long stepDelay = 300;  // Minimum time between steps (ms)

// Sensor data
float currentX, currentY, currentZ;
float magnitude;
float deltaSelectedAxis, deltaMagnitude;  // For web display
bool ledState = false;

// Enhanced step detection variables
float minStepInterval = 200;                 // Minimum time between steps (ms)
float maxStepInterval = 2000;                // Maximum time between steps (ms)
const int FILTER_SIZE = 5;                   // Moving average filter size
float yReadings[FILTER_SIZE];                // Circular buffer for Y-axis
float magReadings[FILTER_SIZE];              // Circular buffer for magnitude
int readingIndex = 0;                        // Current position in buffer
float filteredY = 0, filteredMagnitude = 0;  // Filtered values
bool filterInitialized = false;              // Track if filter is ready

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // Turn off LED initially (inverted logic)

  // Initialize I2C
  Wire.begin();

  // Initialize ADXL345
  initADXL345();

  // Initialize EEPROM for storing configuration
  EEPROM.begin(512);
  loadConfiguration();

  // Calculate initial rest position (calibration)
  calibrateSensor();

  for (int i = 0; i < FILTER_SIZE; i++) {
    yReadings[i] = restY;
    magReadings[i] = sqrt(restX * restX + restY * restY + restZ * restZ);
  }
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

  // Apply moving average filter
  applyMovingAverageFilter();

  // Check for step detection (now uses filtered values)
  detectStep();

  // Handle LED blinking
  handleLED();

  delay(50);  // Small delay for stability
}

float xReadings[FILTER_SIZE];
float zReadings[FILTER_SIZE]; 
float filteredX = 0, filteredZ = 0;

void applyMovingAverageFilter() {
    // Add new readings to circular buffers for ALL axes
    xReadings[readingIndex] = currentX;
    yReadings[readingIndex] = currentY;
    zReadings[readingIndex] = currentZ;
    magReadings[readingIndex] = magnitude;

    // Calculate moving averages for ALL axes
    float xSum = 0, ySum = 0, zSum = 0, magSum = 0;
    for (int i = 0; i < FILTER_SIZE; i++) {
        xSum += xReadings[i];
        ySum += yReadings[i];
        zSum += zReadings[i];
        magSum += magReadings[i];
    }

    filteredX = xSum / FILTER_SIZE;
    filteredY = ySum / FILTER_SIZE;
    filteredZ = zSum / FILTER_SIZE;
    filteredMagnitude = magSum / FILTER_SIZE;

    // Move to next position
    readingIndex = (readingIndex + 1) % FILTER_SIZE;

    // Mark filter as ready after first full cycle
    if (readingIndex == 0 && !filterInitialized) {
        filterInitialized = true;
    }
}

// Helper function for consistent filtered value access
float getFilteredValue(char axis) {
    switch(axis) {
        case 'X': return filteredX;
        case 'Y': return filteredY; 
        case 'Z': return filteredZ;
        default: return filteredY;
    }
}
void initADXL345() {
  // Set power control register
  Wire.beginTransmission(ADXL345_ADDRESS);
  Wire.write(ADXL345_POWER_CTL);
  Wire.write(0x08);  // Enable measurement
  Wire.endTransmission();

  // Set data format register
  Wire.beginTransmission(ADXL345_ADDRESS);
  Wire.write(ADXL345_DATA_FORMAT);
  Wire.write(0x08);  // Full resolution, +/-2g
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
  int samples = 60;  // 3 seconds at 50ms intervals

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
  Serial.print("Rest position - X: ");
  Serial.print(restX);
  Serial.print(", Y: ");
  Serial.print(restY);
  Serial.print(", Z: ");
  Serial.println(restZ);
}

void detectStep() {
  unsigned long currentTime = millis();

  // Skip detection if filter not ready
  if (!filterInitialized) {
    return;
  }

  // Minimum step interval check - prevents too-fast false detections
  if (currentTime - lastStepTime < minStepInterval) {
    return;
  }

  // Calculate filtered deltas
  float deltaY = abs(filteredY - restY);
  float restMagnitude = sqrt(restX * restX + restY * restY + restZ * restZ);
  float deltaMagnitude = abs(filteredMagnitude - restMagnitude);

  // Get selected axis delta (using filtered values)
  float selectedAxisDelta = 0;
  if (selectedAxis == "X") {
    selectedAxisDelta = abs(currentX - restX);  // X not filtered in this example
  } else if (selectedAxis == "Y") {
    selectedAxisDelta = deltaY;  // Use filtered Y
  } else if (selectedAxis == "Z") {
    selectedAxisDelta = abs(currentZ - restZ);  // Z not filtered in this example
  }

  // Enhanced step detection with multiple criteria
  bool magnitudeStep = deltaMagnitude > magnitudeThreshold;
  bool axisStep = selectedAxisDelta > axisThreshold;
  bool timingValid = (currentTime - lastStepTime) >= minStepInterval;

  // Additional validation: Check for sustained movement (not just a spike)
  static int consecutiveHighReadings = 0;
  if (axisStep && magnitudeStep) {
    consecutiveHighReadings++;
  } else {
    consecutiveHighReadings = 0;
  }

  // Step detected when ALL conditions are met
  if (magnitudeStep && axisStep && timingValid && consecutiveHighReadings >= 4) {
    stepCount++;
    stepDetected = true;
    lastStepTime = currentTime;
    consecutiveHighReadings = 0;  // Reset counter

    Serial.print("Step detected! Count: ");
    Serial.print(stepCount);
    Serial.print(" | FilteredY: ");
    Serial.print(filteredY);
    Serial.print(" | DeltaY: ");
    Serial.print(deltaY);
    Serial.print(" | DeltaMag: ");
    Serial.print(deltaMagnitude);
    Serial.print(" | TimeSinceLastStep: ");
    Serial.print(currentTime - lastStepTime);
    Serial.println("ms");
  }

  // Update display deltas with filtered values
  deltaSelectedAxis = selectedAxisDelta;
  deltaMagnitude = deltaMagnitude;
}

void handleLED() {
  if (stepDetected) {
    digitalWrite(LED_PIN, LOW);  // Turn on LED (inverted logic)
    delay(100);
    digitalWrite(LED_PIN, HIGH);  // Turn off LED
    stepDetected = false;
  }
}

void setupWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMask);
  WiFi.softAP(ssid, password);

  Serial.println("WiFi Access Point created");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("Password: ");
  Serial.println(password);
  Serial.print("IP: ");
  Serial.println(apIP);
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
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='deltaAxis'>0</div>";
  html += "<div class='data-label'>Delta <span id='axisLabel'>Y</span></div>";
  html += "</div>";
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='deltaMagnitude'>0</div>";
  html += "<div class='data-label'>Delta Magnitude</div>";
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
  html += "<label for='selectedAxis'>Monitor Axis for Steps:</label>";
  html += "<select id='selectedAxis' style='width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box;'>";
  html += "<option value='X'>X-Axis</option>";
  html += "<option value='Y' selected>Y-Axis</option>";
  html += "<option value='Z'>Z-Axis</option>";
  html += "</select>";
  html += "</div>";

  html += "<div class='config-item'>";
  html += "<label for='magThreshold'>Magnitude Threshold:</label>";
  html += "<input type='number' id='magThreshold' min='1' step='0.1' value='50'>";
  html += "</div>";
  html += "<div class='config-item'>";
  html += "<label for='axisThreshold'>Selected Axis Threshold:</label>";
  html += "<input type='number' id='axisThreshold' min='1' step='0.1' value='50'>";
  html += "</div>";
  
html += "<div class='config-item'>";
html += "<label for='minStepInterval'>Minimum Step Interval (ms):</label>";
html += "<input type='number' id='minStepInterval' min='100' max='1000' step='10' value='200'>";
html += "<div style='font-size: 12px; color: #666; margin-top: 2px;'>Minimum time between steps (100-1000ms)</div>";
html += "</div>";

html += "<div class='config-item'>";
html += "<label for='maxStepInterval'>Maximum Step Interval (ms):</label>";
html += "<input type='number' id='maxStepInterval' min='1000' max='5000' step='50' value='2000'>";
html += "<div style='font-size: 12px; color: #666; margin-top: 2px;'>Maximum time between steps (1000-5000ms)</div>";
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
  html += "document.getElementById('deltaAxis').textContent = data.deltaAxis.toFixed(1);";
  html += "document.getElementById('deltaMagnitude').textContent = data.deltaMagnitude.toFixed(1);";
  html += "document.getElementById('axisLabel').textContent = data.selectedAxis;";
  html += "});";
  html += "}";

  html += "function loadConfig() {";
  html += "fetch('/api/config')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('selectedAxis').value = data.selectedAxis;";
  html += "document.getElementById('magThreshold').value = data.magnitudeThreshold;";
  html += "document.getElementById('axisThreshold').value = data.axisThreshold;";
  html += "document.getElementById('minStepInterval').value = data.minStepInterval || 200;";   // Default fallback
  html += "document.getElementById('maxStepInterval').value = data.maxStepInterval || 2000;";  // Default fallback
  html += "document.getElementById('restX').textContent = data.restX.toFixed(1);";
  html += "document.getElementById('restY').textContent = data.restY.toFixed(1);";
  html += "document.getElementById('restZ').textContent = data.restZ.toFixed(1);";
  html += "document.getElementById('axisLabel').textContent = data.selectedAxis;";
  html += "});";
  html += "}";

  html += "function updateConfig() {";
  html += "const config = {";
  html += "selectedAxis: document.getElementById('selectedAxis').value,";
  html += "magnitudeThreshold: parseFloat(document.getElementById('magThreshold').value),";
  html += "axisThreshold: parseFloat(document.getElementById('axisThreshold').value),";
  html += "minStepInterval: parseFloat(document.getElementById('minStepInterval').value),";
  html += "maxStepInterval: parseFloat(document.getElementById('maxStepInterval').value)";
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
  DynamicJsonDocument doc(300);
  doc["steps"] = stepCount;
  doc["x"] = currentX;  // Raw values
  doc["y"] = currentY;  // Raw values
  doc["z"] = currentZ;  // Raw values
  doc["magnitude"] = magnitude;
  doc["deltaAxis"] = deltaSelectedAxis;
  doc["deltaMagnitude"] = deltaMagnitude;
  doc["selectedAxis"] = selectedAxis;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleGetConfig() {
  DynamicJsonDocument doc(400);  // Increase size
  doc["selectedAxis"] = selectedAxis;
  doc["magnitudeThreshold"] = magnitudeThreshold;
  doc["axisThreshold"] = axisThreshold;
  doc["minStepInterval"] = minStepInterval;
  doc["maxStepInterval"] = maxStepInterval;
  doc["restX"] = restX;
  doc["restY"] = restY;
  doc["restZ"] = restZ;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}
void handleSetConfig() {
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(400);
    deserializeJson(doc, server.arg("plain"));

    selectedAxis = doc["selectedAxis"].as<String>();
    magnitudeThreshold = doc["magnitudeThreshold"];
    axisThreshold = doc["axisThreshold"];

    if (doc.containsKey("minStepInterval")) {
      minStepInterval = doc["minStepInterval"];
      minStepInterval = constrain(minStepInterval, 100, 1000);
    }
    if (doc.containsKey("maxStepInterval")) {
      maxStepInterval = doc["maxStepInterval"];
      maxStepInterval = constrain(maxStepInterval, 1000, 5000);
    }
    // Constrain minimum values only
    magnitudeThreshold = max(magnitudeThreshold, 1.0f);
    axisThreshold = max(axisThreshold, 1.0f);

    // Validate axis selection
    if (selectedAxis != "X" && selectedAxis != "Y" && selectedAxis != "Z") {
      selectedAxis = "Y";  // Default to Y
    }

    saveConfiguration();

    Serial.println("Configuration updated:");
    Serial.print("Selected Axis: ");
    Serial.println(selectedAxis);
    Serial.print("Mag Threshold: ");
    Serial.println(magnitudeThreshold);
    Serial.print("Axis Threshold: ");
    Serial.println(axisThreshold);

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
  int samples = 20;  // 1 second at 50ms intervals

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
  Serial.print("Rest position - X: ");
  Serial.print(restX);
  Serial.print(", Y: ");
  Serial.print(restY);
  Serial.print(", Z: ");
  Serial.println(restZ);

  server.send(200, "text/plain", response);
}

void saveConfiguration() {
  EEPROM.put(0, magnitudeThreshold);
  EEPROM.put(4, axisThreshold);
  EEPROM.put(8, minStepInterval);
  EEPROM.put(12, maxStepInterval);
  EEPROM.put(16, restX);
  EEPROM.put(20, restY);
  EEPROM.put(24, restZ);
  EEPROM.put(28, selectedAxis.c_str()[0]); // Store first character of axis
  EEPROM.commit();
}

void loadConfiguration() {
  float tempMagThreshold, tempAxisThreshold, tempMinStep, tempMaxStep;
  float tempRestX, tempRestY, tempRestZ;
  char tempAxis;
  
  EEPROM.get(0, tempMagThreshold);
  EEPROM.get(4, tempAxisThreshold);
  EEPROM.get(8, tempMinStep);
  EEPROM.get(12, tempMaxStep);
  EEPROM.get(16, tempRestX);
  EEPROM.get(20, tempRestY);
  EEPROM.get(24, tempRestZ);
  EEPROM.get(28, tempAxis);
  
  // Validate loaded values
  if (tempMagThreshold >= 1) {
    magnitudeThreshold = tempMagThreshold;
  }
  if (tempAxisThreshold >= 1) {
    axisThreshold = tempAxisThreshold;
  }
  if (tempMinStep >= 100 && tempMinStep <= 1000) {
    minStepInterval = tempMinStep;
  }
  if (tempMaxStep >= 1000 && tempMaxStep <= 5000) {
    maxStepInterval = tempMaxStep;
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
  if (tempAxis == 'X' || tempAxis == 'Y' || tempAxis == 'Z') {
    selectedAxis = String(tempAxis);
  }
}