/**
 * A modified BLE client that will read BLE HRM, Forerunner and Fenix broadcasting HR 
 * and control a relay
 * author Andrew Grabbs, Petr Divis
 * added led to signal connected HRM (led with 1000 Ohm resistor to GPIO and GND of 3V3)
 */

#include "Arduino.h"
#include "BLEDevice.h"
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <iostream>
#include <sstream>
#include <vector>

// Set to true to define Relay as Normally Open (NO)
#define RELAY_NO true

// Set number of relays
#define NUM_RELAYS 3

#define Z_00 -1
#define Z_0 0
#define Z_1 1
#define Z_2 2
#define Z_3 3

// Heart Rate Tresholds
int T_0 = 110; // start fan from 110 bpm
int T_1 = 150; // enter second speed at 150 bpm
int T_2 = 160; // enter third speed at 160 bpm

bool forceOn = false;


// Assign each GPIO to a relay
uint8_t relayGPIOs[NUM_RELAYS] = {25, 26, 27};
//LEDGPIO
uint8_t ledPin = 19;

// The remote service we wish to connect to.
static BLEUUID serviceUUID("0000180d-0000-1000-8000-00805f9b34fb");
// The characteristic of the remote service we are interested in.
static BLEUUID charUUID(BLEUUID((uint16_t)0x2A37));
//0x2A37

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic *pCharacteristic;
BLECharacteristic *pRecCharacteristic;
static bool phoneConnected = false;
static short prev = 0;
static uint8_t hr = 0;
static boolean doConnect = false;
static boolean connected = false;
static boolean notification = false;
static boolean doScan = true;
static BLERemoteCharacteristic *pRemoteCharacteristic;
static BLEAdvertisedDevice *myDevice;
static BLEScan *pBLEScan;
static BLEServer *pServer;

class PhoneConnection: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      phoneConnected = true;
    };

    void onDisconnect(BLEServer* pServer) {
      phoneConnected = false;
    }
};

class PhoneCallbacks: public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *pCharacteristic) {
    char * txString = (char *)malloc(128);
    snprintf(txString, 128, "Z %d %u bpm", prev, hr);
    pCharacteristic->setValue(txString);
  }
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();

      if (rxValue.length() > 0) {
        Serial.println("*********");
        Serial.print("Received Value: ");

        for (int i = 0; i < rxValue.length(); i++) {
          Serial.print(rxValue[i]);
        }

        Serial.println();

        // Do stuff based on the command received from the app
        if (rxValue.find("ON") != -1) { 
          Serial.println("Turning ON!");
          forceOn = true;
        }
        if (rxValue.find("OFF") != -1) { 
          Serial.println("Turning OFF!");
          forceOn = false;
        }
        if (rxValue.find("*") != -1) { 
          forceOn = false;
          std::vector<int> result;
          std::stringstream ss (rxValue);
          std::string item;

          while (std::getline (ss, item, '*')) {
              result.push_back (atoi( item.c_str() ));
          }
          T_0 = result[0];
          T_1 = result[1];
          T_2 = result[2];
        }

        Serial.println();
        Serial.println("*********");
      }
    }
};

static void phoneZoneNotify(int zone, uint8_t hr){
  // notify on zone
  if (phoneConnected) {

    char * txString = (char *)malloc(128);
    snprintf(txString, 128, "Z %d %u bpm", zone, hr);
    pCharacteristic->setValue(txString);
    
    pCharacteristic->notify(); // Send the value to the app!
    Serial.print("*** Sent Value: ");
    Serial.print(txString);
    Serial.println(" ***");

  }
}

static void notifyCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
  Serial.print("Heart Rate: ");
  Serial.print(pData[1], DEC);
  Serial.println("bpm");

  hr = pData[1];

  if (forceOn) {
    for (int i = 1; i <= NUM_RELAYS; i++)
    {
      digitalWrite(relayGPIOs[i - 1], HIGH);
    }
    digitalWrite(relayGPIOs[2], LOW);
    prev = Z_00;
    Serial.println("ZONE 00!");
    phoneZoneNotify(-1,hr);
  }
  else if (pData[1] <= (T_0 - 5) && prev != Z_0)
  {
    for (int i = 1; i <= NUM_RELAYS; i++)
    {
      digitalWrite(relayGPIOs[i - 1], HIGH);
    }
    prev = Z_0;
    Serial.println("ZONE 0!");
    digitalWrite(ledPin, HIGH);
    phoneZoneNotify(0,hr);
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
    phoneZoneNotify(1,hr);
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
    phoneZoneNotify(2,hr);
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
    phoneZoneNotify(3,hr);
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
  BLEDevice::init("DYI FAN");
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

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new PhoneConnection());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_TX,
                      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
                    );
                      
  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setCallbacks(new PhoneCallbacks());

  pRecCharacteristic = pService->createCharacteristic(
                                         CHARACTERISTIC_UUID_RX,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );

  pRecCharacteristic->setCallbacks(new PhoneCallbacks());

  // Start the service
  pService->start();

  // Start advertising
  pServer->getAdvertising()->start();
  Serial.println("Waiting a client connection to notify...");
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
  if(!phoneConnected){
    pServer->getAdvertising()->start();
  }

  delay(1000); // Delay a second between loops.
} // End of loop
