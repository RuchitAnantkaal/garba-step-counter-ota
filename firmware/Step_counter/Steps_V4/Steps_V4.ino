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
  STATE_FORWARD,
  STATE_BACKWARD
};

// Configurable variables (you can modify these before uploading)
String selectedAxis = "Y";                   // Which axis to monitor: "X", "Y", or "Z"
float magnitudeThreshold = 50.0;             // Magnitude threshold (raw values)
float axisThreshold = 30.0;                  // Axis movement threshold
unsigned long maxPatternTime = 2000;         // Max time for step pattern (ms)

// Calibrated movement ranges (will be set during calibration)
float restPosition = -264.0;                 // Rest position
float forwardMax = -200.0;                   // Maximum forward movement
float backwardMax = -320.0;                  // Maximum backward movement
bool isCalibrated = false;

// Rest position values (will be calibrated to raw ADXL345 values)
float restX = -32.0;
float restY = -264.0;
float restZ = 0.0;

// Step counting variables
unsigned long stepCount = 0;
bool stepDetected = false;
unsigned long lastStepTime = 0;
const unsigned long stepDelay = 300; // Minimum time between steps (ms)

// Movement pattern detection
MovementState currentState = STATE_REST;
MovementState lastState = STATE_REST;
unsigned long stateStartTime = 0;
bool hasMovedForward = false;
bool hasMovedBackward = false;

// Sensor data
float currentX, currentY, currentZ;
float magnitude;
float deltaSelectedAxis, deltaMagnitude;
float selectedAxisValue;
String currentStateStr = "REST";

// Calibration variables
bool calibrationMode = false;
String calibrationStep = "IDLE"; // IDLE, RELAX, REST_CALIBRATING, REST_DONE, FORWARD, FORWARD_CALIBRATING, FORWARD_DONE, BACKWARD, BACKWARD_CALIBRATING, BACKWARD_DONE, COMPLETE
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
  
  Serial.println("Step Counter with Smart Calibration initialized!");
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
  float deltaX = abs(currentX - restX);
  float deltaY = abs(currentY - restY);
  float deltaZ = abs(currentZ - restZ);
  float restMagnitude = sqrt(restX * restX + restY * restY + restZ * restZ);
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
  if (calibrationStep == "REST_DONE" || calibrationStep == "FORWARD_DONE" || calibrationStep == "BACKWARD_DONE") {
    if (millis() - calibrationDisplayTime >= displayDuration) {
      if (calibrationStep == "REST_DONE") {
        calibrationStep = "FORWARD";
        Serial.println("Ready for FORWARD calibration");
      } else if (calibrationStep == "FORWARD_DONE") {
        calibrationStep = "BACKWARD";
        Serial.println("Ready for BACKWARD calibration");
      } else if (calibrationStep == "BACKWARD_DONE") {
        calibrationStep = "COMPLETE";
        calibrationMode = false;
        isCalibrated = true;
        saveConfiguration();
        Serial.println("Calibration COMPLETE!");
        Serial.print("Final values - Rest: "); Serial.print(restPosition);
        Serial.print(", Forward: "); Serial.print(forwardMax);
        Serial.print(", Backward: "); Serial.println(backwardMax);
      }
    }
    return;
  }
  
  // Handle active calibration readings
  if (calibrationStep == "REST_CALIBRATING" || calibrationStep == "FORWARD_CALIBRATING" || calibrationStep == "BACKWARD_CALIBRATING") {
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
        forwardMax = average;
        calibrationStep = "FORWARD_DONE";
        calibrationDisplayTime = millis();
        Serial.print("FORWARD calibrated to: "); Serial.println(forwardMax);
      } else if (calibrationStep == "BACKWARD_CALIBRATING") {
        backwardMax = average;
        calibrationStep = "BACKWARD_DONE";
        calibrationDisplayTime = millis();
        Serial.print("BACKWARD calibrated to: "); Serial.println(backwardMax);
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
  float minMovementThreshold = 15.0; // Ignore movements smaller than this
  if (abs(selectedAxisValue - restPosition) < minMovementThreshold) {
    return;
  }
  
  // Determine current movement state based on calibrated values with larger thresholds
  MovementState newState = STATE_REST;
  float forwardThreshold = abs(forwardMax - restPosition) * 0.6; // 60% of calibrated range
  float backwardThreshold = abs(backwardMax - restPosition) * 0.6; // 60% of calibrated range
  
  // Ensure minimum thresholds
  forwardThreshold = max(forwardThreshold, 20.0f);
  backwardThreshold = max(backwardThreshold, 20.0f);
  
  // Check if we're in forward or backward position
  if (selectedAxisValue > (restPosition + forwardThreshold)) {
    newState = STATE_FORWARD;
  } else if (selectedAxisValue < (restPosition - backwardThreshold)) {
    newState = STATE_BACKWARD;
  } else {
    newState = STATE_REST;
  }
  
  // Update state string for display
  switch (newState) {
    case STATE_REST: currentStateStr = "REST"; break;
    case STATE_FORWARD: currentStateStr = "FORWARD"; break;
    case STATE_BACKWARD: currentStateStr = "BACKWARD"; break;
  }
  
  // Detect state transitions for step counting
  if (newState != currentState) {
    Serial.print("State change: "); 
    Serial.print(currentState); 
    Serial.print(" -> "); 
    Serial.print(newState);
    Serial.print(" | Value: "); Serial.print(selectedAxisValue);
    Serial.print(" | Rest: "); Serial.print(restPosition);
    Serial.print(" | F_thresh: "); Serial.print(forwardThreshold);
    Serial.print(" | B_thresh: "); Serial.println(backwardThreshold);
    
    // Track movement patterns
    if (newState == STATE_FORWARD) hasMovedForward = true;
    if (newState == STATE_BACKWARD) hasMovedBackward = true;
    
    // Check for step completion patterns
    if ((currentState == STATE_FORWARD && newState == STATE_BACKWARD) ||
        (currentState == STATE_BACKWARD && newState == STATE_FORWARD) ||
        (newState == STATE_REST && (hasMovedForward || hasMovedBackward))) {
      
      // Complete step pattern detected
      if ((hasMovedForward && hasMovedBackward) && 
          (millis() - lastStepTime >= stepDelay)) {
        
        stepCount++;
        stepDetected = true;
        lastStepTime = millis();
        
        Serial.print("STEP DETECTED! Count: "); Serial.print(stepCount);
        Serial.print(" | " + selectedAxis + ": "); Serial.print(selectedAxisValue);
        Serial.print(" | Pattern: Forward+Backward completed");
        Serial.println();
        
        // Reset pattern flags
        hasMovedForward = false;
        hasMovedBackward = false;
      }
    }
    
    currentState = newState;
    stateStartTime = millis();
  }
  
  // Reset pattern if it takes too long
  if (millis() - stateStartTime > maxPatternTime) {
    hasMovedForward = false;
    hasMovedBackward = false;
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
  html += ".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
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
  html += ".graph-container { background: white; padding: 20px; border-radius: 8px; margin: 20px 0; border: 1px solid #ddd; text-align: center; }";
  html += ".arc-graph { width: 300px; height: 150px; margin: 0 auto; position: relative; }";
  html += ".calibration-section { background: #e8f5e8; padding: 15px; border-radius: 8px; margin: 15px 0; border: 2px solid #4CAF50; }";
  html += ".calibration-step { font-size: 18px; font-weight: bold; color: #2E7D32; text-align: center; margin: 10px 0; }";
  html += ".calibration-values { background: #f1f8e9; padding: 10px; border-radius: 5px; font-family: monospace; }";
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='container'>";
  html += "<h1>Smart Step Counter</h1>";
  
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
  
  html += "<div class='graph-container'>";
  html += "<h3>Hand Movement Arc</h3>";
  html += "<div class='arc-graph'>";
  html += "<canvas id='arcCanvas' width='300' height='150'></canvas>";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='config-section'>";
  html += "<h3>Movement Calibration</h3>";
  
  html += "<div class='calibration-section' id='calibrationSection'>";
  html += "<div class='calibration-step' id='calibrationStatus'>Ready for Movement Calibration</div>";
  html += "<div class='calibration-values' id='calibrationValues'>";
  html += "Rest: <span id='restPos'>Not calibrated</span><br>";
  html += "Forward: <span id='forwardPos'>Not calibrated</span><br>";
  html += "Backward: <span id='backwardPos'>Not calibrated</span>";
  html += "</div>";
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
  html += "<label for='axisThreshold'>Axis Threshold:</label>";
  html += "<input type='number' id='axisThreshold' min='1' step='0.1' value='30'>";
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
  html += "let calibrationActive = false;";
  html += "let calibrationStep = 'IDLE';";
  html += "let restPos = 0, forwardPos = 0, backwardPos = 0;";
  
  html += "function drawArc() {";
  html += "const canvas = document.getElementById('arcCanvas');";
  html += "const ctx = canvas.getContext('2d');";
  html += "const centerX = 150, centerY = 30, radius = 100;";
  
  html += "ctx.clearRect(0, 0, 300, 150);";
  
  html += "ctx.strokeStyle = '#ddd';";
  html += "ctx.lineWidth = 3;";
  html += "ctx.beginPath();";
  html += "ctx.arc(centerX, centerY, radius, 0, Math.PI);";
  html += "ctx.stroke();";
  
  html += "if (restPos !== 0) {";
  
  html += "let currentValue = parseFloat(document.getElementById('selectedAxisValue').textContent) || restPos;";
  
  html += "let maxRange = Math.max(300, Math.abs(forwardPos - restPos), Math.abs(backwardPos - restPos));";
  html += "let linearMin = restPos - maxRange;";
  html += "let linearMax = restPos + maxRange;";
  html += "let extremeMin = restPos - 1000;";
  html += "let extremeMax = restPos + 1000;";
  
  html += "function valueToAngle(value) {";
  html += "let normalized;";
  html += "if (value >= linearMin && value <= linearMax) {";
  html += "normalized = 0.05 + ((value - linearMin) / (linearMax - linearMin)) * 0.9;";
  html += "} else if (value < linearMin) {";
  html += "let extremeRange = linearMin - extremeMin;";
  html += "let extremePos = Math.max(0, (value - extremeMin) / extremeRange);";
  html += "normalized = extremePos * 0.05;";
  html += "} else {";
  html += "let extremeRange = extremeMax - linearMax;";
  html += "let extremePos = Math.min(1, (value - linearMax) / extremeRange);";
  html += "normalized = 0.95 + extremePos * 0.05;";
  html += "}";
  html += "return Math.PI * (1 - normalized);";
  html += "}";
  
  html += "function drawCalibrationMarker(value, color, label) {";
  html += "let angle = valueToAngle(value);";
  html += "let x = centerX + radius * Math.cos(angle);";
  html += "let y = centerY + radius * Math.sin(angle);";
  html += "ctx.fillStyle = color;";
  html += "ctx.beginPath();";
  html += "ctx.arc(x, y, 6, 0, 2 * Math.PI);";
  html += "ctx.fill();";
  html += "ctx.strokeStyle = 'white';";
  html += "ctx.lineWidth = 1;";
  html += "ctx.stroke();";
  html += "ctx.fillStyle = 'black';";
  html += "ctx.font = 'bold 10px Arial';";
  html += "ctx.textAlign = 'center';";
  html += "ctx.fillText(label, x, y + 15);";
  html += "}";
  
  html += "if (backwardPos !== 0) drawCalibrationMarker(backwardPos, '#f44336', 'B');";
  html += "drawCalibrationMarker(restPos, '#FFC107', 'R');";
  html += "if (forwardPos !== 0) drawCalibrationMarker(forwardPos, '#2196F3', 'F');";
  
  html += "let currentAngle = valueToAngle(currentValue);";
  html += "let restAngle = valueToAngle(restPos);";
  
  html += "let lineColor = '#FFC107';";
  html += "if (currentValue > restPos + 15) lineColor = '#2196F3';";
  html += "else if (currentValue < restPos - 15) lineColor = '#f44336';";
  
  html += "ctx.strokeStyle = lineColor;";
  html += "ctx.lineWidth = 6;";
  html += "ctx.beginPath();";
  html += "ctx.arc(centerX, centerY, radius, Math.min(restAngle, currentAngle), Math.max(restAngle, currentAngle));";
  html += "ctx.stroke();";
  
  html += "let currentX = centerX + radius * Math.cos(currentAngle);";
  html += "let currentY = centerY + radius * Math.sin(currentAngle);";
  html += "ctx.fillStyle = '#FF5722';";
  html += "ctx.beginPath();";
  html += "ctx.arc(currentX, currentY, 5, 0, 2 * Math.PI);";
  html += "ctx.fill();";
  html += "ctx.strokeStyle = 'white';";
  html += "ctx.lineWidth = 1;";
  html += "ctx.stroke();";
  html += "}";
  html += "}";

  html += "function updateData() {";
  html += "fetch('/api/data')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('stepCount').textContent = data.steps;";
  html += "document.getElementById('selectedAxisValue').textContent = data.selectedAxisValue.toFixed(0);";
  html += "document.getElementById('magnitude').textContent = data.magnitude.toFixed(0);";
  html += "document.getElementById('deltaAxis').textContent = data.deltaAxis.toFixed(1);";
  html += "document.getElementById('currentState').textContent = data.currentState;";
  html += "document.getElementById('axisLabel').textContent = data.selectedAxis;";
  html += "document.getElementById('axisLabel2').textContent = data.selectedAxis;";
  
  html += "if (data.calibrationStep && data.calibrationStep !== 'IDLE') {";
  html += "updateCalibrationStatus(data.calibrationStep);";
  html += "}";
  
  html += "if (data.restPosition) restPos = data.restPosition;";
  html += "if (data.forwardMax) forwardPos = data.forwardMax;";
  html += "if (data.backwardMax) backwardPos = data.backwardMax;";
  html += "updateCalibrationDisplay();";
  
  html += "drawArc();";
  html += "})";
  html += ".catch(error => console.error('Error:', error));";
  html += "}";

  html += "function loadConfig() {";
  html += "fetch('/api/config')";
  html += ".then(response => response.json())";
  html += ".then(data => {";
  html += "document.getElementById('selectedAxis').value = data.selectedAxis;";
  html += "document.getElementById('magThreshold').value = data.magnitudeThreshold;";
  html += "document.getElementById('axisThreshold').value = data.axisThreshold;";
  html += "document.getElementById('maxPatternTime').value = data.maxPatternTime;";
  html += "restPos = data.restPosition || 0;";
  html += "forwardPos = data.forwardMax || 0;";
  html += "backwardPos = data.backwardMax || 0;";
  html += "updateCalibrationDisplay();";
  html += "});";
  html += "}";

  html += "function updateCalibrationDisplay() {";
  html += "document.getElementById('restPos').textContent = restPos ? restPos.toFixed(1) : 'Not calibrated';";
  html += "document.getElementById('forwardPos').textContent = forwardPos ? forwardPos.toFixed(1) : 'Not calibrated';";
  html += "document.getElementById('backwardPos').textContent = backwardPos ? backwardPos.toFixed(1) : 'Not calibrated';";
  html += "console.log('Updated calibration display - Rest:', restPos, 'Forward:', forwardPos, 'Backward:', backwardPos);";
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
  html += "case 'FORWARD_DONE': statusEl.textContent = 'Forward calibration done!'; btnEl.style.display = 'none'; break;";
  html += "case 'BACKWARD': statusEl.textContent = 'Move hand back and hold'; btnEl.textContent = 'Calibrate Backward'; btnEl.style.display = 'inline-block'; break;";
  html += "case 'BACKWARD_CALIBRATING': statusEl.textContent = 'Calibrating backward position... (1 sec)'; btnEl.style.display = 'none'; break;";
  html += "case 'BACKWARD_DONE': statusEl.textContent = 'Backward calibration done!'; btnEl.style.display = 'none'; break;";
  html += "case 'COMPLETE': statusEl.textContent = 'Calibration COMPLETE! Ready to count steps.'; btnEl.textContent = 'Start Calibration'; btnEl.style.display = 'inline-block'; break;";
  html += "default: statusEl.textContent = 'Ready for Movement Calibration'; btnEl.textContent = 'Start Calibration'; btnEl.style.display = 'inline-block'; break;";
  html += "}";
  html += "}";

  html += "function handleCalibrationButton() {";
  html += "const statusEl = document.getElementById('calibrationStatus');";
  html += "const currentText = statusEl.textContent;";
  
  html += "if (currentText.includes('Ready for Movement')) {";
  html += "fetch('/api/start-calibration', { method: 'POST' });";
  html += "} else if (currentText.includes('Relax')) {";
  html += "fetch('/api/next-calibration', { method: 'POST' });";
  html += "} else if (currentText.includes('Move hand forward')) {";
  html += "fetch('/api/next-calibration', { method: 'POST' });";
  html += "} else if (currentText.includes('Move hand back')) {";
  html += "fetch('/api/next-calibration', { method: 'POST' });";
  html += "} else if (currentText.includes('COMPLETE')) {";
  html += "fetch('/api/start-calibration', { method: 'POST' });";
  html += "}";
  html += "}";

  html += "function updateConfig() {";
  html += "const config = {";
  html += "selectedAxis: document.getElementById('selectedAxis').value,";
  html += "magnitudeThreshold: parseFloat(document.getElementById('magThreshold').value),";
  html += "axisThreshold: parseFloat(document.getElementById('axisThreshold').value),";
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

  html += "setInterval(updateData, 300);";
  html += "loadConfig();";
  html += "updateData();";
  html += "</script>";
  html += "</body>";
  html += "</html>";

  server.send(200, "text/html", html);
}

void handleGetData() {
  DynamicJsonDocument doc(800);
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
  doc["forwardMax"] = forwardMax;
  doc["backwardMax"] = backwardMax;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleGetConfig() {
  DynamicJsonDocument doc(400);
  doc["selectedAxis"] = selectedAxis;
  doc["magnitudeThreshold"] = magnitudeThreshold;
  doc["axisThreshold"] = axisThreshold;
  doc["maxPatternTime"] = maxPatternTime;
  doc["restPosition"] = restPosition;
  doc["forwardMax"] = forwardMax;
  doc["backwardMax"] = backwardMax;
  doc["isCalibrated"] = isCalibrated;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSetConfig() {
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(300);
    deserializeJson(doc, server.arg("plain"));
    
    selectedAxis = doc["selectedAxis"].as<String>();
    magnitudeThreshold = doc["magnitudeThreshold"];
    axisThreshold = doc["axisThreshold"];
    maxPatternTime = doc["maxPatternTime"];
    
    // Validate values
    magnitudeThreshold = max(magnitudeThreshold, 1.0f);
    axisThreshold = max(axisThreshold, 1.0f);
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
    
  } else if (calibrationStep == "BACKWARD") {
    // Start BACKWARD calibration
    calibrationStep = "BACKWARD_CALIBRATING";
    calibrationStartTime = millis();
    calibrationSum = 0;
    calibrationSamples = 0;
    Serial.println("Starting BACKWARD calibration...");
    server.send(200, "text/plain", "Calibrating BACKWARD");
    
  } else {
    server.send(400, "text/plain", "Not ready for next step");
  }
}

void saveConfiguration() {
  EEPROM.put(0, magnitudeThreshold);
  EEPROM.put(4, axisThreshold);
  EEPROM.put(8, maxPatternTime);
  EEPROM.put(12, restPosition);
  EEPROM.put(16, forwardMax);
  EEPROM.put(20, backwardMax);
  EEPROM.put(24, isCalibrated);
  EEPROM.put(25, selectedAxis.c_str()[0]);
  EEPROM.commit();
}

void loadConfiguration() {
  float tempMagThreshold, tempAxisThreshold;
  unsigned long tempMaxPatternTime;
  float tempRestPos, tempForwardMax, tempBackwardMax;
  bool tempIsCalibrated;
  char tempAxis;
  
  EEPROM.get(0, tempMagThreshold);
  EEPROM.get(4, tempAxisThreshold);
  EEPROM.get(8, tempMaxPatternTime);
  EEPROM.get(12, tempRestPos);
  EEPROM.get(16, tempForwardMax);
  EEPROM.get(20, tempBackwardMax);
  EEPROM.get(24, tempIsCalibrated);
  EEPROM.get(25, tempAxis);
  
  // Validate and load values
  if (tempMagThreshold >= 1) magnitudeThreshold = tempMagThreshold;
  if (tempAxisThreshold >= 1) axisThreshold = tempAxisThreshold;
  if (tempMaxPatternTime >= 500 && tempMaxPatternTime <= 5000) maxPatternTime = tempMaxPatternTime;
  if (tempRestPos > -1000 && tempRestPos < 1000) restPosition = tempRestPos;
  if (tempForwardMax > -1000 && tempForwardMax < 1000) forwardMax = tempForwardMax;
  if (tempBackwardMax > -1000 && tempBackwardMax < 1000) backwardMax = tempBackwardMax;
  isCalibrated = tempIsCalibrated;
  if (tempAxis == 'X' || tempAxis == 'Y' || tempAxis == 'Z') selectedAxis = String(tempAxis);
}