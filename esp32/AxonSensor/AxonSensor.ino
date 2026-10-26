/*
  AxonSensor.ino — police-gear sensor for the Road Scanner iPhone app.

  iPhones can't see other devices' Bluetooth MAC addresses, so this ESP32 does the
  scanning: it passively listens for Bluetooth LE advertisements from Axon equipment
  (body cams, Tasers, in-car systems), which use Axon's registered address prefix
  00:25:DF, and sends each new detection to the phone as a notification:

      AXON,<mac>,<rssi>,<name>

  It also prints detections to the Serial Monitor (115200), so it works on its own too.

  Board:   any ESP32 with Bluetooth LE (ESP32, ESP32-S3, -C3, -C6). NOT the ESP32-S2.
  Library: NimBLE-Arduino by h2zero, version 2.x (Arduino IDE > Library Manager).
  Power:   any USB port or USB car charger.
*/
#include <NimBLEDevice.h>

// Must match SensorUUIDs in the iPhone app (SensorLink.swift).
#define SERVICE_UUID    "6E3A0001-5C1B-4B3D-9E1E-7A2F4C8D0B11"
#define ALERT_CHAR_UUID "6E3A0002-5C1B-4B3D-9E1E-7A2F4C8D0B11"

const int LED_PIN = 2;               // onboard LED on most ESP32 dev boards; -1 to disable
const int BUZZER_PIN = -1;           // active buzzer (e.g. 17); -1 to disable
const uint32_t FORGET_MS = 120000;   // re-alert a device after it's been gone 2 minutes

struct Hit {
  char mac[18];
  int8_t rssi;
  char name[24];
};

static QueueHandle_t hitQueue;
static NimBLECharacteristic* alertChar = nullptr;
static volatile bool phoneConnected = false;

// ---- remember recently seen devices so one officer doesn't trigger 100 alerts ----
struct Seen {
  uint8_t addr[6];
  uint32_t lastMs;
};
static Seen seen[32];
static int seenCount = 0;

static bool isAxon(const NimBLEAddress& address) {
  const uint8_t* v = address.getVal();  // little-endian: v[5] is the first byte of the MAC
  return v[5] == 0x00 && v[4] == 0x25 && v[3] == 0xDF;
}

static bool shouldAlert(const uint8_t* addr, uint32_t now) {
  for (int i = 0; i < seenCount; i++) {
    if (memcmp(seen[i].addr, addr, 6) == 0) {
      bool cameBack = (now - seen[i].lastMs) > FORGET_MS;
      seen[i].lastMs = now;
      return cameBack;
    }
  }
  int slot = 0;
  if (seenCount < 32) {
    slot = seenCount++;
  } else {
    for (int i = 1; i < 32; i++) {
      if (seen[i].lastMs < seen[slot].lastMs) slot = i;  // overwrite the oldest
    }
  }
  memcpy(seen[slot].addr, addr, 6);
  seen[slot].lastMs = now;
  return true;
}

// ---- Bluetooth callbacks (run on the BLE task, so they only queue work for loop()) ----
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    const NimBLEAddress& address = device->getAddress();
    if (!isAxon(address)) return;
    const uint8_t* v = address.getVal();
    if (!shouldAlert(v, millis())) return;

    Hit hit = {};
    snprintf(hit.mac, sizeof(hit.mac), "%02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
    hit.rssi = device->getRSSI();
    std::string name = device->getName();
    strncpy(hit.name, name.c_str(), sizeof(hit.name) - 1);
    for (char* p = hit.name; *p; p++) {
      if (*p == ',') *p = ' ';  // keep the phone's comma-separated format intact
    }
    xQueueSend(hitQueue, &hit, 0);
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    NimBLEDevice::getScan()->start(0, false, true);  // scan forever
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    phoneConnected = true;
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    phoneConnected = false;  // advertising restarts automatically (advertiseOnDisconnect)
  }
};

static void flash(int times) {
  for (int i = 0; i < times; i++) {
    if (LED_PIN >= 0) digitalWrite(LED_PIN, HIGH);
    if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, HIGH);
    delay(120);
    if (LED_PIN >= 0) digitalWrite(LED_PIN, LOW);
    if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, LOW);
    delay(120);
  }
}

void setup() {
  Serial.begin(115200);
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);
  if (BUZZER_PIN >= 0) pinMode(BUZZER_PIN, OUTPUT);
  hitQueue = xQueueCreate(8, sizeof(Hit));

  NimBLEDevice::init("RScan");

  // GATT server the iPhone connects to.
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);
  NimBLEService* service = server->createService(SERVICE_UUID);
  alertChar = service->createCharacteristic(ALERT_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  alertChar->setValue("READY");
  service->start();
  server->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setName("RScan");
  advertising->start();

  // Passive scan for Axon gear, about 50% duty so the phone connection stays solid.
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCallbacks(), true);  // true = report repeats, used to track "still here"
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(50);
  scan->start(0, false, true);

  Serial.println("RScan ready: scanning for Axon devices (00:25:DF)...");
}

void loop() {
  Hit hit;
  while (xQueueReceive(hitQueue, &hit, 0) == pdTRUE) {
    char message[64];
    int len = snprintf(message, sizeof(message), "AXON,%s,%d,%s", hit.mac, (int)hit.rssi, hit.name);
    if (len < 0) continue;
    if (len >= (int)sizeof(message)) len = sizeof(message) - 1;
    Serial.println(message);
    if (phoneConnected && alertChar != nullptr) {
      alertChar->setValue(reinterpret_cast<const uint8_t*>(message), (size_t)len);
      alertChar->notify();
    }
    flash(3);
  }

  // LED stays on while the phone is connected.
  if (LED_PIN >= 0) digitalWrite(LED_PIN, phoneConnected ? HIGH : LOW);

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan->isScanning()) scan->start(0, false, true);
  delay(20);
}
