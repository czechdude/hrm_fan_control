/**
 * A modified BLE client that will read BLE HRM, Forerunner and Fenix broadcasting HR 
 * and control a relay
 * author Andrew Grabbs, Petr Divis
 * added led to signal connected HRM (led with 1000 Ohm resistor to GPIO and GND of 3V3)
 */

#include "Arduino.h"
#include "BLEDevice.h"
//#include "BLEScan.h"

// Set to true to define Relay as Normally Open (NO)
#define RELAY_NO true

// Set number of relays
#define NUM_RELAYS 3

// Heart Rate Tresholds
#define T_0 110 // start fan from 110 bpm
#define T_1 150 // enter second speed at 150 bpm
#define T_2 160 // enter third speed at 160 bpm

#define Z_0 0
#define Z_1 1
#define Z_2 2
#define Z_3 3

// Assign each GPIO to a relay
uint8_t relayGPIOs[NUM_RELAYS] = {25, 26, 27};
//LEDGPIO
uint8_t ledPin = 19;

// The remote service we wish to connect to.
static BLEUUID serviceUUID("0000180d-0000-1000-8000-00805f9b34fb");
// The characteristic of the remote service we are interested in.
static BLEUUID charUUID(BLEUUID((uint16_t)0x2A37));
//0x2A37

static short prev = -1;
static boolean doConnect = false;
static boolean connected = false;
static boolean notification = false;
static boolean doScan = true;
static BLERemoteCharacteristic *pRemoteCharacteristic;
static BLEAdvertisedDevice *myDevice;
static BLEScan *pBLEScan;

static void notifyCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
  Serial.print("Heart Rate: ");
  Serial.print(pData[1], DEC);
  Serial.println("bpm");
  if (pData[1] <= (T_0 - 5) && prev != Z_0)
  {
    for (int i = 1; i <= NUM_RELAYS; i++)
    {
      digitalWrite(relayGPIOs[i - 1], HIGH);
    }
    prev = Z_0;
    Serial.println("ZONE 0!");
    digitalWrite(ledPin, HIGH);
  }
  else if (pData[1] > T_0 && pData[1] <= (T_1 - 5) && prev != Z_1)
  {

    for (int i = 1; i <= NUM_RELAYS; i++)
    {
      digitalWrite(relayGPIOs[i - 1], HIGH);
    }
    digitalWrite(relayGPIOs[0], LOW);
    prev = Z_1;
    Serial.println("ZONE 1!");
    digitalWrite(ledPin, LOW);
    delay(200);
    digitalWrite(ledPin, HIGH);
  }
  else if (pData[1] > T_1 && pData[1] <= (T_2 - 5) && prev != Z_2)
  {

    for (int i = 1; i <= NUM_RELAYS; i++)
    {
      digitalWrite(relayGPIOs[i - 1], HIGH);
    }
    digitalWrite(relayGPIOs[1], LOW);
    prev = Z_2;
    Serial.println("ZONE 2!");
    digitalWrite(ledPin, LOW);
    delay(200);
    digitalWrite(ledPin, HIGH);
    delay(200);
    digitalWrite(ledPin, LOW);
    delay(200);
    digitalWrite(ledPin, HIGH);
  }
  else if (pData[1] > T_2 && prev != Z_3)
  {

    for (int i = 1; i <= NUM_RELAYS; i++)
    {
      digitalWrite(relayGPIOs[i - 1], HIGH);
    }
    digitalWrite(relayGPIOs[2], LOW);
    prev = Z_3;
    Serial.println("ZONE 3!");
    digitalWrite(ledPin, LOW);
    delay(200);
    digitalWrite(ledPin, HIGH);
    delay(200);
    digitalWrite(ledPin, LOW);
    delay(200);
    digitalWrite(ledPin, HIGH);
    delay(200);
    digitalWrite(ledPin, LOW);
    delay(200);
    digitalWrite(ledPin, HIGH);
  }
}

class MyClientCallback : public BLEClientCallbacks
{
  void onConnect(BLEClient *pclient)
  {
  }

  void onDisconnect(BLEClient *pclient)
  {
    digitalWrite(ledPin, LOW);
    for (int i = 1; i <= NUM_RELAYS; i++)
    {
      digitalWrite(relayGPIOs[i - 1], HIGH);
    }
    prev = -1;
    connected = false;
    doScan = true;
    Serial.println("onDisconnect");
  }
};

bool connectToServer()
{
  Serial.print("Forming a connection to ");
  Serial.println(myDevice->getAddress().toString().c_str());

  BLEClient *pClient = BLEDevice::createClient();
  Serial.println(" - Created client");

  pClient->setClientCallbacks(new MyClientCallback());

  // Connect to the remove BLE Server.
  pClient->connect(myDevice); // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
  Serial.println(" - Connected to server");

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr)
  {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(serviceUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our service");

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr)
  {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(charUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our characteristic");
  if (pRemoteCharacteristic->canNotify())
    pRemoteCharacteristic->registerForNotify(notifyCallback);
  connected = true;
  return true;
}
/**
 * Scan for BLE servers and find the first one that advertises the service we are looking for.
 */
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());

    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID))
    {

      pBLEScan->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = false;

    } // Found our server
    else
    {
      Serial.println("No service available");
    }
  } // onResult
};  // MyAdvertisedDeviceCallbacks

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting Arduino BLE Client application...");
  BLEDevice::init("");
  // Set all relays to off when the program starts - if set to Normally Open (NO), the relay is off when you set the relay to HIGH
  for (int i = 1; i <= NUM_RELAYS; i++)
  {
    pinMode(relayGPIOs[i - 1], OUTPUT);
    if (RELAY_NO)
    {
      digitalWrite(relayGPIOs[i - 1], HIGH);
    }
    else
    {
      digitalWrite(relayGPIOs[i - 1], LOW);
    }
  }
  pinMode(ledPin, OUTPUT);
} // End of setup.

// This is the Arduino main loop function.
void loop()
{
  // If the flag "doConnect" is true then we have scanned for and found the desired
  // BLE Server with which we wish to connect.  Now we connect to it.  Once we are
  // connected we set the connected flag to be true.
  if (doConnect == true)
  {
    if (connectToServer())
    {
      Serial.println("We are now connected to the BLE Server.");
    }
    else
    {
      Serial.println("We have failed to connect to the server; there is nothin more we will do.");
    }
    doConnect = false;
  }

  // If we are connected to a peer BLE Server, update the characteristic each time we are reached
  // with the current time since boot.
  if (connected)
  {
    if (notification == false)
    {
      Serial.println("Turning Notification On");
      const uint8_t onPacket[] = {0x01, 0x0};
      pRemoteCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t *)onPacket, 2, true);
      notification = true;
    }
  }
  else if (doScan)
  {
    Serial.println("Rescanning");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setInterval(1349);
    pBLEScan->setWindow(449);
    pBLEScan->setActiveScan(true);
    pBLEScan->start(5, false);
    notification = false;
  }

  delay(1000); // Delay a second between loops.
} // End of loop
