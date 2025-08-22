const express = require('express');
const cors = require('cors');
const os = require('os');

const app = express();
const PORT = 3000;

// Get local IP address
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

// Get PC hostname
function getPCName() {
  return os.hostname();
}

// Middleware
app.use(cors());
app.use(express.json());
app.use(express.text());

// Simple data storage - exactly like ESP32
let devices = {};
let deviceReceiverMap = {};
let receiverLastSeen = {};
let receiverDeviceCount = {};

// Statistics
let totalSteps = 0;
let totalDevices = 0;
let activeReceivers = 0;

// Helper functions
function updateBestDeviceData(deviceId) {
  if (!deviceReceiverMap[deviceId]) return;
  
  let bestReceiver = '';
  let latestTime = 0;
  let bestSignal = -200;
  
  // Find receiver with most recent data and best signal
  for (let receiverId in deviceReceiverMap[deviceId]) {
    let data = deviceReceiverMap[deviceId][receiverId];
    
    let isBetter = false;
    if (data.timestamp > latestTime) {
      isBetter = true;
    } else if (Math.abs(data.timestamp - latestTime) < 10000 && data.signalStrength > bestSignal) {
      isBetter = true;
    }
    
    if (isBetter) {
      bestReceiver = receiverId;
      latestTime = data.timestamp;
      bestSignal = data.signalStrength;
    }
  }
  
  // Update main device record with best data
  if (bestReceiver) {
    let bestData = deviceReceiverMap[deviceId][bestReceiver];
    devices[deviceId] = {
      stepCount: bestData.stepCount,
      batteryLevel: bestData.batteryLevel,
      bestReceiverId: bestReceiver,
      lastSeen: bestData.timestamp,
      signalStrength: bestData.signalStrength
    };
  }
}

function updateStatistics() {
  totalSteps = 0;
  totalDevices = Object.keys(devices).length;
  
  for (let deviceId in devices) {
    totalSteps += devices[deviceId].stepCount;
  }
  
  // Count active receivers
  activeReceivers = 0;
  let now = Date.now();
  for (let receiverId in receiverLastSeen) {
    if (now - receiverLastSeen[receiverId] < 30000) {
      activeReceivers++;
    }
  }
}

function cleanupOldData() {
  let now = Date.now();
  let deviceTimeout = 300000; // 5 minutes
  let receiverTimeout = 60000; // 1 minute
  
  // Clean old devices
  for (let deviceId in devices) {
    if (now - devices[deviceId].lastSeen > deviceTimeout) {
      delete devices[deviceId];
      delete deviceReceiverMap[deviceId];
    }
  }
  
  // Clean old receivers
  for (let receiverId in receiverLastSeen) {
    if (now - receiverLastSeen[receiverId] > receiverTimeout) {
      delete receiverLastSeen[receiverId];
      delete receiverDeviceCount[receiverId];
    }
  }
}

// API endpoint - exactly like ESP32
app.post('/api/receiver-data', (req, res) => {
  try {
    let data = req.body;
    let receiverId = data.receiverId;
    let timestamp = data.timestamp;
    let deviceCount = data.deviceCount;
    let deviceList = data.devices;
    
    console.log(`📡 Received data from ${receiverId} with ${deviceList.length} devices`);
    
    // Update receiver status
    receiverLastSeen[receiverId] = Date.now();
    receiverDeviceCount[receiverId] = deviceCount;
    
    // Process each device
    for (let device of deviceList) {
      let deviceId = device.deviceId;
      let stepCount = device.stepCount;
      let batteryLevel = device.batteryLevel;
      let lastSeen = device.lastSeen;
      let signalStrength = device.signalStrength;
      
      // Store data for this receiver-device combination
      if (!deviceReceiverMap[deviceId]) {
        deviceReceiverMap[deviceId] = {};
      }
      
      deviceReceiverMap[deviceId][receiverId] = {
        stepCount: stepCount,
        batteryLevel: batteryLevel,
        timestamp: Date.now() - (lastSeen * 1000),
        signalStrength: signalStrength
      };
      
      // Update best device data
      updateBestDeviceData(deviceId);
      
      console.log(`  Device ${deviceId.substring(12)}: ${stepCount} steps, ${batteryLevel}% battery`);
    }
    
    // Update statistics
    updateStatistics();
    
    // Send exact response like ESP32
    res.send('Data processed successfully');
    
  } catch (error) {
    console.error('Error processing data:', error);
    res.status(500).send('Error processing data');
  }
});

// Dashboard data API
app.get('/api/dashboard-data', (req, res) => {
  updateStatistics();
  res.json({
    totalSteps: totalSteps,
    totalDevices: totalDevices,
    activeReceivers: activeReceivers,
    systemStatus: 'Online',
    uptime: process.uptime()
  });
});

// Debug data API
app.get('/api/debug-data', (req, res) => {
  let receivers = [];
  let now = Date.now();
  
  for (let receiverId in receiverLastSeen) {
    if (now - receiverLastSeen[receiverId] > 60000) continue;
    
    let devicesForReceiver = [];
    let totalStepsForReceiver = 0;
    
    for (let deviceId in devices) {
      if (devices[deviceId].bestReceiverId === receiverId) {
        totalStepsForReceiver += devices[deviceId].stepCount;
        devicesForReceiver.push({
          deviceId: deviceId,
          stepCount: devices[deviceId].stepCount,
          batteryLevel: devices[deviceId].batteryLevel,
          lastSeen: Math.floor((now - devices[deviceId].lastSeen) / 1000)
        });
      }
    }
    
    receivers.push({
      id: receiverId.substring(12) || receiverId,
      fullId: receiverId,
      deviceCount: receiverDeviceCount[receiverId] || 0,
      totalSteps: totalStepsForReceiver,
      lastSeen: Math.floor((now - receiverLastSeen[receiverId]) / 1000) + 's ago',
      devices: devicesForReceiver
    });
  }
  
  res.json({ receivers: receivers });
});

// Reset API
app.post('/api/reset', (req, res) => {
  devices = {};
  deviceReceiverMap = {};
  receiverLastSeen = {};
  receiverDeviceCount = {};
  totalSteps = 0;
  totalDevices = 0;
  activeReceivers = 0;
  
  console.log('🔄 System reset - all data cleared');
  res.send('System reset successfully');
});

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
  font-family: 'Georgia', serif; 
  background: linear-gradient(135deg, #8B5A2B 0%, #CD853F 25%, #DAA520 50%, #FF6347 75%, #DC143C 100%); 
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
}
.title { 
  font-size: 3.5rem; 
  font-weight: bold; 
  background: linear-gradient(45deg, #8B4513, #DAA520, #FF6347, #DC143C); 
  -webkit-background-clip: text; 
  -webkit-text-fill-color: transparent; 
  margin-bottom: 2rem; 
}
.step-display { 
  background: linear-gradient(135deg, #FF6347, #DC143C); 
  padding: 4rem 2rem; 
  border-radius: 15px; 
  margin: 2rem 0; 
}
.step-count { 
  font-size: 6rem; 
  font-weight: 900; 
  color: white; 
  text-shadow: 3px 3px 6px rgba(0,0,0,0.5); 
  margin-bottom: 1rem; 
  font-family: 'Impact', sans-serif; 
}
.step-label { 
  font-size: 1.8rem; 
  color: white; 
  font-weight: 600; 
  text-transform: uppercase; 
  letter-spacing: 3px; 
}
.status-info { 
  background: rgba(139, 69, 19, 0.1); 
  padding: 1.5rem; 
  border-radius: 10px; 
  margin-top: 2rem; 
}
.status-item { 
  display: inline-block; 
  margin: 0 1rem; 
  padding: 0.5rem 1rem; 
  background: rgba(218, 165, 32, 0.2); 
  border-radius: 20px; 
  color: #8B4513; 
  font-weight: 600; 
}
@media (max-width: 768px) {
  .title { font-size: 2.5rem; }
  .step-count { font-size: 4rem; }
  .status-item { display: block; margin: 0.5rem 0; }
}
</style>
</head><body>
<div class='container'>
  <h1 class='title'>Garba Steps</h1>
  <div class='step-display'>
    <div class='step-count' id='totalSteps'>0</div>
    <div class='step-label'>Steps Counted</div>
  </div>
  <div class='status-info'>
    <div class='status-item'>System: <span id='systemStatus'>Loading...</span></div>
    <div class='status-item'>Devices: <span id='totalDevices'>0</span></div>
    <div class='status-item'>Stations: <span id='activeReceivers'>0</span></div>
  </div>
  <div style='margin-top: 1rem; color: #8B4513; font-size: 0.9rem;'>
    Last Updated: <span id='lastUpdate'>Loading...</span>
  </div>
</div>

<script>
function formatNumber(num) { 
  return num.toLocaleString(); 
}

function updateDashboard() {
  fetch('/api/dashboard-data')
    .then(r => r.json())
    .then(data => {
      document.getElementById('totalSteps').textContent = formatNumber(data.totalSteps);
      document.getElementById('totalDevices').textContent = data.totalDevices;
      document.getElementById('activeReceivers').textContent = data.activeReceivers;
      document.getElementById('systemStatus').textContent = data.systemStatus;
      document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
    })
    .catch(() => {
      document.getElementById('systemStatus').textContent = 'Error';
    });
}

setInterval(updateDashboard, 2000);
updateDashboard();
</script>
</body></html>
  `);
});

// Debug console
app.get('/debugger', (req, res) => {
  res.send(`
<!DOCTYPE html>
<html><head>
<title>Debug Console</title>
<style>
body { font-family: monospace; background: #0d1117; color: #c9d1d9; padding: 2rem; }
.header { background: #21262d; padding: 1rem; border-radius: 6px; margin-bottom: 1rem; }
.stats { display: grid; grid-template-columns: repeat(4, 1fr); gap: 1rem; margin-bottom: 1rem; }
.stat { background: #21262d; padding: 1rem; border-radius: 6px; text-align: center; }
.stat-value { font-size: 1.5rem; color: #58a6ff; font-weight: bold; }
.controls { background: #21262d; padding: 1rem; border-radius: 6px; margin-bottom: 1rem; }
.btn { padding: 0.5rem 1rem; margin: 0.5rem; border: none; border-radius: 4px; cursor: pointer; }
.btn-danger { background: #da3633; color: white; }
.receivers { background: #21262d; padding: 1rem; border-radius: 6px; }
</style>
</head><body>
<div class='header'>
  <h1>Debug Console - Node.js Server</h1>
</div>

<div class='stats'>
  <div class='stat'><div class='stat-value' id='totalSteps'>0</div><div>Total Steps</div></div>
  <div class='stat'><div class='stat-value' id='totalDevices'>0</div><div>Devices</div></div>
  <div class='stat'><div class='stat-value' id='activeReceivers'>0</div><div>Receivers</div></div>
  <div class='stat'><div class='stat-value' id='uptime'>0</div><div>Uptime (s)</div></div>
</div>

<div class='controls'>
  <button class='btn btn-danger' onclick='resetSystem()'>Reset All Data</button>
  <button class='btn' onclick='refreshData()' style='background:#238636;color:white;'>Refresh</button>
</div>

<div class='receivers'>
  <h3>Active Receivers</h3>
  <div id='receiverData'>Loading...</div>
</div>

<script>
function updateStats() {
  fetch('/api/dashboard-data')
    .then(r => r.json())
    .then(data => {
      document.getElementById('totalSteps').textContent = data.totalSteps.toLocaleString();
      document.getElementById('totalDevices').textContent = data.totalDevices;
      document.getElementById('activeReceivers').textContent = data.activeReceivers;
      document.getElementById('uptime').textContent = Math.floor(data.uptime);
    });
    
  fetch('/api/debug-data')
    .then(r => r.json())
    .then(data => {
      let html = '';
      if (data.receivers.length === 0) {
        html = '<p>No receivers connected</p>';
      } else {
        data.receivers.forEach(receiver => {
          html += '<div style="margin:1rem 0; padding:1rem; background:#0d1117; border-radius:4px;">';
          html += '<h4>Receiver ' + receiver.id + ' (' + receiver.deviceCount + ' devices, ' + receiver.totalSteps.toLocaleString() + ' steps)</h4>';
          html += '<p>Last seen: ' + receiver.lastSeen + '</p>';
          if (receiver.devices.length > 0) {
            html += '<div style="margin-top:0.5rem; font-size:0.9rem;">';
            receiver.devices.forEach(device => {
              html += '<div>• Device ' + device.deviceId.substring(9) + ': ' + device.stepCount.toLocaleString() + ' steps, ' + device.batteryLevel + '% battery</div>';
            });
            html += '</div>';
          }
          html += '</div>';
        });
      }
      document.getElementById('receiverData').innerHTML = html;
    });
}

function resetSystem() {
  if (confirm('Reset ALL data? This cannot be undone!')) {
    fetch('/api/reset', {method: 'POST'})
      .then(() => {
        alert('System reset successfully');
        refreshData();
      });
  }
}

function refreshData() {
  updateStats();
}

setInterval(updateStats, 3000);
updateStats();
</script>
</body></html>
  `);
});

// Cleanup old data every 30 seconds
setInterval(() => {
  cleanupOldData();
}, 30000);

// Update statistics every 5 seconds
setInterval(() => {
  updateStatistics();
}, 5000);

// Start server
app.listen(PORT, '0.0.0.0', () => {
  const localIP = getLocalIP();
  const pcName = getPCName();
  
  console.log('\n========================================');
  console.log('   🎪 GARBA STEP COUNTER SERVER 🎪    ');
  console.log('========================================');
  console.log(`💻 PC Name: ${pcName}`);
  console.log(`🌐 Server IP: http://${localIP}:${PORT}`);
  console.log(`🎯 PC Domain: http://${pcName}.local:${PORT}`);
  console.log(`📊 Dashboard: http://${pcName}.local:${PORT}/`);
  console.log(`🔧 Debug Console: http://${pcName}.local:${PORT}/debugger`);
  console.log('========================================');
  console.log('✅ Ready to receive data from ESP8266 receivers!');
  console.log(`✅ Use in ESP8266 code: ${pcName}.local:${PORT}`);
  console.log(`✅ Alternative IP: ${localIP}:${PORT}`);
  console.log('========================================\n');
});

module.exports = app;