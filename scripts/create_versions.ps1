# Create all version files based on your structure
$versionFiles = @{
    "firmware\versions.json" = @"
{
  "transmitter": {
    "current_version": "V3",
    "latest_path": "transmitter/latest/Transmitter_V3.ino.bin",
    "versioned_path": "transmitter/Transmitter_V3/Transmitter_V3.ino.bin"
  },
  "receiver": {
    "current_version": "V3", 
    "latest_path": "Receiver/latest/Receiver_V3.ino.bin",
    "versioned_path": "Receiver/Receiver_V3/Receiver_V3.ino.bin"
  },
  "server": {
    "current_version": "v3",
    "latest_path": "server/latest/Server_v3.ino.bin", 
    "versioned_path": "server/Server_v3/Server_v3.ino.bin"
  }
}
"@
    "firmware\transmitter\versions.json" = @"
{
  "latest_version": "V3",
  "available_versions": ["V3"],
  "latest_binary": "latest/Transmitter_V3.ino.bin"
}
"@
}

foreach ($file in $versionFiles.GetEnumerator()) {
    $file.Value | Out-File -FilePath $file.Key -Encoding UTF8
    Write-Host "Created: $($file.Key)"
}