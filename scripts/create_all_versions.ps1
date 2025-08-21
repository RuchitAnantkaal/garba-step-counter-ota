# PowerShell Script to Create All Version Files
# Run this from: D:\Projects\Garba\Garba_Github\

Write-Host "Creating all version files..." -ForegroundColor Green

# 1. Root firmware versions.json
$rootVersions = @"
{
  "system_info": {
    "project_name": "Garba Step Counter",
    "last_updated": "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "total_devices": 4050
  },
  "transmitter": {
    "current_version": "V3",
    "device_count": 4000,
    "latest_binary": "transmitter/latest/build/esp8266.esp8266.generic/Transmitter_V3.ino.bin",
    "stable_binary": "transmitter/Transmitter_V3/build/esp8266.esp8266.generic/Transmitter_V3.ino.bin",
    "hardware": "ESP8266"
  },
  "receiver": {
    "current_version": "V3",
    "device_count": 50,
    "latest_binary": "Receiver/latest/build/esp8266.esp8266.generic/Receiver_V3.ino.bin",
    "stable_binary": "Receiver/Receiver_V3/build/esp8266.esp8266.generic/Receiver_V3.ino.bin",
    "hardware": "ESP8266"
  },
  "server": {
    "current_version": "v3",
    "device_count": 1,
    "latest_binary": "server/latest/build/esp32.esp32.esp32/Server_v3.ino.bin",
    "stable_binary": "server/Server_v3/build/esp32.esp32.esp32/Server_v3.ino.bin",
    "hardware": "ESP32"
  }
}
"@

# 2. Transmitter root versions.json
$transmitterVersions = @"
{
  "device_type": "transmitter",
  "hardware": "ESP8266",
  "current_version": "V3",
  "available_versions": {
    "V3": {
      "latest": "latest/build/esp8266.esp8266.generic/Transmitter_V3.ino.bin",
      "stable": "Transmitter_V3/build/esp8266.esp8266.generic/Transmitter_V3.ino.bin",
      "source": "latest/Transmitter_V3.ino"
    }
  },
  "ota_enabled": true,
  "update_interval_hours": 6
}
"@

# 3. Transmitter latest versions.json
$transmitterLatestVersions = @"
{
  "version": "V3",
  "device_type": "transmitter",
  "hardware_target": "esp8266.esp8266.generic",
  "files": {
    "source": "Transmitter_V3.ino",
    "binary": "build/esp8266.esp8266.generic/Transmitter_V3.ino.bin",
    "elf": "build/esp8266.esp8266.generic/Transmitter_V3.ino.elf",
    "map": "build/esp8266.esp8266.generic/Transmitter_V3.ino.map"
  },
  "features": [
    "ESP-NOW broadcast mode",
    "Power efficient transmission",
    "Step detection algorithm",
    "Debug mode support",
    "OTA update capability"
  ],
  "build_info": {
    "compiled": "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "arduino_core": "ESP8266",
    "release_type": "development"
  }
}
"@

# 4. Transmitter V3 versions.json
$transmitterV3Versions = @"
{
  "version": "V3",
  "device_type": "transmitter",
  "hardware_target": "esp8266.esp8266.generic",
  "files": {
    "source": "Transmitter_V3.ino",
    "binary": "build/esp8266.esp8266.generic/Transmitter_V3.ino.bin",
    "elf": "build/esp8266.esp8266.generic/Transmitter_V3.ino.elf",
    "map": "build/esp8266.esp8266.generic/Transmitter_V3.ino.map"
  },
  "changelog": [
    "V3: Added ESP-NOW broadcast mode for roaming support",
    "V3: Improved power efficiency with WiFi sleep modes",
    "V3: Enhanced step detection accuracy",
    "V3: Added OTA update support"
  ],
  "build_info": {
    "compiled": "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "arduino_core": "ESP8266", 
    "release_type": "stable"
  }
}
"@

# 5. Receiver root versions.json
$receiverVersions = @"
{
  "device_type": "receiver",
  "hardware": "ESP8266",
  "current_version": "V3",
  "available_versions": {
    "V3": {
      "latest": "latest/build/esp8266.esp8266.generic/Receiver_V3.ino.bin",
      "stable": "Receiver_V3/build/esp8266.esp8266.generic/Receiver_V3.ino.bin",
      "source": "latest/Receiver_V3.ino"
    }
  },
  "ota_enabled": true,
  "update_interval_hours": 6
}
"@

# 6. Receiver latest versions.json
$receiverLatestVersions = @"
{
  "version": "V3",
  "device_type": "receiver",
  "hardware_target": "esp8266.esp8266.generic",
  "files": {
    "source": "Receiver_V3.ino",
    "binary": "build/esp8266.esp8266.generic/Receiver_V3.ino.bin",
    "elf": "build/esp8266.esp8266.generic/Receiver_V3.ino.elf",
    "map": "build/esp8266.esp8266.generic/Receiver_V3.ino.map"
  },
  "features": [
    "ESP-NOW data collection",
    "HTTP data forwarding to server",
    "Signal strength tracking",
    "Duplicate data prevention",
    "OTA update capability"
  ],
  "build_info": {
    "compiled": "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "arduino_core": "ESP8266",
    "release_type": "development"
  }
}
"@

# 7. Receiver V3 versions.json
$receiverV3Versions = @"
{
  "version": "V3",
  "device_type": "receiver",
  "hardware_target": "esp8266.esp8266.generic",
  "files": {
    "source": "Receiver_V3.ino",
    "binary": "build/esp8266.esp8266.generic/Receiver_V3.ino.bin",
    "elf": "build/esp8266.esp8266.generic/Receiver_V3.ino.elf", 
    "map": "build/esp8266.esp8266.generic/Receiver_V3.ino.map"
  },
  "changelog": [
    "V3: Added ESP-NOW broadcast mode support",
    "V3: Improved data aggregation and forwarding",
    "V3: Enhanced signal strength tracking",
    "V3: Added OTA update support"
  ],
  "build_info": {
    "compiled": "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "arduino_core": "ESP8266",
    "release_type": "stable"
  }
}
"@

# 8. Server root versions.json
$serverVersions = @"
{
  "device_type": "server",
  "hardware": "ESP32",
  "current_version": "v3",
  "available_versions": {
    "v3": {
      "latest": "latest/build/esp32.esp32.esp32/Server_v3.ino.bin",
      "stable": "Server_v3/build/esp32.esp32.esp32/Server_v3.ino.bin",
      "source": "latest/Server_v3.ino"
    }
  },
  "ota_enabled": true,
  "serves_ota": true
}
"@

# 9. Server latest versions.json
$serverLatestVersions = @"
{
  "version": "v3",
  "device_type": "server",
  "hardware_target": "esp32.esp32.esp32",
  "files": {
    "source": "Server_v3.ino",
    "binary": "build/esp32.esp32.esp32/Server_v3.ino.bin",
    "bootloader": "build/esp32.esp32.esp32/Server_v3.ino.bootloader.bin",
    "partitions": "build/esp32.esp32.esp32/Server_v3.ino.partitions.bin",
    "merged": "build/esp32.esp32.esp32/Server_v3.ino.merged.bin",
    "elf": "build/esp32.esp32.esp32/Server_v3.ino.elf",
    "map": "build/esp32.esp32.esp32/Server_v3.ino.map"
  },
  "features": [
    "Navratri themed dashboard",
    "Debug console interface", 
    "Multi-receiver data aggregation",
    "OTA update server",
    "mDNS support",
    "Real-time step counting"
  ],
  "build_info": {
    "compiled": "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "arduino_core": "ESP32",
    "release_type": "development"
  }
}
"@

# 10. Server v3 versions.json
$serverV3Versions = @"
{
  "version": "v3",
  "device_type": "server",
  "hardware_target": "esp32.esp32.esp32",
  "files": {
    "source": "Server_v3.ino",
    "binary": "build/esp32.esp32.esp32/Server_v3.ino.bin",
    "bootloader": "build/esp32.esp32.esp32/Server_v3.ino.bootloader.bin",
    "partitions": "build/esp32.esp32.esp32/Server_v3.ino.partitions.bin",
    "merged": "build/esp32.esp32.esp32/Server_v3.ino.merged.bin",
    "elf": "build/esp32.esp32.esp32/Server_v3.ino.elf",
    "map": "build/esp32.esp32.esp32/Server_v3.ino.map"
  },
  "changelog": [
    "v3: Added OTA update server functionality",
    "v3: Enhanced dashboard with Navratri theme",
    "v3: Improved debug console interface",
    "v3: Better device management and tracking"
  ],
  "build_info": {
    "compiled": "$(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "arduino_core": "ESP32",
    "release_type": "stable"
  }
}
"@

# Create all files
$files = @{
    "firmware\versions.json" = $rootVersions
    "firmware\transmitter\versions.json" = $transmitterVersions
    "firmware\transmitter\latest\versions.json" = $transmitterLatestVersions
    "firmware\transmitter\Transmitter_V3\versions.json" = $transmitterV3Versions
    "firmware\Receiver\versions.json" = $receiverVersions
    "firmware\Receiver\latest\versions.json" = $receiverLatestVersions
    "firmware\Receiver\Receiver_V3\versions.json" = $receiverV3Versions
    "firmware\server\versions.json" = $serverVersions
    "firmware\server\latest\versions.json" = $serverLatestVersions
    "firmware\server\Server_v3\versions.json" = $serverV3Versions
}

foreach ($file in $files.GetEnumerator()) {
    try {
        $file.Value | Out-File -FilePath $file.Key -Encoding UTF8 -Force
        Write-Host "✅ Created: $($file.Key)" -ForegroundColor Green
    }
    catch {
        Write-Host "❌ Failed to create: $($file.Key)" -ForegroundColor Red
        Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    }
}

Write-Host "`n🎉 All version files created successfully!" -ForegroundColor Cyan
Write-Host "📁 Total files created: $($files.Count)" -ForegroundColor Yellow
Write-Host "`nNext steps:" -ForegroundColor Magenta
Write-Host "1. Review the version files" -ForegroundColor White
Write-Host "2. Set up OTA update system in your ESP32 server" -ForegroundColor White
Write-Host "3. Add OTA capability to transmitters and receivers" -ForegroundColor White