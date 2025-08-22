const express = require('express');
const cors = require('cors');
const os = require('os');

const app = express();
const PORT = 3000;

// ============================================================================
// DATA STORAGE
// ============================================================================

// Core device data
const deviceData = new Map();           // deviceId -> device info
const receiverData = new Map();         // receiverId -> receiver info
const deviceToReceiver = new Map();     // deviceId -> best receiverId

// Enhanced tracking
const connectionLogs = [];              // Connection history
const deviceHistory = new Map();       // deviceId -> historical data
const serverStats = {
  startTime: Date.now(),
  totalRequests: 0,
  totalSteps: 0,
  activeDevices: 0,
  activeReceivers: 0
};

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

function getLocalIP() {
  const interfaces = os.networkInterfaces();
  for (const name of Object.keys(interfaces)) {
    for (const interface of interfaces[name]) {
      if (interface.family === 'IPv4' && !interface.internal) {
        return interface.address;
      }
    }
  }
  return '127.0.0.1';
}

function getPCName() {
  return os.hostname();
}

function addLog(type, message, extra = {}) {
  const log = {
    id: Date.now() + Math.random(),
    timestamp: new Date().toLocaleString(),
    time: Date.now(),
    type: type, // 'device-connect', 'device-update', 'receiver-connect', 'system'
    message: message,
    ...extra
  };
  
  connectionLogs.unshift(log);
  
  // Keep only last 200 logs
  if (connectionLogs.length > 200) {
    connectionLogs.length = 200;
  }
  
  console.log(`📋 LOG [${type}]: ${message}`);
}

function updateDeviceHistory(deviceId, stepCount, batteryLevel) {
  if (!deviceHistory.has(deviceId)) {
    deviceHistory.set(deviceId, {
      firstSeen: Date.now(),
      totalUpdates: 0,
      peakSteps: 0,
      minBattery: 100,
      maxBattery: 0
    });
    addLog('device-connect', `New device ${deviceId.substring(9)} connected`, { deviceId });
  }
  
  const history = deviceHistory.get(deviceId);
  history.totalUpdates++;
  history.lastUpdate = Date.now();
  
  if (stepCount > history.peakSteps) {
    history.peakSteps = stepCount;
    addLog('device-update', `Device ${deviceId.substring(9)} reached ${stepCount} steps (new peak)`, { deviceId, stepCount });
  }
  
  if (batteryLevel < history.minBattery) history.minBattery = batteryLevel;
  if (batteryLevel > history.maxBattery) history.maxBattery = batteryLevel;
}

function calculateBestReceiver(deviceId) {
  let bestReceiver = null;
  let bestScore = -Infinity;
  let bestTime = 0;
  
  // Check all receivers that have data for this device
  for (const [receiverId, receiver] of receiverData.entries()) {
    if (receiver.devices && receiver.devices.has(deviceId)) {
      const deviceInfo = receiver.devices.get(deviceId);
      const timeSinceUpdate = Date.now() - deviceInfo.lastUpdate;
      
      // Score based on signal strength and recency
      const score = deviceInfo.signalStrength - (timeSinceUpdate / 1000);
      
      if (score > bestScore) {
        bestScore = score;
        bestReceiver = receiverId;
        bestTime = deviceInfo.lastUpdate;
      }
    }
  }
  
  return bestReceiver;
}

function updateStatistics() {
  const now = Date.now();
  const fiveMinutesAgo = now - 300000;  // 5 minutes for devices
  const oneMinuteAgo = now - 60000;     // 1 minute for receivers
  
  serverStats.activeDevices = 0;
  serverStats.totalSteps = 0;
  
  // Count ONLY devices that are actively reporting (updated in last 5 minutes)
  const activeDevices = new Set();
  
  for (const [deviceId, device] of deviceData.entries()) {
    // Only count devices that have been seen in the last 5 minutes
    if (device.lastSeen > fiveMinutesAgo) {
      activeDevices.add(deviceId);
      serverStats.totalSteps += device.stepCount;
    }
  }
  
  serverStats.activeDevices = activeDevices.size;
  
  // Count active receivers (updated in last 1 minute)
  serverStats.activeReceivers = 0;
  
  for (const [receiverId, receiver] of receiverData.entries()) {
    if (receiver.lastSeen > oneMinuteAgo) {
      serverStats.activeReceivers++;
    }
  }
  
  // Debug logging
  console.log(`📊 STATS: ${serverStats.totalSteps} steps from ${serverStats.activeDevices} active devices (${activeDevices.size} unique), ${serverStats.activeReceivers} receivers online`);
}

function cleanupOldData() {
  const now = Date.now();
  const fiveMinutesAgo = now - 300000;  // 5 minutes for devices
  const oneMinuteAgo = now - 60000;     // 1 minute for receivers
  
  let cleanedDevices = 0;
  let cleanedReceivers = 0;
  
  // Clean old devices (haven't been seen for 5+ minutes)
  for (const [deviceId, device] of deviceData.entries()) {
    if (device.lastSeen < fiveMinutesAgo) {
      deviceData.delete(deviceId);
      deviceToReceiver.delete(deviceId);
      cleanedDevices++;
      addLog('device-disconnect', `Device ${deviceId.substring(9)} timed out (inactive for 5+ minutes)`, { deviceId });
    }
  }
  
  // Clean old device data from receivers
  for (const [receiverId, receiver] of receiverData.entries()) {
    for (const [deviceId, deviceInfo] of receiver.devices.entries()) {
      if (deviceInfo.lastUpdate < fiveMinutesAgo) {
        receiver.devices.delete(deviceId);
      }
    }
  }
  
  // Clean old receivers (haven't been seen for 1+ minute)
  for (const [receiverId, receiver] of receiverData.entries()) {
    if (receiver.lastSeen < oneMinuteAgo) {
      receiverData.delete(receiverId);
      cleanedReceivers++;
      addLog('receiver-disconnect', `Receiver ${receiverId.substring(12)} disconnected (inactive for 1+ minute)`, { receiverId });
    }
  }
  
  if (cleanedDevices > 0 || cleanedReceivers > 0) {
    addLog('system', `Cleanup: removed ${cleanedDevices} inactive devices, ${cleanedReceivers} disconnected receivers`);
    console.log(`🧹 CLEANUP: Removed ${cleanedDevices} old devices, ${cleanedReceivers} old receivers`);
  }
}

// ============================================================================
// EXPRESS MIDDLEWARE
// ============================================================================

app.use(cors());
app.use(express.json({ limit: '10mb' }));
app.use(express.urlencoded({ extended: true }));

// Request logging
app.use((req, res, next) => {
  serverStats.totalRequests++;
  
  const start = Date.now();
  res.on('finish', () => {
    const duration = Date.now() - start;
    console.log(`📡 ${req.method} ${req.url} - ${res.statusCode} (${duration}ms)`);
  });
  
  next();
});

// ============================================================================
// API ENDPOINTS
// ============================================================================

// Main receiver data endpoint
app.post('/api/receiver-data', (req, res) => {
  try {
    const { receiverId, timestamp, deviceCount, devices: deviceList, receiverPriority } = req.body;
    
    if (!receiverId || !Array.isArray(deviceList)) {
      return res.status(400).send('Invalid request format');
    }
    
    console.log(`📡 Receiver ${receiverId.substring(12)} sent ${deviceList.length} devices`);
    
    // Update receiver info
    if (!receiverData.has(receiverId)) {
      receiverData.set(receiverId, {
        id: receiverId,
        firstSeen: Date.now(),
        devices: new Map(),
        totalDevicesSeen: 0
      });
      addLog('receiver-connect', `Receiver ${receiverId.substring(12)} connected`, { receiverId });
    }
    
    const receiver = receiverData.get(receiverId);
    receiver.lastSeen = Date.now();
    receiver.deviceCount = deviceCount;
    receiver.priority = receiverPriority || 0;
    
    // Process each device
    for (const device of deviceList) {
      const { deviceId, stepCount, batteryLevel, lastSeen, signalStrength } = device;
      
      if (!deviceId || typeof stepCount !== 'number') continue;
      
      console.log(`  📱 Device ${deviceId.substring(9)}: ${stepCount} steps, ${batteryLevel}% battery`);
      
      // Update device history
      updateDeviceHistory(deviceId, stepCount, batteryLevel);
      
      // Store device data for this receiver
      receiver.devices.set(deviceId, {
        stepCount: stepCount,
        batteryLevel: batteryLevel || 0,
        lastUpdate: Date.now() - (lastSeen * 1000),
        signalStrength: signalStrength || -100,
        lastSeen: lastSeen || 0
      });
      
      // Determine best receiver for this device
      const bestReceiverId = calculateBestReceiver(deviceId);
      if (bestReceiverId) {
        deviceToReceiver.set(deviceId, bestReceiverId);
        
        const bestReceiverData = receiverData.get(bestReceiverId);
        const bestDeviceData = bestReceiverData.devices.get(deviceId);
        
        // Update main device record
        deviceData.set(deviceId, {
          id: deviceId,
          stepCount: bestDeviceData.stepCount,
          batteryLevel: bestDeviceData.batteryLevel,
          bestReceiverId: bestReceiverId,
          lastSeen: bestDeviceData.lastUpdate,
          signalStrength: bestDeviceData.signalStrength
        });
        
        console.log(`  ✅ Updated device ${deviceId.substring(9)} with ${bestDeviceData.stepCount} steps via receiver ${bestReceiverId.substring(12)}`);
      }
    }
    
    updateStatistics();
    res.send('Data processed successfully');
    
  } catch (error) {
    console.error('❌ Error processing receiver data:', error);
    res.status(500).send('Internal server error');
  }
});

// Dashboard data
app.get('/api/dashboard-data', (req, res) => {
  updateStatistics();
  res.json({
    totalSteps: serverStats.totalSteps,
    totalDevices: serverStats.activeDevices,
    activeReceivers: serverStats.activeReceivers,
    systemStatus: 'Online',
    uptime: Math.floor((Date.now() - serverStats.startTime) / 1000),
    totalRequests: serverStats.totalRequests,
    lastUpdate: new Date().toISOString()
  });
});

// All devices with history
app.get('/api/devices', (req, res) => {
  const devices = [];
  const now = Date.now();
  const fiveMinutesAgo = now - 300000;
  
  for (const [deviceId, device] of deviceData.entries()) {
    const history = deviceHistory.get(deviceId);
    const receiver = receiverData.get(device.bestReceiverId);
    
    // Determine if device is truly active (updated in last 5 minutes)
    const isActive = device.lastSeen > fiveMinutesAgo;
    
    devices.push({
      id: deviceId,
      name: deviceId.substring(9),
      stepCount: device.stepCount,
      batteryLevel: device.batteryLevel,
      signalStrength: device.signalStrength,
      receiverId: device.bestReceiverId,
      receiverName: device.bestReceiverId ? device.bestReceiverId.substring(12) : 'Unknown',
      lastSeen: Math.floor((now - device.lastSeen) / 1000),
      status: isActive ? 'Online' : 'Offline',
      firstSeen: history ? new Date(history.firstSeen).toLocaleString() : 'Unknown',
      totalUpdates: history ? history.totalUpdates : 0,
      peakSteps: history ? history.peakSteps : device.stepCount,
      minBattery: history ? history.minBattery : device.batteryLevel,
      maxBattery: history ? history.maxBattery : device.batteryLevel
    });
  }
  
  // Sort by step count descending, but show active devices first
  devices.sort((a, b) => {
    if (a.status === 'Online' && b.status === 'Offline') return -1;
    if (a.status === 'Offline' && b.status === 'Online') return 1;
    return b.stepCount - a.stepCount;
  });
  
  res.json({ devices });
});

// All receivers
app.get('/api/receivers', (req, res) => {
  const receivers = [];
  const now = Date.now();
  
  for (const [receiverId, receiver] of receiverData.entries()) {
    if (now - receiver.lastSeen > 120000) continue; // Skip very old receivers
    
    let totalSteps = 0;
    const devices = [];
    
    for (const [deviceId, deviceInfo] of receiver.devices.entries()) {
      totalSteps += deviceInfo.stepCount;
      devices.push({
        id: deviceId,
        name: deviceId.substring(9),
        stepCount: deviceInfo.stepCount,
        batteryLevel: deviceInfo.batteryLevel,
        signalStrength: deviceInfo.signalStrength,
        lastSeen: Math.floor((now - deviceInfo.lastUpdate) / 1000)
      });
    }
    
    receivers.push({
      id: receiverId,
      name: receiverId.substring(12),
      deviceCount: receiver.deviceCount || devices.length,
      totalSteps: totalSteps,
      lastSeen: Math.floor((now - receiver.lastSeen) / 1000),
      status: (now - receiver.lastSeen) < 60000 ? 'Online' : 'Offline',
      devices: devices,
      firstSeen: new Date(receiver.firstSeen).toLocaleString()
    });
  }
  
  receivers.sort((a, b) => b.totalSteps - a.totalSteps);
  res.json({ receivers });
});

// Connection logs
app.get('/api/logs', (req, res) => {
  res.json({ logs: connectionLogs });
});

// Receiver details
app.get('/api/receiver/:receiverId', (req, res) => {
  const shortId = req.params.receiverId;
  
  // Find receiver by short ID
  let foundReceiver = null;
  let foundId = null;
  
  for (const [receiverId, receiver] of receiverData.entries()) {
    if (receiverId.includes(shortId)) {
      foundReceiver = receiver;
      foundId = receiverId;
      break;
    }
  }
  
  if (!foundReceiver) {
    return res.status(404).json({ error: 'Receiver not found' });
  }
  
  const now = Date.now();
  const devices = [];
  let totalSteps = 0;
  
  for (const [deviceId, deviceInfo] of foundReceiver.devices.entries()) {
    const history = deviceHistory.get(deviceId);
    totalSteps += deviceInfo.stepCount;
    
    devices.push({
      id: deviceId,
      name: deviceId.substring(9),
      stepCount: deviceInfo.stepCount,
      batteryLevel: deviceInfo.batteryLevel,
      signalStrength: deviceInfo.signalStrength,
      lastSeen: Math.floor((now - deviceInfo.lastUpdate) / 1000),
      status: (now - deviceInfo.lastUpdate) < 60000 ? 'Online' : 'Offline',
      peakSteps: history ? history.peakSteps : deviceInfo.stepCount,
      totalUpdates: history ? history.totalUpdates : 0,
      firstSeen: history ? new Date(history.firstSeen).toLocaleString() : 'Unknown'
    });
  }
  
  res.json({
    id: foundId,
    name: shortId,
    deviceCount: foundReceiver.deviceCount,
    totalSteps: totalSteps,
    lastSeen: Math.floor((now - foundReceiver.lastSeen) / 1000),
    firstSeen: new Date(foundReceiver.firstSeen).toLocaleString(),
    devices: devices
  });
});

// System reset
app.post('/api/reset', (req, res) => {
  // Clear all data structures completely
  deviceData.clear();
  receiverData.clear();
  deviceToReceiver.clear();
  deviceHistory.clear();
  connectionLogs.length = 0;
  
  // Reset all statistics to zero
  serverStats.totalSteps = 0;
  serverStats.activeDevices = 0;
  serverStats.activeReceivers = 0;
  serverStats.totalRequests = 0;
  
  console.log('🔄 COMPLETE SYSTEM RESET - All data cleared');
  addLog('system', 'Complete system reset - all data and history cleared');
  res.send('System reset successfully');
});

// Debug endpoint to see raw data
app.get('/api/debug-raw', (req, res) => {
  const now = Date.now();
  
  const rawData = {
    receiverData: Array.from(receiverData.entries()).map(([id, data]) => ({
      id: id,
      shortId: id.substring(12),
      lastSeen: Math.floor((now - data.lastSeen) / 1000),
      deviceCount: data.deviceCount,
      devicesInReceiver: Array.from(data.devices.entries()).map(([deviceId, deviceData]) => ({
        deviceId: deviceId,
        shortId: deviceId.substring(9),
        stepCount: deviceData.stepCount,
        batteryLevel: deviceData.batteryLevel,
        lastUpdate: Math.floor((now - deviceData.lastUpdate) / 1000)
      }))
    })),
    
    deviceData: Array.from(deviceData.entries()).map(([id, data]) => ({
      id: id,
      shortId: id.substring(9),
      stepCount: data.stepCount,
      batteryLevel: data.batteryLevel,
      bestReceiverId: data.bestReceiverId,
      bestReceiverShort: data.bestReceiverId ? data.bestReceiverId.substring(12) : 'None',
      lastSeen: Math.floor((now - data.lastSeen) / 1000)
    })),
    
    statistics: serverStats
  };
  
  res.json(rawData);
});

// ============================================================================
// WEB INTERFACE
// ============================================================================

// Main dashboard
app.get('/', (req, res) => {
  res.send(`
<!DOCTYPE html>
<html><head>
<title>Garba Step Counter</title>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { 
  font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  min-height: 100vh; 
  display: flex; 
  align-items: center; 
  justify-content: center; 
}
.container { 
  text-align: center; 
  padding: 3rem; 
  background: rgba(255, 255, 255, 0.95); 
  border-radius: 20px; 
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3); 
  max-width: 800px; 
  width: 90%; 
  backdrop-filter: blur(10px);
}
.title { 
  font-size: 3.5rem; 
  font-weight: 800; 
  background: linear-gradient(45deg, #667eea, #764ba2);
  -webkit-background-clip: text; 
  -webkit-text-fill-color: transparent; 
  margin-bottom: 2rem; 
}
.step-display { 
  background: linear-gradient(135deg, #667eea, #764ba2); 
  padding: 4rem 2rem; 
  border-radius: 15px; 
  margin: 2rem 0; 
  position: relative;
  overflow: hidden;
}
.step-display::before {
  content: '';
  position: absolute;
  top: -50%;
  left: -50%;
  width: 200%;
  height: 200%;
  background: linear-gradient(45deg, transparent, rgba(255,255,255,0.1), transparent);
  animation: shine 3s infinite;
}
@keyframes shine {
  0% { transform: translateX(-100%) translateY(-100%) rotate(45deg); }
  100% { transform: translateX(100%) translateY(100%) rotate(45deg); }
}
.step-count { 
  font-size: 5rem; 
  font-weight: 900; 
  color: white; 
  text-shadow: 2px 2px 4px rgba(0,0,0,0.3); 
  margin-bottom: 1rem; 
  position: relative;
  z-index: 1;
  transition: all 0.3s ease;
}
.step-count.glow {
  text-shadow: 2px 2px 4px rgba(0,0,0,0.5), 0 0 20px rgba(255,255,255,0.8), 0 0 40px rgba(255,255,255,0.6);
  transform: scale(1.02);
}
.step-label { 
  font-size: 1.5rem; 
  color: white; 
  font-weight: 600; 
  text-transform: uppercase; 
  letter-spacing: 2px; 
  position: relative;
  z-index: 1;
}
.stats-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 1.5rem;
  margin-top: 2rem;
}
.stat-card {
  background: rgba(102, 126, 234, 0.1);
  padding: 1.5rem;
  border-radius: 10px;
  border: 1px solid rgba(102, 126, 234, 0.2);
  position: relative;
  overflow: hidden;
}
.stat-value {
  font-size: 1.8rem;
  font-weight: bold;
  color: #667eea;
  margin-bottom: 0.5rem;
  transition: all 0.3s ease;
  position: relative;
}
.stat-value.animate {
  transform: scale(1.05);
  color: #4f46e5;
}
.stat-label {
  color: #666;
  font-size: 0.9rem;
  font-weight: 500;
}

/* Bigger popup with splash effect for step increments */
.step-increment-popup {
  position: absolute;
  font-size: 1.4rem;
  font-weight: bold;
  text-shadow: 2px 2px 4px rgba(0,0,0,0.6), 0 0 8px currentColor;
  pointer-events: none;
  z-index: 10;
  animation: splashPopup 2.5s ease-out forwards;
  white-space: nowrap;
  max-width: 120px;
  overflow: hidden;
  text-overflow: ellipsis;
}

.step-increment-popup::before {
  content: '';
  position: absolute;
  top: 50%;
  left: 50%;
  width: 0;
  height: 0;
  background: radial-gradient(circle, currentColor 0%, transparent 70%);
  border-radius: 50%;
  transform: translate(-50%, -50%);
  animation: splashEffect 2.5s ease-out forwards;
  z-index: -1;
  opacity: 0.3;
}

@keyframes splashPopup {
  0% {
    opacity: 0;
    transform: scale(0.3) translateY(0) rotate(-5deg);
  }
  10% {
    opacity: 1;
    transform: scale(1.3) translateY(-8px) rotate(2deg);
  }
  20% {
    transform: scale(1) translateY(-15px) rotate(0deg);
  }
  80% {
    opacity: 1;
    transform: scale(0.9) translateY(-35px) rotate(0deg);
  }
  100% {
    opacity: 0;
    transform: scale(0.7) translateY(-50px) rotate(0deg);
  }
}

@keyframes splashEffect {
  0% {
    width: 0;
    height: 0;
    opacity: 0;
  }
  15% {
    width: 60px;
    height: 60px;
    opacity: 0.4;
  }
  30% {
    width: 90px;
    height: 90px;
    opacity: 0.3;
  }
  100% {
    width: 120px;
    height: 120px;
    opacity: 0;
  }
}

/* Number counting animation - faster and snappier */
@keyframes countUp {
  0% { 
    transform: translateY(8px) scale(0.98); 
    opacity: 0.7; 
  }
  100% { 
    transform: translateY(0) scale(1); 
    opacity: 1; 
  }
}

.counting {
  animation: countUp 0.4s ease-out;
}

/* Pulse effect for stats - gentler */
@keyframes gentlePulse {
  0% { 
    transform: scale(1); 
    box-shadow: 0 0 0 rgba(102, 126, 234, 0.3);
  }
  50% { 
    transform: scale(1.02); 
    box-shadow: 0 0 20px rgba(102, 126, 234, 0.3);
  }
  100% { 
    transform: scale(1); 
    box-shadow: 0 0 0 rgba(102, 126, 234, 0.3);
  }
}

.stat-pulse {
  animation: gentlePulse 0.8s ease-in-out;
}

@media (max-width: 768px) {
  .title { font-size: 2.5rem; }
  .step-count { font-size: 3.5rem; }
  .stats-grid { grid-template-columns: 1fr 1fr; }
}
</style>
</head><body>
<div class='container'>
  <h1 class='title'>🎪 Garba Steps</h1>
  
  <div class='step-display'>
    <div class='step-count' id='totalSteps'>0</div>
    <div class='step-label'>Total Steps Counted</div>
  </div>
  
  <div class='stats-grid'>
    <div class='stat-card'>
      <div class='stat-value' id='totalDevices'>0</div>
      <div class='stat-label'>Active Devices</div>
    </div>
    <div class='stat-card'>
      <div class='stat-value' id='activeReceivers'>0</div>
      <div class='stat-label'>Receivers Online</div>
    </div>
    <div class='stat-card'>
      <div class='stat-value' id='uptime'>0s</div>
      <div class='stat-label'>Server Uptime</div>
    </div>
  </div>
</div>

<script>
let previousSteps = 0;
let previousDevices = 0;
let previousReceivers = 0;

function formatNumber(num) { 
  // For main display - show abbreviated form after 1,00,000 (1 Lakh)
  if (num >= 10000000) { // 1 Crore (10 Million)
    return (num / 10000000).toFixed(1) + 'Cr';
  } else if (num >= 100000) { // 1 Lakh (100 Thousand)
    return (num / 100000).toFixed(1) + 'L';
  } else {
    return num.toLocaleString();
  }
}

function formatTime(seconds) {
  if (seconds < 60) return seconds + 's';
  if (seconds < 3600) return Math.floor(seconds/60) + 'm';
  if (seconds < 86400) return Math.floor(seconds/3600) + 'h';
  return Math.floor(seconds/86400) + 'd';
}

function animateNumber(elementId, newValue, previousValue, delay = 0) {
  const element = document.getElementById(elementId);
  const difference = newValue - previousValue;
  
  if (difference > 0) {
    // Start animation after delay
    setTimeout(() => {
      // Add counting animation class
      element.classList.add('counting');
      
      // Much faster counting animation
      const duration = Math.min(800, Math.max(300, difference * 5)); // Faster duration
      const steps = Math.min(40, Math.max(15, difference)); // More steps for smoother but faster animation
      const increment = difference / steps;
      const stepDuration = duration / steps;
      
      let currentValue = previousValue;
      let stepCount = 0;
      
      const timer = setInterval(() => {
        stepCount++;
        currentValue += increment;
        
        if (stepCount >= steps) {
          currentValue = newValue;
          clearInterval(timer);
          element.classList.remove('counting');
        }
        
        element.textContent = formatNumber(Math.floor(currentValue));
      }, stepDuration);
    }, delay);
    
    return true; // Animation will start
  } else {
    // No change or decrease, just update
    setTimeout(() => {
      element.textContent = formatNumber(newValue);
    }, delay);
    return false;
  }
}

function showSmallStepPopup(increment) {
  const stepDisplay = document.querySelector('.step-display');
  const popup = document.createElement('div');
  popup.className = 'step-increment-popup';
  
  // Format large numbers with abbreviations for popup display
  let displayText;
  if (increment >= 1000000000000) { // Trillion
    displayText = '+' + (increment / 1000000000000).toFixed(1) + 'T';
  } else if (increment >= 1000000000) { // Billion
    displayText = '+' + (increment / 1000000000).toFixed(1) + 'B';
  } else if (increment >= 1000000) { // Million
    displayText = '+' + (increment / 1000000).toFixed(1) + 'M';
  } else if (increment >= 1000) { // Thousand
    displayText = '+' + (increment / 1000).toFixed(1) + 'K';
  } else {
    displayText = '+' + formatNumber(increment);
  }
  
  popup.textContent = displayText;
  
  // Array of vibrant colors that change each time
  const colors = [
    '#10b981', // Emerald
    '#f59e0b', // Amber
    '#ef4444', // Red
    '#3b82f6', // Blue
    '#8b5cf6', // Purple
    '#06b6d4', // Cyan
    '#f97316', // Orange
    '#84cc16', // Lime
    '#ec4899', // Pink
    '#14b8a6', // Teal
    '#6366f1', // Indigo
    '#f43f5e'  // Rose
  ];
  
  // Get random color
  const randomColor = colors[Math.floor(Math.random() * colors.length)];
  popup.style.color = randomColor;
  
  // More strategic positioning to avoid main number area
  const positions = [
    { top: '5%', right: '5%' },      // Far top right
    { top: '5%', left: '5%' },       // Far top left
    { bottom: '15%', right: '8%' },  // Bottom right
    { bottom: '15%', left: '8%' },   // Bottom left
    { top: '35%', right: '2%' },     // Mid right edge
    { top: '35%', left: '2%' },      // Mid left edge
    { top: '20%', right: '15%' },    // Upper right corner
    { top: '20%', left: '15%' },     // Upper left corner
    { bottom: '35%', right: '3%' },  // Lower right edge
    { bottom: '35%', left: '3%' }    // Lower left edge
  ];
  
  const randomPos = positions[Math.floor(Math.random() * positions.length)];
  Object.assign(popup.style, randomPos);
  
  stepDisplay.appendChild(popup);
  
  // Remove popup after animation completes
  setTimeout(() => {
    if (popup.parentNode) {
      popup.parentNode.removeChild(popup);
    }
  }, 2500);
}

function animateStatCard(elementId) {
  const element = document.getElementById(elementId);
  element.classList.add('animate');
  
  // Remove animation class after animation completes
  setTimeout(() => {
    element.classList.remove('animate');
  }, 400);
}

function updateDashboard() {
  fetch('/api/dashboard-data')
    .then(r => r.json())
    .then(data => {
      const newSteps = data.totalSteps;
      const newDevices = data.totalDevices;
      const newReceivers = data.activeReceivers;
      
      // Handle step count with glow effect and small popup
      if (newSteps > previousSteps) {
        const increment = newSteps - previousSteps;
        
        // Show small popup outside the main number
        showSmallStepPopup(increment);
        
        // Add glow effect to step counter
        const stepElement = document.getElementById('totalSteps');
        stepElement.classList.add('glow');
        setTimeout(() => stepElement.classList.remove('glow'), 600);
        
        // Start number counting animation
        animateNumber('totalSteps', newSteps, previousSteps, 0);
      } else {
        document.getElementById('totalSteps').textContent = formatNumber(newSteps);
      }
      
      // Animate devices count if changed
      if (newDevices !== previousDevices) {
        if (animateNumber('totalDevices', newDevices, previousDevices, 0)) {
          animateStatCard('totalDevices');
        }
      }
      
      // Animate receivers count if changed
      if (newReceivers !== previousReceivers) {
        if (animateNumber('activeReceivers', newReceivers, previousReceivers, 0)) {
          animateStatCard('activeReceivers');
        }
      }
      
      // Update uptime (no animation needed)
      document.getElementById('uptime').textContent = formatTime(data.uptime);
      
      // Store previous values for next comparison
      previousSteps = newSteps;
      previousDevices = newDevices;
      previousReceivers = newReceivers;
    })
    .catch(() => {
      console.error('Failed to update dashboard');
    });
}

// Initial load to set previous values
updateDashboard();

// Update every 2 seconds
setInterval(updateDashboard, 2000);
</script>
</body></html>
  `);
});

// ============================================================================
// SERVER STARTUP
// ============================================================================

// Cleanup and statistics intervals
setInterval(cleanupOldData, 30000);     // Every 30 seconds
setInterval(updateStatistics, 5000);    // Every 5 seconds

// Start server
app.listen(PORT, '0.0.0.0', () => {
  const localIP = getLocalIP();
  const pcName = getPCName();
  
  console.log('\n' + '='.repeat(60));
  console.log('   🎪 FRESH GARBA STEP COUNTER SERVER 🎪   ');
  console.log('='.repeat(60));
  console.log(`💻 PC Name: ${pcName}`);
  console.log(`🌐 Server IP: http://${localIP}:${PORT}`);
  console.log(`🎯 PC Domain: http://${pcName}.local:${PORT}`);
  console.log(`📊 Dashboard: http://${localIP}:${PORT}/`);
  console.log(`🔧 Debug Console: http://${localIP}:${PORT}/debug`);
  console.log('='.repeat(60));
  console.log(`✅ Use in ESP8266: ${localIP}:${PORT}`);
  console.log(`✅ Alternative: ${pcName}.local:${PORT}`);
  console.log('✅ Ready for enhanced device tracking!');
  console.log('='.repeat(60));
  
  addLog('system', `Server started on ${localIP}:${PORT}`);
});

module.exports = app;