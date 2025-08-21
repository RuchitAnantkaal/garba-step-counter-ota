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

// Movement states for step detection
enum MovementState {
  STATE_REST,
  STATE_FORWARD
};

// Configurable variables (you can modify these before uploading)
String selectedAxis = "Y";                   // Which axis to monitor: "X", "Y", or "Z"
float magnitudeThreshold = 50.0;             // Magnitude threshold (raw values)
unsigned long maxPatternTime = 2000;         // Max time for step pattern (ms)

// Calibrated movement positions (only Rest and Forward needed)
float restPosition = -247.5;                 // Rest position
float forwardPosition = -169.4;              // Forward position
bool isCalibrated = false;

// Step counting variables
unsigned long stepCount = 0;
bool stepDetected = false;
unsigned long lastStepTime = 0;
const unsigned long stepDelay = 300; // Minimum time between steps (ms)

// Movement pattern detection
MovementState currentState = STATE_REST;
unsigned long stateStartTime = 0;
bool hasMovedToForward = false;
bool hasReturnedToRest = false;

// Sensor data
float currentX, currentY, currentZ;
float magnitude;
float deltaSelectedAxis, deltaMagnitude;
float selectedAxisValue;
String currentStateStr = "REST";

// Calibration variables
bool calibrationMode = false;
String calibrationStep = "IDLE"; // IDLE, RELAX, REST_CALIBRATING, REST_DONE, FORWARD, FORWARD_CALIBRATING, COMPLETE
unsigned long calibrationStartTime = 0;
unsigned long calibrationDisplayTime = 0;
const unsigned long calibrationDuration = 1000; // 1 second per position
const unsigned long displayDuration = 2000; // 2 seconds display time
float calibrationSum = 0;
int calibrationSamples = 0;

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
  
  // Setup WiFi Access Point
  setupWiFi();
  
  // Setup web server routes
  setupWebServer();
  
  Serial.println("Smart Step Counter (Forward-Only) initialized!");
  Serial.print("Access web interface at: http://");
  Serial.println(apIP);
}

void loop() {
  server.handleClient();
  
  // Read accelerometer data
  readADXL345();
  
  // Calculate magnitude using raw values
  magnitude = sqrt(currentX * currentX + currentY * currentY + currentZ * currentZ);
  
  // Calculate deltas for web display
  float deltaX = abs(currentX - restPosition);
  float deltaY = abs(currentY - restPosition);
  float deltaZ = abs(currentZ - restPosition);
  float restMagnitude = sqrt(restPosition * restPosition + restPosition * restPosition + 0);
  deltaMagnitude = abs(magnitude - restMagnitude);
  
  // Get the selected axis values
  if (selectedAxis == "X") {
    deltaSelectedAxis = deltaX;
    selectedAxisValue = currentX;
  } else if (selectedAxis == "Y") {
    deltaSelectedAxis = deltaY;
    selectedAxisValue = currentY;
  } else if (selectedAxis == "Z") {
    deltaSelectedAxis = deltaZ;
    selectedAxisValue = currentZ;
  }
  
  // Handle calibration if active
  if (calibrationMode) {
    handleCalibration();
  } else if (isCalibrated) {
    // Only detect steps if calibrated
    detectStepPattern();
  }
  
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

void handleCalibration() {
  if (calibrationStep == "IDLE") return;
  
  // Handle display timing for completion messages
  if (calibrationStep == "REST_DONE") {
    if (millis() - calibrationDisplayTime >= displayDuration) {
      calibrationStep = "FORWARD";
      Serial.println("Ready for FORWARD calibration");
    }
    return;
  }
  
  // Handle active calibration readings
  if (calibrationStep == "REST_CALIBRATING" || calibrationStep == "FORWARD_CALIBRATING") {
    // Add current reading to calibration sum
    calibrationSum += selectedAxisValue;
    calibrationSamples++;
    
    // Debug output every 10 samples
    if (calibrationSamples % 10 == 0) {
      Serial.print("Calibrating "); Serial.print(calibrationStep);
      Serial.print(" - Sample "); Serial.print(calibrationSamples);
      Serial.print("/20, Current value: "); Serial.print(selectedAxisValue);
      Serial.print(", Running avg: "); Serial.println(calibrationSum / calibrationSamples);
    }
    
    // Check if 1 second has passed
    if (millis() - calibrationStartTime >= calibrationDuration) {
      float average = calibrationSum / calibrationSamples;
      
      if (calibrationStep == "REST_CALIBRATING") {
        restPosition = average;
        calibrationStep = "REST_DONE";
        calibrationDisplayTime = millis();
        Serial.print("REST calibrated to: "); Serial.println(restPosition);
      } else if (calibrationStep == "FORWARD_CALIBRATING") {
        forwardPosition = average;
        calibrationStep = "COMPLETE";
        calibrationMode = false;
        isCalibrated = true;
        saveConfiguration();
        Serial.print("FORWARD calibrated to: "); Serial.println(forwardPosition);
        Serial.println("Calibration COMPLETE! Ready to count steps.");
        Serial.print("Movement range: "); Serial.print(abs(forwardPosition - restPosition));
        Serial.println(" units");
      }
      
      // Reset for next step
      calibrationSum = 0;
      calibrationSamples = 0;
    }
  }
}

void detectStepPattern() {
  // Check if magnitude is sufficient (increased threshold for less sensitivity)
  if (deltaMagnitude < magnitudeThreshold) {
    return;
  }
  
  // Add minimum movement threshold to ignore small vibrations
  float minMovementThreshold = 20.0; // Ignore movements smaller than this
  if (abs(selectedAxisValue - restPosition) < minMovementThreshold) {
    return;
  }
  
  // Calculate dynamic thresholds based on calibrated range
  float movementRange = abs(forwardPosition - restPosition);
  float forwardThreshold = movementRange * 0.5; // 50% of calibrated range to trigger forward
  float restThreshold = movementRange * 0.3;    // 30% of calibrated range to return to rest
  
  // Ensure minimum thresholds
  forwardThreshold = max(forwardThreshold, 25.0f);
  restThreshold = max(restThreshold, 15.0f);
  
  // Determine current movement state
  MovementState newState = STATE_REST;
  
  // Check if we're in forward position (direction-aware)
  if (forwardPosition < restPosition) {
    // Forward is more negative
    if (selectedAxisValue <= (restPosition - forwardThreshold)) {
      newState = STATE_FORWARD;
    }
  } else {
    // Forward is more positive
    if (selectedAxisValue >= (restPosition + forwardThreshold)) {
      newState = STATE_FORWARD;
    }
  }
  
  // Update state string for display
  switch (newState) {
    case STATE_REST: currentStateStr = "REST"; break;
    case STATE_FORWARD: currentStateStr = "FORWARD"; break;
  }
  
  // Detect state transitions for step counting
  if (newState != currentState) {
    Serial.print("State change: "); 
    Serial.print(currentState); 
    Serial.print(" -> "); 
    Serial.print(newState);
    Serial.print(" | Value: "); Serial.print(selectedAxisValue);
    Serial.print(" | Rest: "); Serial.print(restPosition);
    Serial.print(" | Forward: "); Serial.print(forwardPosition);
    Serial.print(" | F_thresh: "); Serial.println(forwardThreshold);
    
    // Track movement pattern: Rest -> Forward -> Rest = 1 Step
    if (currentState == STATE_REST && newState == STATE_FORWARD) {
      hasMovedToForward = true;
      Serial.println("✓ Moved to FORWARD position");
    }
    
    if (currentState == STATE_FORWARD && newState == STATE_REST && hasMovedToForward) {
      hasReturnedToRest = true;
      Serial.println("✓ Returned to REST position");
      
      // Complete step pattern detected: Rest -> Forward -> Rest
      if (millis() - lastStepTime >= stepDelay) {
        stepCount++;
        stepDetected = true;
        lastStepTime = millis();
        
        Serial.print("🎯 STEP DETECTED! Count: "); Serial.print(stepCount);
        Serial.print(" | Pattern: REST→FORWARD→REST completed");
        Serial.println();
        
        // Reset pattern flags
        hasMovedToForward = false;
        hasReturnedToRest = false;
      }
    }
    
    currentState = newState;
    stateStartTime = millis();
  }
  
  // Reset pattern if it takes too long
  if (millis() - stateStartTime > maxPatternTime) {
    hasMovedToForward = false;
    hasReturnedToRest = false;
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
  server.on("/api/start-calibration", HTTP_POST, handleStartCalibration);
  server.on("/api/next-calibration", HTTP_POST, handleNextCalibration);
  
  server.begin();
  Serial.println("Web server started");
}

void handleRoot() {
  String html = "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";
  html += "<title>Smart Step Counter</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; text-align: center; }";
  html += ".data-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 15px; margin: 20px 0; }";
  html += ".data-item { background: #e8f4fd; padding: 15px; border-radius: 8px; text-align: center; }";
  html += ".data-value { font-size: 24px; font-weight: bold; color: #2196F3; }";
  html += ".data-label { font-size: 12px; color: #666; margin-top: 5px; }";
  html += ".state-item { background: #fff3e0; border-left: 4px solid #ff9800; }";
  html += ".config-section { background: #f9f9f9; padding: 20px; border-radius: 8px; margin: 20px 0; }";
  html += ".config-item { margin: 15px 0; }";
  html += "label { display: block; margin-bottom: 5px; font-weight: bold; }";
  html += "input, select { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }";
  html += "button { background: #2196F3; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; margin: 5px; }";
  html += "button:hover { background: #1976D2; }";
  html += ".reset-btn { background: #f44336; }";
  html += ".reset-btn:hover { background: #d32f2f; }";
  html += ".calibrate-btn { background: #FF9800; }";
  html += ".calibrate-btn:hover { background: #F57C00; }";
  html += ".step-counter { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 20px; border-radius: 10px; text-align: center; margin: 20px 0; }";
  html += ".step-counter .count { font-size: 48px; font-weight: bold; }";
  html += ".calibration-section { background: #e8f5e8; padding: 15px; border-radius: 8px; margin: 15px 0; border: 2px solid #4CAF50; }";
  html += ".calibration-step { font-size: 18px; font-weight: bold; color: #2E7D32; text-align: center; margin: 10px 0; }";
  html += ".calibration-values { background: #f1f8e9; padding: 10px; border-radius: 5px; font-family: monospace; }";
  html += ".movement-range { background: #e3f2fd; padding: 10px; border-radius: 5px; margin: 10px 0; text-align: center; color: #1976d2; font-weight: bold; }";
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='container'>";
  html += "<h1>Smart Step Counter</h1>";
  html += "<p style='text-align: center; color: #666;'>Forward-Only Detection System</p>";
  
  html += "<div class='step-counter'>";
  html += "<div class='count' id='stepCount'>0</div>";
  html += "<div>Steps Detected</div>";
  html += "</div>";
  
  html += "<div class='data-grid'>";
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='selectedAxisValue'>0</div>";
  html += "<div class='data-label'><span id='axisLabel'>Y</span>-Axis Value</div>";
  html += "</div>";
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='magnitude'>0</div>";
  html += "<div class='data-label'>Magnitude</div>";
  html += "</div>";
  html += "<div class='data-item'>";
  html += "<div class='data-value' id='deltaAxis'>0</div>";
  html += "<div class='data-label'>Delta <span id='axisLabel2'>Y</span></div>";
  html += "</div>";
  html += "<div class='data-item state-item'>";
  html += "<div class='data-value' id='currentState'>REST</div>";
  html += "<div class='data-label'>Movement State</div>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='config-section'>";
  html += "<h3>Movement Calibration</h3>";
  
  html += "<div class='calibration-section' id='calibrationSection'>";
  html += "<div class='calibration-step' id='calibrationStatus'>Ready for Movement Calibration</div>";
  html += "<div class='calibration-values' id='calibrationValues'>";
  html += "Rest Position: <span id='restPos'>Not calibrated</span><br>";
  html += "Forward Position: <span id='forwardPos'>Not calibrated</span>";
  html += "</div>";
  html += "<div class='movement-range' id='movementRange'>Movement Range: Not calibrated</div>";
  html += "<button onclick='handleCalibrationButton()' class='calibrate-btn' id='calibrateBtn'>Start Calibration</button>";
  html += "</div>";
  
  html += "<div class='config-item'>";
  html += "<label for='selectedAxis'>Monitor Axis:</label>";
  html += "<select id='selectedAxis'>";
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
  html += "<label for='maxPatternTime'>Max Pattern Time (ms):</label>";
  html += "<input type='number' id='maxPatternTime' min='500' max='5000' step='100' value='2000'>";
  html += "</div>";
  
  html += "<button onclick='updateConfig()'>Update Configuration</button>";
  html += "<button onclick='resetSteps()' class='reset-btn'>Reset Steps</button>";
  html += "</div>";
  html += "</div>";

  html += "<script>";
  html += "let restPos = 0, forwardPos = 0;";

  html += "function updateData() {";
  html += "fetch('/api/data')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('stepCount').textContent = data.steps;";
  html += "document.getElementById('selectedAxisValue').textContent = data.selectedAxisValue.toFixed(1);";
  html += "document.getElementById('magnitude').textContent = data.magnitude.toFixed(0);";
  html += "document.getElementById('deltaAxis').textContent = data.deltaAxis.toFixed(1);";
  html += "document.getElementById('currentState').textContent = data.currentState;";
  html += "document.getElementById('axisLabel').textContent = data.selectedAxis;";
  html += "document.getElementById('axisLabel2').textContent = data.selectedAxis;";
  
  html += "if (data.calibrationStep && data.calibrationStep !== 'IDLE') {";
  html += "updateCalibrationStatus(data.calibrationStep);";
  html += "}";
  
  html += "if (data.restPosition) restPos = data.restPosition;";
  html += "if (data.forwardPosition) forwardPos = data.forwardPosition;";
  html += "updateCalibrationDisplay();";
  html += "})";
  html += ".catch(error => console.error('Error:', error));";
  html += "}";

  html += "function loadConfig() {";
  html += "fetch('/api/config')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('selectedAxis').value = data.selectedAxis;";
  html += "document.getElementById('magThreshold').value = data.magnitudeThreshold;";
  html += "document.getElementById('maxPatternTime').value = data.maxPatternTime;";
  html += "restPos = data.restPosition || 0;";
  html += "forwardPos = data.forwardPosition || 0;";
  html += "updateCalibrationDisplay();";
  html += "});";
  html += "}";

  html += "function updateCalibrationDisplay() {";
  html += "document.getElementById('restPos').textContent = restPos ? restPos.toFixed(1) : 'Not calibrated';";
  html += "document.getElementById('forwardPos').textContent = forwardPos ? forwardPos.toFixed(1) : 'Not calibrated';";
  html += "if (restPos && forwardPos) {";
  html += "let range = Math.abs(forwardPos - restPos).toFixed(1);";
  html += "document.getElementById('movementRange').textContent = 'Movement Range: ' + range + ' units';";
  html += "} else {";
  html += "document.getElementById('movementRange').textContent = 'Movement Range: Not calibrated';";
  html += "}";
  html += "}";

  html += "function updateCalibrationStatus(step) {";
  html += "const statusEl = document.getElementById('calibrationStatus');";
  html += "const btnEl = document.getElementById('calibrateBtn');";
  html += "switch(step) {";
  html += "case 'RELAX': statusEl.textContent = 'Relax - Hold device in rest position'; btnEl.textContent = 'Calibrate Rest'; btnEl.style.display = 'inline-block'; break;";
  html += "case 'REST_CALIBRATING': statusEl.textContent = 'Calibrating rest position... (1 sec)'; btnEl.style.display = 'none'; break;";
  html += "case 'REST_DONE': statusEl.textContent = 'Rest calibration done!'; btnEl.style.display = 'none'; break;";
  html += "case 'FORWARD': statusEl.textContent = 'Move hand forward and hold'; btnEl.textContent = 'Calibrate Forward'; btnEl.style.display = 'inline-block'; break;";
  html += "case 'FORWARD_CALIBRATING': statusEl.textContent = 'Calibrating forward position... (1 sec)'; btnEl.style.display = 'none'; break;";
  html += "case 'COMPLETE': statusEl.textContent = 'Calibration COMPLETE! Ready to count steps.'; btnEl.textContent = 'Start Calibration'; btnEl.style.display = 'inline-block'; break;";
  html += "default: statusEl.textContent = 'Ready for Movement Calibration'; btnEl.textContent = 'Start Calibration'; btnEl.style.display = 'inline-block'; break;";
  html += "}";
  html += "}";

  html += "function handleCalibrationButton() {";
  html += "const statusEl = document.getElementById('calibrationStatus');";
  html += "const currentText = statusEl.textContent;";
  
  html += "if (currentText.includes('Ready for Movement') || currentText.includes('COMPLETE')) {";
  html += "fetch('/api/start-calibration', { method: 'POST' });";
  html += "} else if (currentText.includes('Relax')) {";
  html += "fetch('/api/next-calibration', { method: 'POST' });";
  html += "} else if (currentText.includes('Move hand forward')) {";
  html += "fetch('/api/next-calibration', { method: 'POST' });";
  html += "}";
  html += "}";

  html += "function updateConfig() {";
  html += "const config = {";
  html += "selectedAxis: document.getElementById('selectedAxis').value,";
  html += "magnitudeThreshold: parseFloat(document.getElementById('magThreshold').value),";
  html += "maxPatternTime: parseInt(document.getElementById('maxPatternTime').value)";
  html += "};";
  html += "fetch('/api/config', {";
  html += "method: 'POST',";
  html += "headers: { 'Content-Type': 'application/json' },";
  html += "body: JSON.stringify(config)";
  html += "})";
  html += ".then(() => alert('Configuration updated!'));";
  html += "}";

  html += "function resetSteps() {";
  html += "if(confirm('Reset step counter?')) {";
  html += "fetch('/api/reset', { method: 'POST' })";
  html += ".then(() => updateData());";
  html += "}";
  html += "}";

  html += "setInterval(updateData, 400);";
  html += "loadConfig();";
  html += "updateData();";
  html += "</script>";
  html += "</body>";
  html += "</html>";

  server.send(200, "text/html", html);
}

void handleGetData() {
  DynamicJsonDocument doc(600);
  doc["steps"] = stepCount;
  doc["selectedAxisValue"] = selectedAxisValue;
  doc["magnitude"] = magnitude;
  doc["deltaAxis"] = deltaSelectedAxis;
  doc["deltaMagnitude"] = deltaMagnitude;
  doc["selectedAxis"] = selectedAxis;
  doc["currentState"] = currentStateStr;
  doc["calibrationStep"] = calibrationStep;
  doc["isCalibrated"] = isCalibrated;
  doc["restPosition"] = restPosition;
  doc["forwardPosition"] = forwardPosition;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleGetConfig() {
  DynamicJsonDocument doc(300);
  doc["selectedAxis"] = selectedAxis;
  doc["magnitudeThreshold"] = magnitudeThreshold;
  doc["maxPatternTime"] = maxPatternTime;
  doc["restPosition"] = restPosition;
  doc["forwardPosition"] = forwardPosition;
  doc["isCalibrated"] = isCalibrated;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSetConfig() {
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(200);
    deserializeJson(doc, server.arg("plain"));
    
    selectedAxis = doc["selectedAxis"].as<String>();
    magnitudeThreshold = doc["magnitudeThreshold"];
    maxPatternTime = doc["maxPatternTime"];
    
    // Validate values
    magnitudeThreshold = max(magnitudeThreshold, 1.0f);
    maxPatternTime = constrain(maxPatternTime, 500, 5000);
    
    if (selectedAxis != "X" && selectedAxis != "Y" && selectedAxis != "Z") {
      selectedAxis = "Y";
    }
    
    saveConfiguration();
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

void handleStartCalibration() {
  calibrationMode = true;
  calibrationStep = "RELAX";
  isCalibrated = false;
  Serial.println("Calibration started - waiting for user to get in rest position");
  server.send(200, "text/plain", "Calibration started");
}

void handleNextCalibration() {
  if (!calibrationMode) {
    server.send(400, "text/plain", "Calibration not active");
    return;
  }
  
  if (calibrationStep == "RELAX") {
    // Start REST calibration
    calibrationStep = "REST_CALIBRATING";
    calibrationStartTime = millis();
    calibrationSum = 0;
    calibrationSamples = 0;
    Serial.println("Starting REST calibration...");
    server.send(200, "text/plain", "Calibrating REST");
    
  } else if (calibrationStep == "FORWARD") {
    // Start FORWARD calibration
    calibrationStep = "FORWARD_CALIBRATING";
    calibrationStartTime = millis();
    calibrationSum = 0;
    calibrationSamples = 0;
    Serial.println("Starting FORWARD calibration...");
    server.send(200, "text/plain", "Calibrating FORWARD");
    
  } else {
    server.send(400, "text/plain", "Not ready for next step");
  }
}

void saveConfiguration() {
  EEPROM.put(0, magnitudeThreshold);
  EEPROM.put(4, maxPatternTime);
  EEPROM.put(8, restPosition);
  EEPROM.put(12, forwardPosition);
  EEPROM.put(16, isCalibrated);
  EEPROM.put(17, selectedAxis.c_str()[0]);
  EEPROM.commit();
}

void loadConfiguration() {
  float tempMagThreshold;
  unsigned long tempMaxPatternTime;
  float tempRestPos, tempForwardPos;
  bool tempIsCalibrated;
  char tempAxis;
  
  EEPROM.get(0, tempMagThreshold);
  EEPROM.get(4, tempMaxPatternTime);
  EEPROM.get(8, tempRestPos);
  EEPROM.get(12, tempForwardPos);
  EEPROM.get(16, tempIsCalibrated);
  EEPROM.get(17, tempAxis);
  
  // Validate and load values
  if (tempMagThreshold >= 1) magnitudeThreshold = tempMagThreshold;
  if (tempMaxPatternTime >= 500 && tempMaxPatternTime <= 5000) maxPatternTime = tempMaxPatternTime;
  if (tempRestPos > -1000 && tempRestPos < 1000) restPosition = tempRestPos;
  if (tempForwardPos > -1000 && tempForwardPos < 1000) forwardPosition = tempForwardPos;
  isCalibrated = tempIsCalibrated;
  if (tempAxis == 'X' || tempAxis == 'Y' || tempAxis == 'Z') selectedAxis = String(tempAxis);
}