# HRM Fan Control - Setup and Usage Guide

## Overview
This system controls fan speed based on heart rate data from a Bluetooth HRM (Heart Rate Monitor) strap. It features:
- WiFi connectivity with AP mode for easy configuration
- Web-based dashboard for monitoring and control
- Real-time debugging console
- OTA firmware updates
- Configurable heart rate thresholds

## First Time Setup

### 1. Flash the Firmware
Upload the firmware to your ESP32 using PlatformIO:
```bash
pio run --target upload
```

### 2. Connect to WiFi
On first boot, the device will create a WiFi Access Point:
- **SSID:** `HRM_Fan_Setup`
- **Password:** None (open network)

1. Connect your phone/computer to the `HRM_Fan_Setup` WiFi network
2. Your device should automatically open the configuration portal (if not, navigate to http://192.168.4.1)
3. Select your home WiFi network and enter the password
4. Click "Save" - the device will restart and connect to your WiFi

### 3. Find the Device IP Address
After connecting to WiFi, check your router's DHCP client list or use a network scanner to find the device IP address. The device hostname is `hrm-fan-control`.

## Web Dashboard

Access the dashboard by entering the device IP address in your web browser (e.g., `http://192.168.1.100`).

### Dashboard Features

#### Status Cards
- **Heart Rate:** Current BPM from your HRM strap
- **Current Zone:** Active fan speed zone (0-3, or FORCE ON/OFF)
- **HRM Status:** Connection status with your heart rate monitor
- **Uptime:** How long the device has been running

#### Manual Control
- **Force ON:** Run fan at maximum speed regardless of heart rate
- **Force OFF:** Turn off fan completely
- **Normal Mode:** Resume automatic heart rate-based control

#### Heart Rate Thresholds
Configure the heart rate zones:
- **T_0 (Start):** Heart rate to start the fan (default: 110 bpm)
- **T_1 (Speed 2):** Heart rate to engage second speed (default: 150 bpm)
- **T_2 (Speed 3):** Heart rate to engage third/maximum speed (default: 160 bpm)

**Note:** The system includes hysteresis - zones decrease when HR drops 5 bpm below the threshold to prevent rapid switching.

#### Relay Status
Visual indication of which relays are currently active:
- **Relay 1:** Zone 1 (low speed)
- **Relay 2:** Zone 2 (medium speed)
- **Relay 3:** Zone 3 (high speed)

#### Debug Console
Real-time log of all system events:
- HRM connection/disconnection
- Heart rate readings
- Zone changes
- Threshold updates
- System events

## OTA (Over-The-Air) Updates

### Using Arduino IDE
1. Click "OTA Update" button in the web dashboard to see connection details
2. In Arduino IDE: Tools → Port → Select "hrm-fan-control at [IP]"
3. Upload normally with Ctrl+U

### Using PlatformIO
Add to `platformio.ini`:
```ini
upload_protocol = espota
upload_port = <device_ip>
upload_flags =
    --port=3232
    --auth=admin
```

Then upload with:
```bash
pio run --target upload
```

**Default OTA Password:** `admin` (change in code if needed)

## Troubleshooting

### Device Won't Connect to WiFi
1. Re-flash the firmware
2. To force AP mode again, uncomment this line in `main.cpp` setup():
   ```cpp
   wifiManager.resetSettings();
   ```
3. Re-flash, configure WiFi, then comment it out again

### HRM Not Connecting
- Ensure your HRM strap is turned on and transmitting
- Check the debug console for scan results
- Supported: Polar H7/H10, Garmin HRM straps, and any HRM broadcasting standard Heart Rate Service (0x180D)

### Relays Not Switching Properly
- Monitor the debug console to see zone changes
- Check the relay status indicators in the web dashboard
- Verify GPIO connections: GPIO 25 (Zone 1), GPIO 26 (Zone 2), GPIO 27 (Zone 3)
- Confirm relay type setting in code (RELAY_NO = true for Normally Open)

### Can't Access Web Dashboard
- Verify device is connected to WiFi (LED on GPIO 19 will be on when HRM is connected)
- Check router for device IP address
- Try mDNS: `http://hrm-fan-control.local` (may not work on all networks)

## Technical Details

### Zone Logic
- **Zone 0:** All relays OFF (HR ≤ T_0 - 5)
- **Zone 1:** Relay 1 ON (HR > T_0)
- **Zone 2:** Relay 2 ON (HR ≥ T_1)
- **Zone 3:** Relay 3 ON (HR ≥ T_2)

Descending transitions include 5 bpm hysteresis to prevent rapid cycling.

### GPIO Pin Assignments
- GPIO 25: Relay 1 (Zone 1 - Low speed)
- GPIO 26: Relay 2 (Zone 2 - Medium speed)
- GPIO 27: Relay 3 (Zone 3 - High speed)
- GPIO 19: LED indicator (ON when HRM connected)

### Network Services
- **Web Server:** Port 80
- **WebSocket:** Port 80, path `/ws`
- **OTA Updates:** Port 3232

## Safety Notes

⚠️ **IMPORTANT:**
- This device controls high-voltage relays (230V AC in some cases)
- Ensure proper electrical isolation
- The ESP32 and relay board should NOT share a common ground with mains voltage
- **DO NOT connect serial USB cable while relays are powered by mains voltage**
- Use the web interface for all debugging when the system is live

## Customization

### Changing Zone Behavior
Edit the relay activation logic in the `loop()` function in `main.cpp`.

### Adding More Zones
1. Increase `NUM_RELAYS` definition
2. Add GPIO pins to `relayGPIOs[]` array
3. Add threshold variables (T_3, T_4, etc.)
4. Update logic in `loop()` function
5. Update web interface HTML

### Custom Web Interface
Edit the `index_html[]` string in `main.cpp` to customize the dashboard appearance and functionality.

---

YouTube Video: https://www.youtube.com/watch?v=6tJlJQgutkI 

**Original Authors:** Andrew Grabbs, Petr Divis  
**Modified:** 2026 - Added WiFi, Web Dashboard, OTA Updates
