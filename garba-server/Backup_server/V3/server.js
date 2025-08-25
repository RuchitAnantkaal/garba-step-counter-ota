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
const deviceHistory = new Map();       // Already exists, but enhance it
const offlineDevices = new Map();      // NEW: Store offline device data
const systemHistory = [];              // NEW: Store system statistics history
const fs = require('fs').promises;
const path = require('path');

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

const DATA_DIR = './data';
const DEVICE_HISTORY_FILE = path.join(DATA_DIR, 'device_history.json');
const SYSTEM_HISTORY_FILE = path.join(DATA_DIR, 'system_history.json');
const OFFLINE_DEVICES_FILE = path.join(DATA_DIR, 'offline_devices.json');

async function ensureDataDirectory() {
  try {
    await fs.access(DATA_DIR);
  } catch (error) {
    await fs.mkdir(DATA_DIR, { recursive: true });
    console.log('📁 Created data directory for persistence');
  }
}

async function saveHistoryData() {
  try {
    await ensureDataDirectory();
    
    // Convert Maps to Objects for JSON serialization
    const deviceHistoryObj = {};
    for (const [key, value] of deviceHistory.entries()) {
      deviceHistoryObj[key] = value;
    }
    
    const offlineDevicesObj = {};
    for (const [key, value] of offlineDevices.entries()) {
      offlineDevicesObj[key] = value;
    }
    
    // Save device history
    await fs.writeFile(DEVICE_HISTORY_FILE, JSON.stringify(deviceHistoryObj, null, 2));
    
    // Save system history (limit to last 500 records to prevent huge files)
    const limitedSystemHistory = systemHistory.slice(0, 500);
    await fs.writeFile(SYSTEM_HISTORY_FILE, JSON.stringify(limitedSystemHistory, null, 2));
    
    // Save offline devices
    await fs.writeFile(OFFLINE_DEVICES_FILE, JSON.stringify(offlineDevicesObj, null, 2));
    
    console.log('💾 History data saved successfully');
    
  } catch (error) {
    console.error('❌ Failed to save history data:', error.message);
  }
}

async function loadHistoryData() {
  try {
    await ensureDataDirectory();
    
    // Load device history
    try {
      const deviceHistoryData = await fs.readFile(DEVICE_HISTORY_FILE, 'utf8');
      const deviceHistoryObj = JSON.parse(deviceHistoryData);
      deviceHistory.clear();
      for (const [key, value] of Object.entries(deviceHistoryObj)) {
        deviceHistory.set(key, value);
      }
      console.log(`📚 Loaded ${deviceHistory.size} device histories`);
    } catch (error) {
      console.log('📚 No existing device history file found');
    }
    
    // Load system history
    try {
      const systemHistoryData = await fs.readFile(SYSTEM_HISTORY_FILE, 'utf8');
      const systemHistoryArray = JSON.parse(systemHistoryData);
      systemHistory.length = 0;
      systemHistory.push(...systemHistoryArray);
      console.log(`📊 Loaded ${systemHistory.length} system history records`);
    } catch (error) {
      console.log('📊 No existing system history file found');
    }
    
    // Load offline devices
    try {
      const offlineDevicesData = await fs.readFile(OFFLINE_DEVICES_FILE, 'utf8');
      const offlineDevicesObj = JSON.parse(offlineDevicesData);
      offlineDevices.clear();
      for (const [key, value] of Object.entries(offlineDevicesObj)) {
        offlineDevices.set(key, value);
      }
      console.log(`📱 Loaded ${offlineDevices.size} offline devices`);
    } catch (error) {
      console.log('📱 No existing offline devices file found');
    }
    
  } catch (error) {
    console.error('❌ Failed to load history data:', error.message);
  }
}

async function clearHistoryFiles() {
  try {
    await ensureDataDirectory();
    
    // Clear the files by writing empty data
    await fs.writeFile(DEVICE_HISTORY_FILE, JSON.stringify({}, null, 2));
    await fs.writeFile(SYSTEM_HISTORY_FILE, JSON.stringify([], null, 2));
    await fs.writeFile(OFFLINE_DEVICES_FILE, JSON.stringify({}, null, 2));
    
    console.log('🗑️ History files cleared');
    
  } catch (error) {
    console.error('❌ Failed to clear history files:', error.message);
  }
}

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
  let wasReconnecting = false;
  
  if (!deviceHistory.has(deviceId)) {
    // Check if this device has history from being offline
    if (offlineDevices.has(deviceId) && offlineDevices.get(deviceId).history) {
      // Restore the history from offline state
      const offlineHistory = offlineDevices.get(deviceId).history;
      deviceHistory.set(deviceId, {
        ...offlineHistory,
        lastOnlineTime: Date.now() // Update the last online time
      });
      wasReconnecting = true;
      addLog('device-update', `Device ${deviceId.substring(9)} history restored from offline state`, { deviceId });
    } else {
      // Completely new device
      deviceHistory.set(deviceId, {
        firstSeen: Date.now(),
        totalUpdates: 0,
        peakSteps: 0,
        minBattery: 100,
        maxBattery: 0,
        lastOnlineTime: Date.now(),
        totalOnlineTime: 0,
        disconnectionCount: 0
      });
      addLog('device-connect', `New device ${deviceId.substring(9)} connected`, { deviceId });
    }
  }
  
  const history = deviceHistory.get(deviceId);
  history.totalUpdates++;
  history.lastUpdate = Date.now();
  history.lastOnlineTime = Date.now();
  
  if (stepCount > history.peakSteps) {
    history.peakSteps = stepCount;
    if (!wasReconnecting) {
      addLog('device-update', `Device ${deviceId.substring(9)} reached ${stepCount} steps (new peak)`, { deviceId, stepCount });
    }
  }
  
  if (batteryLevel < history.minBattery) history.minBattery = batteryLevel;
  if (batteryLevel > history.maxBattery) history.maxBattery = batteryLevel;
}

function checkForReconnectingDevices(receivedDeviceIds) {
  const reconnectedDevices = [];
  
  // Check if any offline devices are now sending data again
  for (const deviceId of receivedDeviceIds) {
    if (offlineDevices.has(deviceId)) {
      reconnectedDevices.push(deviceId);
    }
  }
  
  return reconnectedDevices;
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
  
  // Move offline devices to offline storage before cleaning
  for (const [deviceId, device] of deviceData.entries()) {
    if (device.lastSeen < fiveMinutesAgo) {
      // Check if this device is already in offline storage
      if (!offlineDevices.has(deviceId)) {
        // Store in offline devices
        const history = deviceHistory.get(deviceId);
        offlineDevices.set(deviceId, {
          ...device,
          offlineTime: now,
          history: history ? {...history} : null
        });
        
        // Update history with disconnection info
        if (history) {
          history.disconnectionCount++;
          history.totalOnlineTime += (now - history.lastOnlineTime);
        }
        
        addLog('device-disconnect', `Device ${deviceId.substring(9)} went offline (inactive for 5+ minutes)`, { deviceId });
      }
      
      // Remove from active devices
      deviceData.delete(deviceId);
      deviceToReceiver.delete(deviceId);
      cleanedDevices++;
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
    addLog('system', `Cleanup: moved ${cleanedDevices} devices offline, removed ${cleanedReceivers} disconnected receivers`);
    console.log(`🧹 CLEANUP: Moved ${cleanedDevices} devices offline, removed ${cleanedReceivers} old receivers`);
  }
}

function recordSystemHistory() {
  const now = Date.now();
  updateStatistics(); // Ensure stats are current
  
  // Get list of offline device IDs and names
  const offlineDeviceList = [];
  for (const [deviceId, device] of offlineDevices.entries()) {
    offlineDeviceList.push({
      id: deviceId,
      name: deviceId.substring(9),
      offlineSince: new Date(device.offlineTime).toLocaleString(),
      offlineDuration: Math.floor((now - device.offlineTime) / 1000),
      lastSteps: device.stepCount,
      lastBattery: device.batteryLevel
    });
  }
  
  // Sort offline devices by offline duration (longest offline first)
  offlineDeviceList.sort((a, b) => b.offlineDuration - a.offlineDuration);
  
  const historyRecord = {
    timestamp: now,
    dateTime: new Date(now).toLocaleString(),
    totalSteps: serverStats.totalSteps,
    activeDevices: serverStats.activeDevices,
    activeReceivers: serverStats.activeReceivers,
    totalRequests: serverStats.totalRequests,
    offlineDevices: offlineDevices.size,
    offlineDeviceList: offlineDeviceList, // NEW: Detailed offline device info
    uptime: Math.floor((now - serverStats.startTime) / 1000)
  };
  
  systemHistory.unshift(historyRecord);
  
  // Keep only last 500 records in memory
  if (systemHistory.length > 500) {
    systemHistory.length = 500;
  }
  
  // Save to file every 10 records to avoid excessive I/O
  if (systemHistory.length % 10 === 0) {
    saveHistoryData().catch(err => console.error('Failed to save history:', err));
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
      
      // Check if this device was previously offline and is now reconnecting
      if (offlineDevices.has(deviceId)) {
        console.log(`  🔄 Device ${deviceId.substring(9)} reconnecting from offline state`);
        
        // Get the offline device data
        const offlineDevice = offlineDevices.get(deviceId);
        
        // Restore device history if it exists
        if (offlineDevice.history && !deviceHistory.has(deviceId)) {
          deviceHistory.set(deviceId, offlineDevice.history);
        }
        
        // Remove from offline devices
        offlineDevices.delete(deviceId);
        
        // Log the reconnection
        addLog('device-connect', `Device ${deviceId.substring(9)} reconnected (was offline)`, { deviceId });
      }
      
      // Update device history (this will create new entry if needed)
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
        
        // Update main device record (this moves it back to active devices)
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

app.get('/api/offline-summary', (req, res) => {
  const now = Date.now();
  const offlineDeviceSummary = [];
  
  for (const [deviceId, device] of offlineDevices.entries()) {
    const offlineDuration = Math.floor((now - device.offlineTime) / 1000);
    
    offlineDeviceSummary.push({
      id: deviceId,
      name: deviceId.substring(9),
      lastSteps: device.stepCount,
      lastBattery: device.batteryLevel,
      lastReceiver: device.bestReceiverId ? device.bestReceiverId.substring(12) : 'Unknown',
      offlineTime: offlineDuration,
      offlineSince: new Date(device.offlineTime).toLocaleString(),
      peakSteps: device.history ? device.history.peakSteps : device.stepCount,
      totalUpdates: device.history ? device.history.totalUpdates : 0,
      disconnectionCount: device.history ? device.history.disconnectionCount : 1
    });
  }
  
  // Sort by offline duration (longest offline first)
  offlineDeviceSummary.sort((a, b) => b.offlineTime - a.offlineTime);
  
  res.json({
    totalOffline: offlineDeviceSummary.length,
    offlineDevices: offlineDeviceSummary
  });
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
// History data endpoint
app.get('/api/history', (req, res) => {
  recordSystemHistory(); // Record current state
  
  const allDevicesHistory = [];
  const now = Date.now();
  
  // Add online devices history
  for (const [deviceId, device] of deviceData.entries()) {
    const history = deviceHistory.get(deviceId);
    if (history) {
      allDevicesHistory.push({
        id: deviceId,
        name: deviceId.substring(9),
        status: 'Online',
        currentSteps: device.stepCount,
        peakSteps: history.peakSteps,
        batteryLevel: device.batteryLevel,
        minBattery: history.minBattery,
        maxBattery: history.maxBattery,
        firstSeen: new Date(history.firstSeen).toLocaleString(),
        lastUpdate: new Date(history.lastUpdate || history.lastOnlineTime).toLocaleString(),
        totalUpdates: history.totalUpdates,
        disconnectionCount: history.disconnectionCount,
        totalOnlineTime: Math.floor((now - history.firstSeen) / 1000),
        receiverId: device.bestReceiverId,
        receiverName: device.bestReceiverId ? device.bestReceiverId.substring(12) : 'Unknown',
        onlineDuration: Math.floor((now - (history.lastUpdate || history.lastOnlineTime)) / 1000)
      });
    }
  }
  
  // Add offline devices history
  for (const [deviceId, device] of offlineDevices.entries()) {
    const history = device.history;
    if (history) {
      allDevicesHistory.push({
        id: deviceId,
        name: deviceId.substring(9),
        status: 'Offline',
        currentSteps: device.stepCount,
        peakSteps: history.peakSteps,
        batteryLevel: device.batteryLevel,
        minBattery: history.minBattery,
        maxBattery: history.maxBattery,
        firstSeen: new Date(history.firstSeen).toLocaleString(),
        lastUpdate: new Date(device.offlineTime).toLocaleString(),
        totalUpdates: history.totalUpdates,
        disconnectionCount: history.disconnectionCount,
        totalOnlineTime: Math.floor(history.totalOnlineTime / 1000),
        receiverId: device.bestReceiverId,
        receiverName: device.bestReceiverId ? device.bestReceiverId.substring(12) : 'Unknown',
        offlineTime: Math.floor((now - device.offlineTime) / 1000),
        offlineSince: new Date(device.offlineTime).toLocaleString()
      });
    }
  }
  
  res.json({
    devices: allDevicesHistory,
    systemHistory: systemHistory
  });
});

// Clear history endpoint
app.post('/api/clear-history', async (req, res) => {
  try {
    // Clear memory
    systemHistory.length = 0;
    offlineDevices.clear();
    
    // Clear device history but keep current connections
    for (const [deviceId, history] of deviceHistory.entries()) {
      if (deviceData.has(deviceId)) {
        // Reset history for online devices but keep them connected
        deviceHistory.set(deviceId, {
          firstSeen: Date.now(),
          totalUpdates: 0,
          peakSteps: deviceData.get(deviceId).stepCount,
          minBattery: deviceData.get(deviceId).batteryLevel,
          maxBattery: deviceData.get(deviceId).batteryLevel,
          lastOnlineTime: Date.now(),
          totalOnlineTime: 0,
          disconnectionCount: 0
        });
      } else {
        // Remove history for offline devices
        deviceHistory.delete(deviceId);
      }
    }
    
    // Clear files
    await clearHistoryFiles();
    
    addLog('system', 'History cleared - device histories reset and files cleared');
    console.log('📜 HISTORY CLEARED: All historical data cleared from memory and files');
    res.send('History cleared successfully');
    
  } catch (error) {
    console.error('❌ Failed to clear history:', error);
    res.status(500).send('Failed to clear history');
  }
});

app.get('/api/data-info', async (req, res) => {
  try {
    const info = {
      dataDirectory: DATA_DIR,
      files: {}
    };
    
    // Check each file
    for (const [name, filepath] of [
      ['deviceHistory', DEVICE_HISTORY_FILE],
      ['systemHistory', SYSTEM_HISTORY_FILE],
      ['offlineDevices', OFFLINE_DEVICES_FILE]
    ]) {
      try {
        const stats = await fs.stat(filepath);
        info.files[name] = {
          exists: true,
          size: stats.size,
          modified: stats.mtime.toLocaleString(),
          path: filepath
        };
      } catch (error) {
        info.files[name] = {
          exists: false,
          path: filepath
        };
      }
    }
    
    res.json(info);
  } catch (error) {
    res.status(500).json({ error: 'Failed to get data info' });
  }
});

app.get('/api/devices', (req, res) => {
  const devices = [];
  const now = Date.now();
  const fiveMinutesAgo = now - 300000;

  // Add online devices
  for (const [deviceId, device] of deviceData.entries()) {
    const history = deviceHistory.get(deviceId);
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

  // Add offline devices
  for (const [deviceId, device] of offlineDevices.entries()) {
    const history = device.history;

    devices.push({
      id: deviceId,
      name: deviceId.substring(9),
      stepCount: device.stepCount,
      batteryLevel: device.batteryLevel,
      signalStrength: device.signalStrength,
      receiverId: device.bestReceiverId,
      receiverName: device.bestReceiverId ? device.bestReceiverId.substring(12) : 'Unknown',
      lastSeen: Math.floor((now - device.offlineTime) / 1000),
      status: 'Offline',
      firstSeen: history ? new Date(history.firstSeen).toLocaleString() : 'Unknown',
      totalUpdates: history ? history.totalUpdates : 0,
      peakSteps: history ? history.peakSteps : device.stepCount,
      minBattery: history ? history.minBattery : device.batteryLevel,
      maxBattery: history ? history.maxBattery : device.batteryLevel,
      offlineTime: Math.floor((now - device.offlineTime) / 1000)
    });
  }

  // Sort: Online first, then by step count
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

app.get('/debug', (req, res) => {
  res.send(`
<!DOCTYPE html>
<html><head>
<title>Debug Console - Garba Server</title>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { 
  font-family: 'Segoe UI', sans-serif; 
  background: #0a0e16; 
  color: #e6edf3; 
  min-height: 100vh;
}
.header { 
  background: linear-gradient(135deg, #1e293b, #334155); 
  padding: 1.5rem 2rem; 
  border-bottom: 2px solid #475569; 
  box-shadow: 0 4px 6px rgba(0,0,0,0.1);
}
.header h1 { 
  color: #60a5fa; 
  margin: 0; 
  font-size: 1.8rem;
  font-weight: 700;
}
.header p { 
  color: #94a3b8; 
  margin-top: 0.5rem;
}
.nav-bar { 
  background: #1e293b; 
  padding: 1rem 2rem; 
  border-bottom: 1px solid #334155; 
  display: flex;
  gap: 1rem;
  flex-wrap: wrap;
}
.nav-btn { 
  padding: 0.7rem 1.5rem; 
  background: #334155; 
  color: #e6edf3; 
  text-decoration: none; 
  border-radius: 8px; 
  border: none; 
  cursor: pointer; 
  transition: all 0.2s;
  font-weight: 500;
}
.nav-btn:hover { background: #475569; }
.nav-btn.active { background: #3b82f6; color: white; }
.nav-btn.home { background: #059669; }
.nav-btn.danger { background: #dc2626; }
.nav-btn.warning { background: #f59e0b; }
.container { 
  padding: 2rem; 
  max-width: 1400px; 
  margin: 0 auto; 
}
.stats-grid { 
  display: grid; 
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); 
  gap: 1.5rem; 
  margin-bottom: 2rem; 
}
.stat-card { 
  background: linear-gradient(135deg, #1e293b, #334155); 
  padding: 1.5rem; 
  border-radius: 12px; 
  text-align: center; 
  border: 1px solid #475569;
  box-shadow: 0 4px 6px rgba(0,0,0,0.1);
}
.stat-value { 
  font-size: 2.2rem; 
  color: #60a5fa; 
  font-weight: bold; 
  margin-bottom: 0.5rem; 
}
.stat-label { 
  color: #94a3b8; 
  font-size: 0.9rem; 
  font-weight: 500;
}
.section { 
  background: linear-gradient(135deg, #1e293b, #334155); 
  border-radius: 12px; 
  padding: 1.5rem; 
  margin-bottom: 1.5rem; 
  border: 1px solid #475569;
  box-shadow: 0 4px 6px rgba(0,0,0,0.1);
}
.section h3 { 
  color: #f8fafc; 
  margin-bottom: 1rem; 
  font-size: 1.3rem;
  font-weight: 600;
}
.table { 
  width: 100%; 
  border-collapse: collapse; 
  background: rgba(15, 23, 42, 0.5);
  border-radius: 8px;
  overflow: hidden;
}
.table th, .table td { 
  padding: 1rem; 
  text-align: left; 
  border-bottom: 1px solid #334155; 
}
.table th { 
  background: #0f172a; 
  color: #94a3b8; 
  font-weight: 600; 
  font-size: 0.9rem;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}
.table tr:hover { 
  background: rgba(51, 65, 85, 0.5); 
}
.status-online { 
  color: #10b981; 
  font-weight: bold; 
}
.status-offline { 
  color: #ef4444; 
  font-weight: bold; 
}
.device-link, .receiver-link { 
  color: #60a5fa; 
  text-decoration: none; 
  cursor: pointer; 
  font-weight: 500;
}
.device-link:hover, .receiver-link:hover { 
  text-decoration: underline; 
  color: #93c5fd;
}
.log-entry { 
  padding: 1rem; 
  margin-bottom: 0.8rem; 
  background: rgba(15, 23, 42, 0.7); 
  border-radius: 8px; 
  border-left: 4px solid #60a5fa; 
}
.log-entry.device-connect { border-left-color: #10b981; }
.log-entry.device-disconnect { border-left-color: #ef4444; }
.log-entry.receiver-connect { border-left-color: #f59e0b; }
.log-entry.receiver-disconnect { border-left-color: #ef4444; }
.log-entry.system { border-left-color: #8b5cf6; }
.log-time { 
  color: #94a3b8; 
  font-size: 0.8rem; 
  font-weight: 500;
}
.log-message {
  margin-top: 0.3rem;
  font-weight: 500;
}
.hidden { display: none; }
.controls { 
  margin-bottom: 1.5rem; 
  display: flex;
  gap: 1rem;
  flex-wrap: wrap;
}
.empty-state {
  text-align: center;
  padding: 3rem;
  color: #64748b;
  font-style: italic;
}
@media (max-width: 768px) {
  .nav-bar { flex-direction: column; }
  .stats-grid { grid-template-columns: 1fr 1fr; }
  .table { font-size: 0.9rem; }
  .table th, .table td { padding: 0.7rem; }
}
</style>
</head><body>

<div class='header'>
  <h1>🔧 Debug Console</h1>
  <p>Enhanced monitoring and device management</p>
</div>

<div class='nav-bar'>
  <button class='nav-btn active' onclick='showView("overview")'>📊 Overview</button>
  <button class='nav-btn' onclick='showView("receivers")'>📡 Receivers</button>
  <button class='nav-btn' onclick='showView("devices")'>📱 All Devices</button>
  <button class='nav-btn' onclick='showView("history")'>📜 History</button>
  <button class='nav-btn' onclick='showView("logs")'>📋 Connection Logs</button>
  <a href='/' class='nav-btn home'>🏠 Main Dashboard</a>
  <button class='nav-btn warning' onclick='clearHistory()'>🗂️ Clear History</button>
  <button class='nav-btn danger' onclick='resetSystem()'>🗑️ Reset System</button>
</div>

<div class='container'>
  <!-- Overview View -->
  <div id='overview-view'>
    <div class='stats-grid'>
      <div class='stat-card'>
        <div class='stat-value' id='totalSteps'>0</div>
        <div class='stat-label'>Total Steps</div>
      </div>
      <div class='stat-card'>
        <div class='stat-value' id='totalDevices'>0</div>
        <div class='stat-label'>Active Devices</div>
      </div>
      <div class='stat-card'>
        <div class='stat-value' id='activeReceivers'>0</div>
        <div class='stat-label'>Active Receivers</div>
      </div>
      <div class='stat-card'>
        <div class='stat-value' id='totalRequests'>0</div>
        <div class='stat-label'>Total Requests</div>
      </div>
      <div class='stat-card'>
        <div class='stat-value' id='uptime'>0s</div>
        <div class='stat-label'>Server Uptime</div>
      </div>
    </div>
    
    <div class='section'>
      <h3>📡 Active Receivers Summary</h3>
      <div id='receiversOverview'>Loading...</div>
    </div>
    
    <div class='section'>
      <h3>🏆 Top Devices</h3>
      <div id='topDevices'>Loading...</div>
    </div>
  </div>

  <!-- Receivers View -->
  <div id='receivers-view' class='hidden'>
    <div class='section'>
      <h3>📡 All Receivers</h3>
      <div style='overflow-x: auto;'>
        <table class='table'>
          <thead>
            <tr>
              <th>Receiver ID</th>
              <th>Devices</th>
              <th>Total Steps</th>
              <th>Status</th>
              <th>Last Seen</th>
              <th>First Connected</th>
              <th>Actions</th>
            </tr>
          </thead>
          <tbody id='receiversTable'>
            <tr><td colspan='7' class='empty-state'>Loading receivers...</td></tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>

  <!-- All Devices View -->
  <div id='devices-view' class='hidden'>
    <div class='section'>
      <h3>📱 All Connected Devices (Online & Offline)</h3>
      <div style='overflow-x: auto;'>
        <table class='table'>
          <thead>
            <tr>
              <th>Device ID</th>
              <th>Current Steps</th>
              <th>Peak Steps</th>
              <th>Battery</th>
              <th>Receiver</th>
              <th>Signal</th>
              <th>Status</th>
              <th>Last Seen</th>
              <th>Updates</th>
              <th>First Seen</th>
            </tr>
          </thead>
          <tbody id='devicesTable'>
            <tr><td colspan='10' class='empty-state'>Loading devices...</td></tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>

  <!-- History View -->
  <div id='history-view' class='hidden'>
    <div class='controls'>
      <button class='nav-btn' onclick='refreshHistory()'>🔄 Refresh History</button>
      <button class='nav-btn warning' onclick='clearHistory()'>🗂️ Clear History</button>
    </div>
    
    <div class='section'>
      <h3>📊 System History</h3>
      <div id='systemHistoryContent'>Loading...</div>
    </div>
    
    <div class='section'>
      <h3>📱 Device History (All Time)</h3>
      <div style='overflow-x: auto;'>
        <table class='table'>
<thead>
  <tr>
    <th>Device ID</th>
    <th>Status</th>
    <th>Current Steps</th>
    <th>Peak Steps</th>
    <th>Battery Range</th>
    <th>Total Updates</th>
    <th>Disconnections</th>
    <th>Online Time</th>
    <th>First Seen</th>
    <th>Last Update</th>
    <th>Receiver/Offline Info</th>
  </tr>
</thead>
<tbody id='deviceHistoryTable'>
  <tr><td colspan='11' class='empty-state'>Loading device history...</td></tr>
</tbody>
        </table>
      </div>
    </div>
  </div>

  <!-- Connection Logs View -->
  <div id='logs-view' class='hidden'>
    <div class='controls'>
      <button class='nav-btn' onclick='refreshLogs()'>🔄 Refresh Logs</button>
    </div>
    <div class='section'>
      <h3>📋 Connection Logs</h3>
      <div id='connectionLogs'>Loading...</div>
    </div>
  </div>

  <!-- Receiver Details View -->
  <div id='receiver-details-view' class='hidden'>
    <div class='controls'>
      <button class='nav-btn' onclick='showView("receivers")'>← Back to Receivers</button>
    </div>
    <div class='section'>
      <h3 id='receiverDetailsTitle'>Receiver Details</h3>
      <div id='receiverDetailsContent'>Loading...</div>
    </div>
  </div>
</div>

<script>
let currentView = 'overview';

function showView(view) {
  // Hide all views
  document.querySelectorAll('[id$="-view"]').forEach(el => el.classList.add('hidden'));
  
  // Show selected view
  document.getElementById(view + '-view').classList.remove('hidden');
  
  // Update nav buttons
  document.querySelectorAll('.nav-btn').forEach(btn => btn.classList.remove('active'));
  if (event && event.target) event.target.classList.add('active');
  
  currentView = view;
  loadViewData(view);
}

function loadViewData(view) {
  switch(view) {
    case 'overview':
      updateOverview();
      break;
    case 'receivers':
      updateReceiversTable();
      break;
    case 'devices':
      updateDevicesTable();
      break;
    case 'history':
      updateHistory();
      break;
    case 'logs':
      updateLogs();
      break;
  }
}

function formatNumber(num) { 
  return num.toLocaleString(); 
}

function formatTime(seconds) {
  if (seconds < 60) return seconds + 's';
  if (seconds < 3600) return Math.floor(seconds/60) + 'm';
  if (seconds < 86400) return Math.floor(seconds/3600) + 'h';
  return Math.floor(seconds/86400) + 'd';
}

function updateOverview() {
  // Update stats
  fetch('/api/dashboard-data')
    .then(r => r.json())
    .then(data => {
      document.getElementById('totalSteps').textContent = formatNumber(data.totalSteps);
      document.getElementById('totalDevices').textContent = data.totalDevices;
      document.getElementById('activeReceivers').textContent = data.activeReceivers;
      document.getElementById('totalRequests').textContent = formatNumber(data.totalRequests);
      document.getElementById('uptime').textContent = formatTime(data.uptime);
    })
    .catch(() => console.error('Failed to update dashboard data'));
    
  // Update receivers overview - only show active receivers
  fetch('/api/receivers')
    .then(r => r.json())
    .then(data => {
      let html = '';
      
      // Filter to only show online receivers
      const activeReceivers = data.receivers.filter(receiver => receiver.status === 'Online');
      
      if (activeReceivers.length === 0) {
        html = '<div class="empty-state">No receivers currently online</div>';
      } else {
        activeReceivers.slice(0, 5).forEach(receiver => {
          html += '<div style="margin:1rem 0; padding:1.2rem; background:rgba(15,23,42,0.7); border-radius:8px; border-left:4px solid #f59e0b;">';
          html += '<div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:0.5rem;">';
          html += '<h4 style="color:#f8fafc;"><span class="receiver-link" onclick="showReceiverDetails(\\''+receiver.name+'\\')">📡 Receiver ' + receiver.name + '</span></h4>';
          html += '<span class="status-online">Online</span>';
          html += '</div>';
          html += '<div style="color:#94a3b8; font-size:0.9rem;">';
          html += receiver.deviceCount + ' devices • ' + formatNumber(receiver.totalSteps) + ' steps • Last seen ' + receiver.lastSeen + 's ago';
          html += '</div>';
          html += '</div>';
        });
        if (activeReceivers.length > 5) {
          html += '<div style="text-align:center; margin-top:1rem;"><button class="nav-btn" onclick="showView(\\'receivers\\')">View All ' + activeReceivers.length + ' Active Receivers</button></div>';
        }
      }
      document.getElementById('receiversOverview').innerHTML = html;
    });
    
  // Update top devices - only show active devices
  fetch('/api/devices')
    .then(r => r.json())
    .then(data => {
      let html = '';
      
      // Filter to only show online devices
      const activeDevices = data.devices.filter(device => device.status === 'Online');
      
      if (activeDevices.length === 0) {
        html = '<div class="empty-state">No devices currently active</div>';
      } else {
        html += '<div style="overflow-x:auto;"><table class="table">';
        html += '<thead><tr><th>Rank</th><th>Device ID</th><th>Steps</th><th>Battery</th><th>Receiver</th><th>Status</th></tr></thead><tbody>';
        
        activeDevices.slice(0, 10).forEach((device, index) => {
          html += '<tr>';
          html += '<td><strong style="color:#f59e0b;">#' + (index + 1) + '</strong></td>';
          html += '<td>📱 ' + device.name + '</td>';
          html += '<td><strong style="color:#10b981;">' + formatNumber(device.stepCount) + '</strong></td>';
          html += '<td>' + device.batteryLevel + '%</td>';
          html += '<td><span class="receiver-link" onclick="showReceiverDetails(\\''+device.receiverName+'\\')">📡 ' + device.receiverName + '</span></td>';
          html += '<td><span class="status-online">Online</span></td>';
          html += '</tr>';
        });
        
        html += '</tbody></table></div>';
if (activeDevices.length > 10) {
         html += '<div style="text-align:center; margin-top:1rem;"><button class="nav-btn" onclick="showView(\\'devices\\')">View All ' + activeDevices.length + ' Active Devices</button></div>';
       }
     }
     document.getElementById('topDevices').innerHTML = html;
   });
}

function updateReceiversTable() {
 fetch('/api/receivers')
   .then(r => r.json())
   .then(data => {
     let html = '';
     if (data.receivers.length === 0) {
       html = '<tr><td colspan="7" class="empty-state">No receivers currently connected</td></tr>';
     } else {
       data.receivers.forEach(receiver => {
         html += '<tr>';
         html += '<td><span class="receiver-link" onclick="showReceiverDetails(\\''+receiver.name+'\\')">📡 ' + receiver.name + '</span></td>';
         html += '<td><strong>' + receiver.deviceCount + '</strong></td>';
         html += '<td><strong style="color:#10b981;">' + formatNumber(receiver.totalSteps) + '</strong></td>';
         html += '<td><span class="status-' + receiver.status.toLowerCase() + '">' + receiver.status + '</span></td>';
         html += '<td>' + receiver.lastSeen + 's ago</td>';
         html += '<td style="font-size:0.8rem;">' + receiver.firstSeen + '</td>';
         html += '<td><button class="nav-btn" onclick="showReceiverDetails(\\''+receiver.name+'\\')">View Details</button></td>';
         html += '</tr>';
       });
     }
     document.getElementById('receiversTable').innerHTML = html;
   })
   .catch(() => {
     document.getElementById('receiversTable').innerHTML = '<tr><td colspan="7" class="empty-state" style="color:#ef4444;">Failed to load receivers</td></tr>';
   });
}

function updateDevicesTable() {
 fetch('/api/devices')
   .then(r => r.json())
   .then(data => {
     let html = '';
     if (data.devices.length === 0) {
       html = '<tr><td colspan="10" class="empty-state">No devices found</td></tr>';
     } else {
       data.devices.forEach(device => {
         html += '<tr>';
         html += '<td>📱 ' + device.name + '</td>';
         html += '<td><strong style="color:#10b981;">' + formatNumber(device.stepCount) + '</strong></td>';
         html += '<td style="color:#f59e0b;">' + formatNumber(device.peakSteps) + '</td>';
         html += '<td>' + device.batteryLevel + '%</td>';
         html += '<td><span class="receiver-link" onclick="showReceiverDetails(\\''+device.receiverName+'\\')">📡 ' + device.receiverName + '</span></td>';
         html += '<td>' + (device.signalStrength || 'N/A') + (device.signalStrength ? ' dBm' : '') + '</td>';
         html += '<td><span class="status-' + device.status.toLowerCase() + '">' + device.status + '</span></td>';
         html += '<td>' + device.lastSeen + 's ago' + (device.offlineTime ? ' (offline: ' + device.offlineTime + 's)' : '') + '</td>';
         html += '<td>' + device.totalUpdates + '</td>';
         html += '<td style="font-size:0.8rem;">' + device.firstSeen + '</td>';
         html += '</tr>';
       });
     }
     document.getElementById('devicesTable').innerHTML = html;
   })
   .catch(() => {
     document.getElementById('devicesTable').innerHTML = '<tr><td colspan="10" class="empty-state" style="color:#ef4444;">Failed to load devices</td></tr>';
   });
}


function updateHistory() {
  fetch('/api/history')
    .then(r => r.json())
    .then(data => {
      // Update system history with dropdown
      let systemHtml = '';
      if (data.systemHistory.length === 0) {
        systemHtml = '<div class="empty-state">No system history recorded yet</div>';
      } else {
        // Add dropdown control
        systemHtml += '<div style="margin-bottom: 1rem;">';
        systemHtml += '<select id="historyLimit" onchange="filterSystemHistory()" style="padding: 0.5rem; background: #334155; color: #e6edf3; border: 1px solid #475569; border-radius: 4px;">';
        systemHtml += '<option value="10">Show Recent 10 Records</option>';
        systemHtml += '<option value="all">Show All ' + data.systemHistory.length + ' Records</option>';
        systemHtml += '</select>';
        systemHtml += '</div>';
        
        systemHtml += '<div id="systemHistoryTable">';
        systemHtml += generateSystemHistoryTable(data.systemHistory, 10); // Show 10 by default
        systemHtml += '</div>';
      }
      document.getElementById('systemHistoryContent').innerHTML = systemHtml;
      
      // Store full history data globally for filtering
      window.fullSystemHistory = data.systemHistory;
      
      // Update device history (keep existing code)
      let deviceHtml = '';
      if (data.devices.length === 0) {
        deviceHtml = '<tr><td colspan="12" class="empty-state">No device history available</td></tr>';
      } else {
        data.devices.forEach(device => {
          deviceHtml += '<tr>';
          deviceHtml += '<td>📱 ' + device.name + '</td>';
          deviceHtml += '<td><span class="status-' + device.status.toLowerCase() + '">' + device.status + '</span></td>';
          deviceHtml += '<td><strong style="color:#10b981;">' + formatNumber(device.currentSteps) + '</strong></td>';
          deviceHtml += '<td style="color:#f59e0b;">' + formatNumber(device.peakSteps) + '</td>';
          deviceHtml += '<td>' + device.minBattery + '% - ' + device.maxBattery + '%</td>';
          deviceHtml += '<td>' + device.totalUpdates + '</td>';
          deviceHtml += '<td>' + device.disconnectionCount + '</td>';
          deviceHtml += '<td>' + formatTime(device.totalOnlineTime) + '</td>';
          deviceHtml += '<td style="font-size:0.8rem;">' + device.firstSeen + '</td>';
          deviceHtml += '<td style="font-size:0.8rem;">' + device.lastUpdate + '</td>';
          
          if (device.status === 'Offline') {
            deviceHtml += '<td style="font-size:0.8rem;"><span style="color:#ef4444;">Offline: ' + formatTime(device.offlineTime) + '</span><br>';
            deviceHtml += '<span style="color:#94a3b8;">Since: ' + device.offlineSince + '</span></td>';
          } else {
            deviceHtml += '<td><span class="receiver-link" onclick="showReceiverDetails(\\''+device.receiverName+'\\')">📡 ' + device.receiverName + '</span></td>';
          }
          
          deviceHtml += '</tr>';
        });
      }
      document.getElementById('deviceHistoryTable').innerHTML = deviceHtml;
    })
    .catch(() => {
      document.getElementById('systemHistoryContent').innerHTML = '<div class="empty-state" style="color:#ef4444;">Failed to load system history</div>';
      document.getElementById('deviceHistoryTable').innerHTML = '<tr><td colspan="12" class="empty-state" style="color:#ef4444;">Failed to load device history</td></tr>';
    });
}

function generateSystemHistoryTable(historyData, limit) {
  const displayData = limit === 'all' ? historyData : historyData.slice(0, limit);
  
  let html = '<div style="overflow-x:auto;"><table class="table">';
  html += '<thead><tr><th>Date & Time</th><th>Total Steps</th><th>Active Devices</th><th>Active Receivers</th><th>Offline Devices</th><th>Offline Device Details</th><th>Total Requests</th><th>Uptime</th></tr></thead><tbody>';
  
  displayData.forEach(record => {
    html += '<tr>';
    html += '<td style="font-size:0.9rem;">' + record.dateTime + '</td>';
    html += '<td><strong style="color:#10b981;">' + formatNumber(record.totalSteps) + '</strong></td>';
    html += '<td><strong style="color:#60a5fa;">' + record.activeDevices + '</strong></td>';
    html += '<td><strong style="color:#f59e0b;">' + record.activeReceivers + '</strong></td>';
    html += '<td><strong style="color:#ef4444;">' + record.offlineDevices + '</strong></td>';
    
    // Offline device details
    let offlineDetailsHtml = '';
    if (record.offlineDeviceList && record.offlineDeviceList.length > 0) {
      offlineDetailsHtml = '<div style="max-width:300px; max-height:100px; overflow-y:auto; font-size:0.8rem;">';
      record.offlineDeviceList.forEach(offlineDevice => {
        offlineDetailsHtml += '<div style="margin:2px 0; padding:3px; background:rgba(239,68,68,0.1); border-radius:3px;">';
        offlineDetailsHtml += '<strong>📱 ' + offlineDevice.name + '</strong><br>';
        offlineDetailsHtml += '<span style="color:#94a3b8;">Steps: ' + formatNumber(offlineDevice.lastSteps) + ' | Battery: ' + offlineDevice.lastBattery + '%</span><br>';
        offlineDetailsHtml += '<span style="color:#ef4444; font-size:0.7rem;">Offline: ' + formatTime(offlineDevice.offlineDuration) + '</span>';
        offlineDetailsHtml += '</div>';
      });
      offlineDetailsHtml += '</div>';
    } else {
      offlineDetailsHtml = '<span style="color:#94a3b8; font-style:italic;">No offline devices</span>';
    }
    
    html += '<td>' + offlineDetailsHtml + '</td>';
    html += '<td>' + formatNumber(record.totalRequests) + '</td>';
    html += '<td>' + formatTime(record.uptime) + '</td>';
    html += '</tr>';
  });
  
  html += '</tbody></table></div>';
  return html;
}

function filterSystemHistory() {
  const limit = document.getElementById('historyLimit').value;
  const limitValue = limit === 'all' ? 'all' : parseInt(limit);
  
  if (window.fullSystemHistory) {
    document.getElementById('systemHistoryTable').innerHTML = 
      generateSystemHistoryTable(window.fullSystemHistory, limitValue);
  }
}

function updateLogs() {
 fetch('/api/logs')
   .then(r => r.json())
   .then(data => {
     let html = '';
     if (data.logs.length === 0) {
       html = '<div class="empty-state">No connection logs yet</div>';
     } else {
       data.logs.forEach(log => {
         html += '<div class="log-entry ' + log.type + '">';
         html += '<div class="log-time">' + log.timestamp + '</div>';
         html += '<div class="log-message">' + log.message + '</div>';
         html += '</div>';
       });
     }
     document.getElementById('connectionLogs').innerHTML = html;
   })
   .catch(() => {
     document.getElementById('connectionLogs').innerHTML = '<div class="empty-state" style="color:#ef4444;">Failed to load logs</div>';
   });
}

function showReceiverDetails(receiverId) {
 fetch('/api/receiver/' + receiverId)
   .then(r => r.json())
   .then(data => {
     document.getElementById('receiverDetailsTitle').textContent = '📡 Receiver ' + receiverId + ' Details';
     
     let html = '<div class="stats-grid" style="margin-bottom:2rem;">';
     html += '<div class="stat-card"><div class="stat-value">' + data.deviceCount + '</div><div class="stat-label">Connected Devices</div></div>';
     html += '<div class="stat-card"><div class="stat-value">' + formatNumber(data.totalSteps) + '</div><div class="stat-label">Total Steps</div></div>';
     html += '<div class="stat-card"><div class="stat-value">' + data.lastSeen + 's</div><div class="stat-label">Last Seen</div></div>';
     html += '<div class="stat-card"><div class="stat-value" style="font-size:1rem;">' + data.firstSeen + '</div><div class="stat-label">First Connected</div></div>';
     html += '</div>';
     
     html += '<h4 style="margin-bottom:1rem; color:#f8fafc;">Connected Devices:</h4>';
     
     if (data.devices.length === 0) {
       html += '<div class="empty-state">No devices currently connected to this receiver</div>';
     } else {
       html += '<div style="overflow-x:auto;"><table class="table">';
       html += '<thead><tr><th>Device ID</th><th>Steps</th><th>Peak</th><th>Battery</th><th>Signal</th><th>Status</th><th>Last Seen</th><th>Updates</th></tr></thead>';
       html += '<tbody>';
       
       data.devices.forEach(device => {
         html += '<tr>';
         html += '<td>📱 ' + device.name + '</td>';
         html += '<td><strong style="color:#10b981;">' + formatNumber(device.stepCount) + '</strong></td>';
         html += '<td style="color:#f59e0b;">' + formatNumber(device.peakSteps) + '</td>';
         html += '<td>' + device.batteryLevel + '%</td>';
         html += '<td>' + device.signalStrength + ' dBm</td>';
         html += '<td><span class="status-' + device.status.toLowerCase() + '">' + device.status + '</span></td>';
         html += '<td>' + device.lastSeen + 's ago</td>';
         html += '<td>' + device.totalUpdates + '</td>';
         html += '</tr>';
       });
       
       html += '</tbody></table></div>';
     }
     
     document.getElementById('receiverDetailsContent').innerHTML = html;
     showView('receiver-details');
   })
   .catch(err => {
     document.getElementById('receiverDetailsContent').innerHTML = '<div class="empty-state" style="color:#ef4444;">Error loading receiver details: ' + err.message + '</div>';
     showView('receiver-details');
   });
}

function resetSystem() {
 if (confirm('⚠️ Reset ALL data including logs and device history?\\n\\nThis will clear everything and start fresh!\\n\\nThis action cannot be undone!')) {
   fetch('/api/reset', {method: 'POST'})
     .then(() => {
       alert('✅ System completely reset! All data cleared - ready for fresh device connections');
       // Force immediate refresh of current view
       setTimeout(() => {
         loadViewData(currentView);
       }, 500);
     })
     .catch(() => {
       alert('❌ Reset failed');
     });
 }
}

function clearHistory() {
 if (confirm('🗂️ Clear all historical data?\\n\\nThis will clear:\\n- System history records\\n- Offline device data\\n- Device connection history\\n\\nCurrent online devices will remain connected.\\n\\nProceed?')) {
   fetch('/api/clear-history', {method: 'POST'})
     .then(() => {
       alert('✅ History cleared successfully!');
       // Force immediate refresh if on history view
       if (currentView === 'history') {
         setTimeout(() => {
           updateHistory();
         }, 500);
       }
     })
     .catch(() => {
       alert('❌ Clear history failed');
     });
 }
}

function refreshHistory() {
 updateHistory();
}

function refreshLogs() {
 updateLogs();
}

// Auto-refresh every 3 seconds (except on receiver details page)
setInterval(() => {
 if (currentView !== 'receiver-details') {
   loadViewData(currentView);
 }
}, 3000);

// Initial load
updateOverview();
</script>
</body></html>
 `);
});


// Graceful shutdown handlers - ADD these before app.listen()
process.on('SIGINT', async () => {
  console.log('\n🔄 Received SIGINT, saving data before shutdown...');
  await saveHistoryData();
  console.log('✅ Data saved successfully');
  process.exit(0);
});

process.on('SIGTERM', async () => {
  console.log('\n🔄 Received SIGTERM, saving data before shutdown...');
  await saveHistoryData();
  console.log('✅ Data saved successfully');
  process.exit(0);
});

// Save data every 5 minutes automatically
setInterval(async () => {
  await saveHistoryData();
}, 300000); // 5 minutes

// ============================================================================
// SERVER STARTUP
// ============================================================================

// Cleanup and statistics intervals
setInterval(cleanupOldData, 30000);     // Every 30 seconds
setInterval(updateStatistics, 5000);    // Every 5 seconds
setInterval(recordSystemHistory, 60000); // Every 1 minute

// Start server
app.listen(PORT, '0.0.0.0', async () => {
  const localIP = getLocalIP();
  const pcName = getPCName();
  
  // Load historical data first
  console.log('📚 Loading historical data...');
  await loadHistoryData();
  
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
  console.log(`💾 Data persistence enabled - history saved to ./data/`);
  console.log('='.repeat(60));
  console.log('✅ Debug route registered at /debug');
  addLog('system', `Server started on ${localIP}:${PORT} with data persistence`);
});

module.exports = app;