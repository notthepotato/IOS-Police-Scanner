#include "ble_link.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

#include <atomic>

#include "config.h"
#include "phone_clock.h"
#include "text_utils.h"

namespace ble {
namespace {

const Settings* gSettings = nullptr;

constexpr int kMaxDevices = 32;
Device gDevices[kMaxDevices];
int gDeviceCount = 0;
portMUX_TYPE gDevicesLock = portMUX_INITIALIZER_UNLOCKED;

QueueHandle_t gHitQueue = nullptr;
NimBLECharacteristic* gAlertChar = nullptr;
volatile bool gPhoneConnected = false;
std::atomic<bool> gSpeakerTestRequested{false};

const WatchPrefix* findWatch(const uint8_t* addrLE) {
  for (int i = 0; i < gSettings->watchCount; i++) {
    if (matchesPrefix(addrLE, gSettings->watch[i])) return &gSettings->watch[i];
  }
  return nullptr;
}

// Records a sighting. Returns true when it should alert: a new device, or one that's been gone
// longer than forgetMs. Caller holds gDevicesLock.
bool recordSighting(const uint8_t* addr, int8_t rssi, const char* name, const char* label, uint32_t now) {
  for (int i = 0; i < gDeviceCount; i++) {
    Device& d = gDevices[i];
    if (memcmp(d.addr, addr, 6) == 0) {
      bool cameBack = (now - d.lastMs) > gSettings->forgetMs;
      d.lastMs = now;
      d.rssi = rssi;
      if (name[0]) strlcpy(d.name, name, sizeof(d.name));
      return cameBack;
    }
  }
  int slot = 0;
  if (gDeviceCount < kMaxDevices) {
    slot = gDeviceCount++;
  } else {
    for (int i = 1; i < kMaxDevices; i++) {
      if (gDevices[i].lastMs < gDevices[slot].lastMs) slot = i;  // reuse the oldest
    }
  }
  Device& d = gDevices[slot];
  memcpy(d.addr, addr, 6);
  d.lastMs = now;
  d.rssi = rssi;
  strlcpy(d.name, name, sizeof(d.name));
  strlcpy(d.label, label, sizeof(d.label));
  return true;
}

// Runs on the Bluetooth task: record, queue, return quickly. The main loop does the rest.
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    const uint8_t* addr = device->getAddress().getVal();
    const WatchPrefix* watch = findWatch(addr);
    if (!watch) return;

    int8_t rssi = device->getRSSI();
    char name[24];
    strlcpy(name, device->getName().c_str(), sizeof(name));
    sanitizeName(name);

    portENTER_CRITICAL(&gDevicesLock);
    bool alert = recordSighting(addr, rssi, name, watch->label, millis());
    portEXIT_CRITICAL(&gDevicesLock);
    if (!alert) return;

    Hit hit = {};
    formatMac(addr, hit.mac);
    hit.rssi = rssi;
    strlcpy(hit.name, name, sizeof(hit.name));
    strlcpy(hit.label, watch->label, sizeof(hit.label));
    xQueueSend(gHitQueue, &hit, 0);
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    NimBLEDevice::getScan()->start(0, false, true);  // scan forever
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override { gPhoneConnected = true; }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    gPhoneConnected = false;  // advertising restarts automatically (advertiseOnDisconnect)
  }
};

// The phone writes "TIME,<unix>,<utc offset>" right after connecting.
class TimeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    std::string value = characteristic->getValue();
    int64_t unixSeconds = 0;
    int32_t offset = 0;
    if (parseTimeSync(value.c_str(), unixSeconds, offset)) {
      phoneclock::set(unixSeconds, offset);
      Serial.printf("Clock set from phone (UTC offset %ld s)\n", (long)offset);
    }
  }
};

// The phone writes short commands here, e.g. "TEST,SPEAKER". Runs on the Bluetooth task, so it
// only raises a flag; the main loop plays the sound.
class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    std::string value = characteristic->getValue();
    if (parseCommand(value.c_str()) == Command::SpeakerTest) gSpeakerTestRequested = true;
  }
};

}  // namespace

void begin(const Settings& settings) {
  gSettings = &settings;
  gHitQueue = xQueueCreate(8, sizeof(Hit));

  NimBLEDevice::init(RSCAN_DEVICE_NAME);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);
  NimBLEService* service = server->createService(RSCAN_SERVICE_UUID);
  gAlertChar = service->createCharacteristic(RSCAN_ALERT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  gAlertChar->setValue("READY");
  NimBLECharacteristic* timeChar = service->createCharacteristic(RSCAN_TIME_UUID, NIMBLE_PROPERTY::WRITE);
  timeChar->setCallbacks(new TimeCallbacks());
  NimBLECharacteristic* commandChar = service->createCharacteristic(RSCAN_COMMAND_UUID, NIMBLE_PROPERTY::WRITE);
  commandChar->setCallbacks(new CommandCallbacks());
  server->start();  // also starts the service

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(RSCAN_SERVICE_UUID);
  advertising->setName(RSCAN_DEVICE_NAME);
  advertising->start();

  // Passive scan at ~50% duty so the phone connection stays solid. Repeats are reported
  // (true) because they drive the live signal meter.
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCallbacks(), true);
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(50);
  scan->start(0, false, true);
}

bool phoneConnected() { return gPhoneConnected; }

bool nextHit(Hit& out) { return gHitQueue && xQueueReceive(gHitQueue, &out, 0) == pdTRUE; }

Nearby nearby(uint32_t nowMs) {
  Nearby n = {0, -127, "", ""};
  portENTER_CRITICAL(&gDevicesLock);
  for (int i = 0; i < gDeviceCount; i++) {
    const Device& d = gDevices[i];
    if (nowMs - d.lastMs > gSettings->activeMs) continue;
    n.count++;
    if (d.rssi > n.rssi) {
      n.rssi = d.rssi;
      strlcpy(n.name, d.name, sizeof(n.name));
      strlcpy(n.label, d.label, sizeof(n.label));
    }
  }
  portEXIT_CRITICAL(&gDevicesLock);
  return n;
}

void notifyPhone(const char* message, size_t length) {
  if (!gPhoneConnected || !gAlertChar) return;
  gAlertChar->setValue(reinterpret_cast<const uint8_t*>(message), length);
  gAlertChar->notify();
}

void keepScanning() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan->isScanning()) scan->start(0, false, true);
}

bool takeSpeakerTestRequest() { return gSpeakerTestRequested.exchange(false); }

}  // namespace ble
