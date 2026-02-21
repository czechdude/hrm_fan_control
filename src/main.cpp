/**
 * BLE Heart Rate Monitor Fan Control with WiFi Web Interface
 * Reads BLE HRM (Forerunner, Fenix, etc.) and controls fan relays based on heart rate zones
 * Features: WiFi AP mode, Web dashboard, real-time debugging
 * author Andrew Grabbs, Petr Divis
 */

#include "Arduino.h"
#include "BLEDevice.h"
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <ArduinoJson.h>
#include <vector>
#include <sstream>

// Configuration
Preferences preferences;
#define RELAY_NO true  // Normally Open relay
#define NUM_RELAYS 3

// Zone definitions
#define Z_ON -1
#define Z_OFF -2
#define Z_0 0
#define Z_1 1
#define Z_2 2
#define Z_3 3

// Heart Rate Thresholds (modifiable via web interface)
unsigned int T_0 = 110; // start fan
unsigned int T_1 = 150; // second speed
unsigned int T_2 = 160; // third speed

// Control mode
enum ControlMode { MODE_AUTOMATIC, MODE_MANUAL };
ControlMode controlMode = MODE_AUTOMATIC;
int manualZone = 0; // Zone to use in manual mode (0-3)

// GPIO assignments
uint8_t relayGPIOs[NUM_RELAYS] = {25, 26, 27};
uint8_t ledPin = 19;

// BLE HRM Service (Heart Rate Monitor)
static BLEUUID serviceUUID("0000180d-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID(BLEUUID((uint16_t)0x2A37));

// Found BLE devices during scan
struct FoundDevice {
  String name;
  String address;
  BLEAdvertisedDevice* device;
};
std::vector<FoundDevice> foundDevices;
String preferredAddress = ""; // Saved preferred HRM device address

// BLE state
static short prev = 0;
static uint8_t hr = 0;
static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = true;
static boolean isScanning = false;  // Async BLE scan in progress
static boolean justConnected = false; // Flag to initialize zone on first HR reading
static BLERemoteCharacteristic *pRemoteCharacteristic;
static BLEAdvertisedDevice *myDevice;
static BLEScan *pBLEScan;

// Web server and WiFi
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dns;

// Debug log buffer (circular buffer for web console)
#define LOG_BUFFER_SIZE 100
String logBuffer[LOG_BUFFER_SIZE];
int logIndex = 0;

// Forward declarations
void addLog(String message);
void broadcastStatus();
void setZoneForHR(uint8_t heartRate); // Initialize zone based on current HR
void scanComplete(BLEScanResults results); // Async BLE scan callback
void broadcastDeviceList();              // Send found BLE devices to web UI

// Function to add log entry
void addLog(String message) {
  logBuffer[logIndex] = String(millis()) + ": " + message;
  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
  
  // Send to WebSocket clients
  String logMsg = "{\"type\":\"log\",\"message\":\"" + message + "\"}";
  ws.textAll(logMsg);
}

// BLE notification callback - receives heart rate data
static void notifyCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
  hr = pData[1];
  String logMsg = "Heart Rate: " + String(hr) + " bpm";
  addLog(logMsg);
  broadcastStatus(); // Update web UI with new heart rate
  
  // If just connected and in automatic mode, initialize zone based on current HR
  if (justConnected && controlMode == MODE_AUTOMATIC) {
    justConnected = false;
    setZoneForHR(hr);
  }
}

// BLE client callbacks
class MyClientCallback : public BLEClientCallbacks
{
  void onConnect(BLEClient *pclient)
  {
    digitalWrite(ledPin, HIGH);
    justConnected = true;
    addLog("HRM Connected!");
  }

  void onDisconnect(BLEClient *pclient)
  {
    digitalWrite(ledPin, LOW);
    // Turn off all relays
    for (int i = 0; i < NUM_RELAYS; i++)
    {
      digitalWrite(relayGPIOs[i], HIGH);
    }
    prev = -1;
    hr = 0;
    connected = false;
    doScan = true;
    justConnected = false;
    addLog("HRM Disconnected - rescanning");
    broadcastStatus(); // Update UI with disconnected status
  }
};

// Connect to BLE HRM server
bool connectToServer()
{
  addLog("Connecting to HRM: " + String(myDevice->getAddress().toString().c_str()));

  BLEClient *pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(myDevice))
  {
    prev = -1;
    hr = 0;
    connected = false;
    doScan = true;
    addLog("Failed to connect to HRM");
    return false;
  }
  
  addLog("Connected to HRM server");

  BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr)
  {
    addLog("Failed to find HRM service");
    pClient->disconnect();
    return false;
  }

  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr)
  {
    addLog("Failed to find HRM characteristic");
    pClient->disconnect();
    return false;
  }
  
  if (pRemoteCharacteristic->canNotify())
  {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    const uint8_t onPacket[] = {0x01, 0x0};
    pRemoteCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t *)onPacket, 2, true);
    addLog("HRM notifications enabled");
  }
  
  connected = true;
  broadcastStatus(); // Update UI with connected status
  return true;
}
// BLE scan callback
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID))
    {
      String addr = String(advertisedDevice.getAddress().toString().c_str());
      String name = advertisedDevice.haveName() ? String(advertisedDevice.getName().c_str()) : "Unknown HRM";
      // Deduplicate
      for (auto& d : foundDevices) { if (d.address == addr) return; }
      foundDevices.push_back({name, addr, new BLEAdvertisedDevice(advertisedDevice)});
      addLog("Found HRM: " + name + " (" + addr + ")");
    }
  }
};

// WebSocket event handler
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    addLog("WebSocket client connected");
    // Send current status to new client
    String statusMsg = "{\"type\":\"status\",\"hr\":" + String(hr) + 
                       ",\"zone\":" + String(prev) + 
                       ",\"connected\":" + String(connected) +
                       ",\"T_0\":" + String(T_0) +
                       ",\"T_1\":" + String(T_1) +
                       ",\"T_2\":" + String(T_2) +
                       ",\"mode\":\"" + String(controlMode == MODE_AUTOMATIC ? "automatic" : "manual") + "\"" +
                       ",\"manualZone\":" + String(manualZone) +
                       ",\"ip\":\"" + WiFi.localIP().toString() +
                       "\",\"uptime\":" + String(millis() / 1000) + "}";
    client->text(statusMsg);
    
    // Send log history
    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
      int idx = (logIndex + i) % LOG_BUFFER_SIZE;
      if (logBuffer[idx].length() > 0) {
        String logMsg = "{\"type\":\"log\",\"message\":\"" + logBuffer[idx] + "\"}";
        client->text(logMsg);
      }
    }
  } else if (type == WS_EVT_DISCONNECT) {
    addLog("WebSocket client disconnected");
  } else if (type == WS_EVT_DATA) {
    // Handle commands from web interface
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;
      String msg = (char*)data;
      
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, msg);
      
      if (!error) {
        String cmd = doc["cmd"];
        
        if (cmd == "setMode") {
          String mode = doc["mode"];
          if (mode == "automatic") {
            controlMode = MODE_AUTOMATIC;
            preferences.begin("diyfan", false);
            preferences.putInt("mode", MODE_AUTOMATIC);
            preferences.end();
            addLog("Switched to AUTOMATIC mode");
          } else if (mode == "manual") {
            controlMode = MODE_MANUAL;
            preferences.begin("diyfan", false);
            preferences.putInt("mode", MODE_MANUAL);
            preferences.end();
            addLog("Switched to MANUAL mode");
          }
          broadcastStatus();
        } else if (cmd == "setManualZone") {
          int zone = doc["zone"];
          if (zone >= 0 && zone <= 3) {
            manualZone = zone;
            preferences.begin("diyfan", false);
            preferences.putInt("manualZone", manualZone);
            preferences.end();
            addLog("Manual zone set to: " + String(zone));
            broadcastStatus();
          }
        } else if (cmd == "restart") {
          addLog("Restarting ESP32...");
          delay(500);
          ESP.restart();
        } else if (cmd == "resetSettings") {
          addLog("Resetting all settings to defaults...");
          preferences.begin("diyfan", false);
          preferences.clear();
          preferences.end();
          delay(500);
          ESP.restart();
        } else if (cmd == "setThresholds") {
          T_0 = doc["T_0"];
          T_1 = doc["T_1"];
          T_2 = doc["T_2"];
          preferences.begin("diyfan", false);
          preferences.putUInt("T_0", T_0);
          preferences.putUInt("T_1", T_1);
          preferences.putUInt("T_2", T_2);
          preferences.end();
          addLog("Thresholds updated: T_0=" + String(T_0) + " T_1=" + String(T_1) + " T_2=" + String(T_2));
        } else if (cmd == "connectDevice") {
          String addr = doc["address"];
          for (auto& d : foundDevices) {
            if (d.address == addr) {
              myDevice = d.device;
              doConnect = true;
              preferredAddress = addr;
              preferences.begin("diyfan", false);
              preferences.putString("prefAddr", addr);
              preferences.end();
              addLog("Connecting to: " + d.name + " (" + addr + ")");
              break;
            }
          }
        } else if (cmd == "rescan") {
          if (!connected && !isScanning) {
            for (auto& d : foundDevices) delete d.device;
            foundDevices.clear();
            doScan = true;
            addLog("Manual rescan requested");
          }
        } else if (cmd == "forgetDevice") {
          preferredAddress = "";
          preferences.begin("diyfan", false);
          preferences.remove("prefAddr");
          preferences.end();
          addLog("Preferred HRM device forgotten");
        }
      }
    }
  }
}

// HTML for the web interface
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>HRM Fan Control</title>
    <style>
        body { 
            font-family: Arial, sans-serif; 
            margin: 20px; 
            background: #1a1a1a; 
            color: #fff; 
        }
        .container { 
            max-width: 1200px; 
            margin: 0 auto; 
        }
        .header { 
            background: #2d2d2d; 
            padding: 20px; 
            border-radius: 8px; 
            margin-bottom: 20px; 
        }
        .status-grid { 
            display: grid; 
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); 
            gap: 15px; 
            margin-bottom: 20px; 
        }
        .status-card { 
            background: #2d2d2d; 
            padding: 15px; 
            border-radius: 8px; 
            text-align: center; 
        }
        .status-value { 
            font-size: 2em; 
            font-weight: bold; 
            color: #4CAF50; 
        }
        .status-label { 
            color: #888; 
            margin-top: 5px; 
        }
        .controls { 
            background: #2d2d2d; 
            padding: 20px; 
            border-radius: 8px; 
            margin-bottom: 20px; 
        }
        .control-section { 
            margin-bottom: 20px; 
        }
        .control-section h3 { 
            margin-top: 0; 
            color: #4CAF50; 
        }
        button { 
            background: #4CAF50; 
            color: white; 
            border: none; 
            padding: 12px 24px; 
            border-radius: 4px; 
            cursor: pointer; 
            margin: 5px; 
            font-size: 14px; 
        }
        button:hover { 
            background: #45a049; 
        }
        button.danger { 
            background: #f44336; 
        }
        button.danger:hover { 
            background: #da190b; 
        }
        button.warning { 
            background: #ff9800; 
        }
        button.warning:hover { 
            background: #e68900; 
        }
        input[type="number"] { 
            background: #3d3d3d; 
            border: 1px solid #555; 
            color: #fff; 
            padding: 8px; 
            border-radius: 4px; 
            width: 80px; 
            margin: 5px; 
        }
        .console { 
            background: #000; 
            color: #0f0; 
            padding: 15px; 
            border-radius: 8px; 
            font-family: 'Courier New', monospace; 
            height: 400px; 
            overflow-y: auto; 
            font-size: 12px; 
        }
        .console-line { 
            margin: 2px 0; 
        }
        .zone-indicator { 
            display: inline-block; 
            width: 20px; 
            height: 20px; 
            border-radius: 50%; 
            margin-left: 10px; 
        }
        .relay-status { 
            display: grid; 
            grid-template-columns: repeat(3, 1fr); 
            gap: 10px; 
            margin-top: 10px; 
        }
        .relay { 
            background: #3d3d3d; 
            padding: 10px; 
            border-radius: 4px; 
            text-align: center; 
        }
        .relay.active { 
            background: #4CAF50; 
        }
        .device-btn { 
            display: block;
            width: 100%;
            text-align: left;
            margin: 4px 0;
            background: #3d3d3d;
            border: 1px solid #555;
        }
        .device-btn:hover { background: #4CAF50; }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>❤️ HRM Fan Control Dashboard</h1>
            <p>WiFi: <strong id="wifiStatus">Connected</strong> | IP: <strong id="ipAddress">Loading...</strong></p>
        </div>
        
        <div class="status-grid">
            <div class="status-card">
                <div class="status-value" id="heartRate">--</div>
                <div class="status-label">Heart Rate (bpm)</div>
            </div>
            <div class="status-card">
                <div class="status-value" id="zone">--</div>
                <div class="status-label">Current Zone</div>
            </div>
            <div class="status-card">
                <div class="status-value" id="hrmStatus">Disconnected</div>
                <div class="status-label">HRM Status</div>
            </div>
            <div class="status-card">
                <div class="status-value" id="uptime">0s</div>
                <div class="status-label">Uptime</div>
            </div>
        </div>

        <div class="controls">
            <div class="control-section">
                <h3>Control Mode</h3>
                <button onclick="setMode('automatic')" id="btnAuto">🤖 Automatic (by HR)</button>
                <button onclick="setMode('manual')" id="btnManual" class="warning">✋ Manual</button>
                <div id="modeStatus" style="margin-top:10px; font-size:14px; color:#888;"></div>
            </div>

            <div class="control-section" id="manualControls" style="display:none;">
                <h3>Manual Zone Selection</h3>
                <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; margin-top: 10px;">
                    <button onclick="setManualZone(0)" id="zoneBtn0">Zone 0 (OFF)</button>
                    <button onclick="setManualZone(1)" id="zoneBtn1">Zone 1 (Low)</button>
                    <button onclick="setManualZone(2)" id="zoneBtn2">Zone 2 (Med)</button>
                    <button onclick="setManualZone(3)" id="zoneBtn3">Zone 3 (High)</button>
                </div>
            </div>

            <div class="control-section" id="autoControls">
                <h3>Heart Rate Thresholds (Automatic Mode)</h3>
                <label>T_0 (Start): <input type="number" id="t0" value="110" min="60" max="200" oninput="thresholdsDirty=true"></label>
                <label>T_1 (Speed 2): <input type="number" id="t1" value="150" min="60" max="200" oninput="thresholdsDirty=true"></label>
                <label>T_2 (Speed 3): <input type="number" id="t2" value="160" min="60" max="200" oninput="thresholdsDirty=true"></label>
                <button onclick="updateThresholds()">Update Thresholds</button>
            </div>

            <div class="control-section">
                <h3>Relay Status</h3>
                <div class="relay-status">
                    <div class="relay" id="relay0">Relay 1 (Zone 1)</div>
                    <div class="relay" id="relay1">Relay 2 (Zone 2)</div>
                    <div class="relay" id="relay2">Relay 3 (Zone 3)</div>
                </div>
            </div>

            <div class="control-section">
                <h3>System</h3>
                <button onclick="resetSettings()" class="warning">Reset Settings</button>
                <button onclick="restartDevice()" class="danger">Restart Device</button>
                <button onclick="forgetDevice()" style="background:#555">Change HRM Device</button>
            </div>
        </div>

        <div class="controls" id="devicePickerSection" style="display:none;">
            <h3>Select HRM Device</h3>
            <div id="deviceList"></div>
            <button onclick="rescan()" style="background:#555;margin-top:8px">🔄 Rescan</button>
        </div>

        <div class="controls">
            <h3>Debug Console</h3>
            <div class="console" id="console"></div>
            <button onclick="clearConsole()">Clear Console</button>
        </div>
    </div>

    <script>
        let ws;
        let thresholdsDirty = false;
        
        function connect() {
            ws = new WebSocket('ws://' + window.location.hostname + '/ws');
            
            ws.onopen = function() {
                addConsoleLog('WebSocket connected');
            };
            
            ws.onclose = function() {
                addConsoleLog('WebSocket disconnected, reconnecting...');
                setTimeout(connect, 2000);
            };
            
            ws.onerror = function(error) {
                addConsoleLog('WebSocket error: ' + error);
            };
            
            ws.onmessage = function(event) {
                let data = JSON.parse(event.data);
                
                if (data.type === 'status') {
                    document.getElementById('heartRate').textContent = data.hr;
                    document.getElementById('zone').textContent = getZoneName(data.zone);
                    document.getElementById('hrmStatus').textContent = data.connected ? 'Connected' : 'Disconnected';
                    document.getElementById('hrmStatus').style.color = data.connected ? '#4CAF50' : '#f44336';
                    // Hide device picker when connected
                    if (data.connected) document.getElementById('devicePickerSection').style.display = 'none';
                    if (!thresholdsDirty) {
                        document.getElementById('t0').value = data.T_0;
                        document.getElementById('t1').value = data.T_1;
                        document.getElementById('t2').value = data.T_2;
                    }
                    if (data.ip) document.getElementById('ipAddress').textContent = data.ip;
                    if (data.uptime !== undefined) document.getElementById('uptime').textContent = formatUptime(data.uptime);
                    updateRelayDisplay(data.zone);
                    updateModeUI(data.mode, data.manualZone);
                } else if (data.type === 'deviceList') {
                    showDevicePicker(data.devices);
                } else if (data.type === 'log') {
                    addConsoleLog(data.message);
                }
            };
        }
        
        function getZoneName(zone) {
            switch(zone) {
                case 0: return 'Zone 0 (OFF)';
                case 1: return 'Zone 1 (Low)';
                case 2: return 'Zone 2 (Medium)';
                case 3: return 'Zone 3 (High)';
                default: return '--';
            }
        }
        
        function updateModeUI(mode, manualZone) {
            // Update mode buttons
            document.getElementById('btnAuto').style.background = mode === 'automatic' ? '#4CAF50' : '#555';
            document.getElementById('btnManual').style.background = mode === 'manual' ? '#ff9800' : '#555';
            
            // Show/hide controls
            document.getElementById('manualControls').style.display = mode === 'manual' ? 'block' : 'none';
            
            // Update status text
            document.getElementById('modeStatus').textContent = 
                mode === 'automatic' ? 'Mode: Automatic (HR-based)' : 'Mode: Manual Control';
            
            // Update manual zone button highlights
            if (mode === 'manual') {
                for (let i = 0; i <= 3; i++) {
                    let btn = document.getElementById('zoneBtn' + i);
                    btn.style.background = (i === manualZone) ? '#4CAF50' : '#555';
                }
            }
        }
        
        function setMode(mode) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'setMode', mode: mode}));
            }
        }
        
        function setManualZone(zone) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'setManualZone', zone: zone}));
            }
        }
        
        function updateRelayDisplay(zone) {
            document.getElementById('relay0').classList.remove('active');
            document.getElementById('relay1').classList.remove('active');
            document.getElementById('relay2').classList.remove('active');
            
            if (zone === 1) document.getElementById('relay0').classList.add('active');
            if (zone === 2) document.getElementById('relay1').classList.add('active');
            if (zone === 3) document.getElementById('relay2').classList.add('active');
        }
        
        function sendCommand(cmd) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: cmd}));
            }
        }
        
        function updateThresholds() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                let t0 = parseInt(document.getElementById('t0').value);
                let t1 = parseInt(document.getElementById('t1').value);
                let t2 = parseInt(document.getElementById('t2').value);
                ws.send(JSON.stringify({
                    cmd: 'setThresholds',
                    T_0: t0,
                    T_1: t1,
                    T_2: t2
                }));
                thresholdsDirty = false;
            }
        }
        
        function restartDevice() {
            if (confirm('Are you sure you want to restart the device?')) {
                sendCommand('restart');
            }
        }
        
        function resetSettings() {
            if (confirm('Reset all settings to defaults? This will restart the device.')) {
                if (ws && ws.readyState === WebSocket.OPEN) {
                    ws.send(JSON.stringify({cmd: 'resetSettings'}));
                }
            }
        }
        
        function addConsoleLog(msg) {
            let console = document.getElementById('console');
            let line = document.createElement('div');
            line.className = 'console-line';
            line.textContent = msg;
            console.appendChild(line);
            console.scrollTop = console.scrollHeight;
        }
        
        function clearConsole() {
            document.getElementById('console').innerHTML = '';
        }
        
        function formatUptime(seconds) {
            let h = Math.floor(seconds / 3600);
            let m = Math.floor((seconds % 3600) / 60);
            let s = seconds % 60;
            return (h > 0 ? h + 'h ' : '') + (m > 0 ? m + 'm ' : '') + s + 's';
        }
        
        function showDevicePicker(devices) {
            let section = document.getElementById('devicePickerSection');
            let list = document.getElementById('deviceList');
            list.innerHTML = '';
            if (devices.length === 0) { section.style.display = 'none'; return; }
            section.style.display = 'block';
            devices.forEach(d => {
                let btn = document.createElement('button');
                btn.className = 'device-btn';
                btn.textContent = '📡 ' + d.name + '  (' + d.address + ')';
                btn.onclick = () => connectDevice(d.address);
                list.appendChild(btn);
            });
        }
        
        function connectDevice(address) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'connectDevice', address: address}));
                document.getElementById('devicePickerSection').style.display = 'none';
            }
        }
        
        function rescan() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'rescan'}));
                document.getElementById('deviceList').innerHTML = '<p style="color:#888">Scanning...</p>';
            }
        }
        
        function forgetDevice() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'forgetDevice'}));
                ws.send(JSON.stringify({cmd: 'rescan'}));
            }
        }
        
        // Connect WebSocket
        connect();
        
        // Periodic status update request
        setInterval(function() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'getStatus'}));
            }
        }, 2000);
    </script>
</body>
</html>
)rawliteral";

void setup()
{
  Serial.begin(115200);
  Serial.println("\n\nStarting HRM Fan Control with WiFi...");
  
  // Load preferences
  preferences.begin("diyfan", true);
  T_0 = preferences.getUInt("T_0", 110);
  T_1 = preferences.getUInt("T_1", 150);
  T_2 = preferences.getUInt("T_2", 160);
  controlMode = (ControlMode)preferences.getInt("mode", MODE_AUTOMATIC);
  manualZone = preferences.getInt("manualZone", 0);
  preferredAddress = preferences.getString("prefAddr", "");
  preferences.end();
  if (preferredAddress.length() > 0)
    Serial.println("Preferred HRM: " + preferredAddress);
  
  Serial.printf("Loaded thresholds: T_0=%d, T_1=%d, T_2=%d\n", T_0, T_1, T_2);
  Serial.printf("Control mode: %s, Manual zone: %d\n", 
                controlMode == MODE_AUTOMATIC ? "AUTOMATIC" : "MANUAL", manualZone);
  
  // Initialize GPIO pins
  for (int i = 0; i < NUM_RELAYS; i++)
  {
    pinMode(relayGPIOs[i], OUTPUT);
    digitalWrite(relayGPIOs[i], RELAY_NO ? HIGH : LOW);
  }
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  
  // Initialize WiFi with WiFiManager (AP mode for configuration)
  AsyncWiFiManager wifiManager(&server, &dns);
  
  // Uncomment to reset WiFi settings for testing
  // wifiManager.resetSettings();
  
  wifiManager.setAPCallback([](AsyncWiFiManager *myWiFiManager) {
    Serial.println("Entered config mode");
    Serial.println("AP SSID: " + String(myWiFiManager->getConfigPortalSSID()));
    Serial.println("AP IP: 192.168.4.1");
    // Blink LED to indicate config mode
    for(int i=0; i<10; i++) {
      digitalWrite(ledPin, !digitalRead(ledPin));
      delay(100);
    }
  });
  
  // Automatically connect using saved credentials or start AP
  if (!wifiManager.autoConnect("HRM_Fan_Setup")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
  }
  
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Initialize BLE
  BLEDevice::init("HRM Fan Control");
  
  // Setup WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  
  // Web server routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
  });
  
  server.begin();
  Serial.println("Web server started");
  
  addLog("System initialized - WiFi IP: " + WiFi.localIP().toString());
  addLog("Thresholds: T_0=" + String(T_0) + " T_1=" + String(T_1) + " T_2=" + String(T_2));
}

void scanComplete(BLEScanResults results) {
  isScanning = false;
  if (connected) return;
  if (foundDevices.empty()) {
    doScan = true; // nothing found, rescan
    return;
  }
  // Auto-connect to preferred device if it was found
  if (preferredAddress.length() > 0) {
    for (auto& d : foundDevices) {
      if (d.address == preferredAddress) {
        myDevice = d.device;
        doConnect = true;
        return;
      }
    }
  }
  // Only one device found - auto-connect
  if (foundDevices.size() == 1) {
    myDevice = foundDevices[0].device;
    doConnect = true;
    return;
  }
  // Multiple devices - let user pick
  broadcastDeviceList();
}

void loop()
{
  // Cleanup WebSocket clients periodically
  static unsigned long lastCleanup = 0;
  if (millis() - lastCleanup > 10000) {
    ws.cleanupClients();
    lastCleanup = millis();
  }

  // BLE HRM connection management
  if (doConnect == true)
  {
    connectToServer();
    doConnect = false;
  }

  // Async BLE scan - starts scan and returns immediately, scanComplete() called when done
  if (!connected && doScan && !isScanning)
  {
    // Clear previous scan results
    for (auto& d : foundDevices) delete d.device;
    foundDevices.clear();
    isScanning = true;
    doScan = false;
    addLog("Scanning for HRM devices...");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setInterval(1349);
    pBLEScan->setWindow(449);
    pBLEScan->setActiveScan(true);
    pBLEScan->start(5, scanComplete, false); // Async - returns immediately!
  }

  // Control logic - runs every 1 second via millis, no blocking delay
  static unsigned long lastZoneCheck = 0;
  if (millis() - lastZoneCheck >= 1000) {
    lastZoneCheck = millis();

    if (controlMode == MODE_MANUAL) {
      // Manual mode - apply selected zone regardless of HR
      int targetZone = manualZone;
      if (prev != targetZone) {
        for (int i = 0; i < NUM_RELAYS; i++) {
          digitalWrite(relayGPIOs[i], HIGH);
        }
        if (targetZone == 1) {
          digitalWrite(relayGPIOs[0], LOW);
        } else if (targetZone == 2) {
          digitalWrite(relayGPIOs[1], LOW);
        } else if (targetZone == 3) {
          digitalWrite(relayGPIOs[2], LOW);
        }
        prev = targetZone;
        addLog("MANUAL: Zone " + String(targetZone));
        broadcastStatus();
      }
    }
    else if (controlMode == MODE_AUTOMATIC) {
      // Automatic mode - control based on heart rate
      // Descending transitions (hysteresis)
      if (hr <= (T_0 - 5) && prev >= Z_1)
      {
        for (int i = 0; i < NUM_RELAYS; i++) digitalWrite(relayGPIOs[i], HIGH);
        prev = Z_0;
        addLog("AUTO: ZONE 0 - HR: " + String(hr) + " (descent below " + String(T_0-5) + ")");
        broadcastStatus();
      }
      else if (hr < (T_1 - 5) && prev >= Z_2)
      {
        for (int i = 0; i < NUM_RELAYS; i++) digitalWrite(relayGPIOs[i], HIGH);
        digitalWrite(relayGPIOs[0], LOW);
        prev = Z_1;
        addLog("AUTO: ZONE 1 - HR: " + String(hr) + " (descent below " + String(T_1-5) + ")");
        broadcastStatus();
      }
      else if (hr < (T_2 - 5) && prev == Z_3)
      {
        for (int i = 0; i < NUM_RELAYS; i++) digitalWrite(relayGPIOs[i], HIGH);
        digitalWrite(relayGPIOs[1], LOW);
        prev = Z_2;
        addLog("AUTO: ZONE 2 - HR: " + String(hr) + " (descent below " + String(T_2-5) + ")");
        broadcastStatus();
      }
      // Ascending transitions
      else if (hr > T_0 && prev == Z_0)
      {
        for (int i = 0; i < NUM_RELAYS; i++) digitalWrite(relayGPIOs[i], HIGH);
        digitalWrite(relayGPIOs[0], LOW);
        prev = Z_1;
        addLog("AUTO: ZONE 1 - HR: " + String(hr) + " (above " + String(T_0) + ")");
        broadcastStatus();
      }
      else if (hr >= T_1 && (prev == Z_0 || prev == Z_1))
      {
        for (int i = 0; i < NUM_RELAYS; i++) digitalWrite(relayGPIOs[i], HIGH);
        digitalWrite(relayGPIOs[1], LOW);
        prev = Z_2;
        addLog("AUTO: ZONE 2 - HR: " + String(hr) + " (above " + String(T_1) + ")");
        broadcastStatus();
      }
      else if (hr >= T_2 && prev != Z_3)
      {
        for (int i = 0; i < NUM_RELAYS; i++) digitalWrite(relayGPIOs[i], HIGH);
        digitalWrite(relayGPIOs[2], LOW);
        prev = Z_3;
        addLog("AUTO: ZONE 3 - HR: " + String(hr) + " (above " + String(T_2) + ")");
        broadcastStatus();
      }
    }
  }
  // No delay() - loop runs freely, AsyncWebServer handles HTTP/WS in background
}

// Broadcast status update to all WebSocket clients
void broadcastStatus() {
  String statusMsg = "{\"type\":\"status\",\"hr\":" + String(hr) + 
                     ",\"zone\":" + String(prev) + 
                     ",\"connected\":" + String(connected) +
                     ",\"T_0\":" + String(T_0) +
                     ",\"T_1\":" + String(T_1) +
                     ",\"T_2\":" + String(T_2) +
                     ",\"mode\":\"" + String(controlMode == MODE_AUTOMATIC ? "automatic" : "manual") + "\"" +
                     ",\"manualZone\":" + String(manualZone) +
                     ",\"ip\":\"" + WiFi.localIP().toString() +
                     "\",\"uptime\":" + String(millis() / 1000) + "}";
  ws.textAll(statusMsg);
}

void broadcastDeviceList() {
  String msg = "{\"type\":\"deviceList\",\"devices\":[";
  for (size_t i = 0; i < foundDevices.size(); i++) {
    if (i > 0) msg += ",";
    msg += "{\"name\":\"" + foundDevices[i].name + "\",\"address\":\"" + foundDevices[i].address + "\"}";
  }
  msg += "]}";
  ws.textAll(msg);
}
void setZoneForHR(uint8_t heartRate) {
  int targetZone = Z_0;
  
  // Determine appropriate zone based on HR
  if (heartRate >= T_2) {
    targetZone = Z_3;
  } else if (heartRate >= T_1) {
    targetZone = Z_2;
  } else if (heartRate > T_0) {
    targetZone = Z_1;
  } else {
    targetZone = Z_0;
  }
  
  // Turn off all relays first
  for (int i = 0; i < NUM_RELAYS; i++) {
    digitalWrite(relayGPIOs[i], HIGH);
  }
  
  // Turn on appropriate relay for target zone
  if (targetZone == Z_1) {
    digitalWrite(relayGPIOs[0], LOW);
  } else if (targetZone == Z_2) {
    digitalWrite(relayGPIOs[1], LOW);
  } else if (targetZone == Z_3) {
    digitalWrite(relayGPIOs[2], LOW);
  }
  
  prev = targetZone;
  addLog("Initial zone set to " + String(targetZone) + " for HR: " + String(heartRate));
  broadcastStatus();
}