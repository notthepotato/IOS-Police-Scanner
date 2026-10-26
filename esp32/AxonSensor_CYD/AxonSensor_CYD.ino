/*
  AxonSensor_CYD.ino — police-gear sensor for the Road Scanner iPhone app,
  built for the ESP32-2432S028R "Cheap Yellow Display" (2.8" 240x320 touch screen).

  iPhones can't see other devices' Bluetooth MAC addresses, so this board does the
  scanning: it passively listens for Bluetooth LE advertisements from Axon equipment
  (body cams, Tasers, in-car systems), which use Axon's registered address prefix
  00:25:DF. It shows what it finds on its own screen and sends each new detection to
  the phone as a notification:

      AXON,<mac>,<rssi>,<name>

  On the board
    Screen      green "ALL CLEAR" while scanning; turns red with a live signal meter
                while police gear is in range
    Tap screen  mute / unmute the beeps (remembered)
    Hold screen cycle brightness 100% > 50% > 20% (remembered)
    BOOT button fix colors if the screen looks inverted (some revisions; remembered)
    RGB LED     red while police gear is in range (on the back of the board)
    Speaker     beeps through a small speaker on the 2-pin "SPEAK" connector (optional)

  Arduino IDE setup
    Board package: "esp32" by Espressif Systems, version 3.x
    Board:         "ESP32 Dev Module"
    Libraries (Library Manager):
      NimBLE-Arduino by h2zero (2.x)
      Adafruit ILI9341 (say yes to installing Adafruit GFX and Adafruit BusIO)
      XPT2046_Touchscreen by Paul Stoffregen
*/
#include <SPI.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <NimBLEDevice.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include "RScanTypes.h"

// ============================ settings you might change ============================
const uint8_t  SCREEN_ROTATION = 1;       // 1 = landscape; 3 = landscape upside down
const uint32_t FORGET_MS = 120000;        // alert again for a device after it's been gone 2 min
const uint32_t ACTIVE_MS = 20000;         // treat a device as "still here" for 20 s after its last signal
const uint16_t BEEP_HZ = 2600;            // beep pitch
const uint8_t  BRIGHTNESS_LEVELS[] = {255, 128, 50};  // what "hold screen" cycles through

// Must match SensorUUIDs in the iPhone app (SensorLink.swift).
#define SERVICE_UUID    "6E3A0001-5C1B-4B3D-9E1E-7A2F4C8D0B11"
#define ALERT_CHAR_UUID "6E3A0002-5C1B-4B3D-9E1E-7A2F4C8D0B11"

// ============================ ESP32-2432S028R pins ============================
// Display (HSPI)
const int TFT_SCK = 14, TFT_MISO = 12, TFT_MOSI = 13, TFT_CS = 15, TFT_DC = 2, TFT_BL = 21;
// Touch controller XPT2046 (VSPI)
const int TOUCH_SCK = 25, TOUCH_MISO = 39, TOUCH_MOSI = 32, TOUCH_CS = 33, TOUCH_IRQ = 36;
// RGB LED (active LOW), speaker amp, BOOT button
const int LED_R = 4, LED_G = 16, LED_B = 17;
const int SPEAKER_PIN = 26;
const int BOOT_BUTTON = 0;

// ============================ colors (RGB565) ============================
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t C_BG = rgb(0, 0, 0);
const uint16_t C_BAR = rgb(28, 32, 40);
const uint16_t C_TEXT = rgb(255, 255, 255);
const uint16_t C_DIM = rgb(140, 146, 158);
const uint16_t C_GREEN = rgb(64, 220, 140);
const uint16_t C_AMBER = rgb(255, 176, 32);
const uint16_t C_ALERT = rgb(180, 16, 24);
const uint16_t C_ALERT_DIM = rgb(110, 40, 44);

// ============================ hardware objects ============================
SPIClass tftSPI(HSPI);
SPIClass touchSPI(VSPI);
Adafruit_ILI9341 tft(&tftSPI, TFT_DC, TFT_CS, -1);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;

// ============================ Bluetooth state ============================
const int MAX_DEVICES = 32;
static Device devices[MAX_DEVICES];
static int deviceCount = 0;
static portMUX_TYPE devicesLock = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t hitQueue;
static NimBLECharacteristic* alertChar = nullptr;
static volatile bool phoneConnected = false;

static bool isAxon(const NimBLEAddress& address) {
  const uint8_t* v = address.getVal();  // little-endian: v[5] is the first byte of the MAC
  return v[5] == 0x00 && v[4] == 0x25 && v[3] == 0xDF;
}

static void formatMac(const uint8_t* v, char* out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
}

// Records a sighting. Returns true when it should alert: a new device, or one that's been
// gone longer than FORGET_MS. Caller must hold devicesLock.
static bool recordSighting(const uint8_t* addr, int8_t rssi, const char* name, uint32_t now) {
  for (int i = 0; i < deviceCount; i++) {
    Device& d = devices[i];
    if (memcmp(d.addr, addr, 6) == 0) {
      bool cameBack = (now - d.lastMs) > FORGET_MS;
      d.lastMs = now;
      d.rssi = rssi;
      if (name[0]) strlcpy(d.name, name, sizeof(d.name));
      return cameBack;
    }
  }
  int slot = 0;
  if (deviceCount < MAX_DEVICES) {
    slot = deviceCount++;
  } else {
    for (int i = 1; i < MAX_DEVICES; i++) {
      if (devices[i].lastMs < devices[slot].lastMs) slot = i;  // overwrite the oldest
    }
  }
  Device& d = devices[slot];
  memcpy(d.addr, addr, 6);
  d.lastMs = now;
  d.rssi = rssi;
  strlcpy(d.name, name, sizeof(d.name));
  return true;
}

// Runs on the Bluetooth task, so it only records and queues; loop() does the rest.
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    const NimBLEAddress& address = device->getAddress();
    if (!isAxon(address)) return;

    const uint8_t* v = address.getVal();
    int8_t rssi = device->getRSSI();
    char name[24] = "";
    strlcpy(name, device->getName().c_str(), sizeof(name));
    for (char* p = name; *p; p++) {
      if (*p == ',' || *p < 0x20 || *p > 0x7E) *p = ' ';  // keep the CSV format and the font happy
    }

    portENTER_CRITICAL(&devicesLock);
    bool alert = recordSighting(v, rssi, name, millis());
    portEXIT_CRITICAL(&devicesLock);
    if (!alert) return;

    Hit hit = {};
    formatMac(v, hit.mac);
    hit.rssi = rssi;
    strlcpy(hit.name, name, sizeof(hit.name));
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

// Strongest device currently in range.
static Nearby readNearby(uint32_t now) {
  Nearby n = {0, -127, ""};
  portENTER_CRITICAL(&devicesLock);
  for (int i = 0; i < deviceCount; i++) {
    const Device& d = devices[i];
    if (now - d.lastMs > ACTIVE_MS) continue;
    n.count++;
    if (d.rssi > n.rssi) {
      n.rssi = d.rssi;
      strlcpy(n.name, d.name, sizeof(n.name));
    }
  }
  portEXIT_CRITICAL(&devicesLock);
  return n;
}

// ============================ helpers ============================
static const char* rangeLabel(int rssi) {
  if (rssi >= -60) return "VERY CLOSE";
  if (rssi >= -75) return "CLOSE";
  if (rssi >= -88) return "NEARBY";
  return "IN RANGE";
}

static int signalBars(int rssi) {
  if (rssi >= -60) return 5;
  if (rssi >= -68) return 4;
  if (rssi >= -76) return 3;
  if (rssi >= -84) return 2;
  return 1;
}

static void setLed(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);  // active LOW
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

static void drawText(const char* text, int16_t x, int16_t baseline, const GFXfont* font, uint16_t color) {
  tft.setFont(font);
  tft.setTextColor(color);
  tft.setCursor(x, baseline);
  tft.print(text);
}

static uint16_t textWidth(const char* text, const GFXfont* font) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setFont(font);
  tft.getTextBounds(text, 0, 100, &x1, &y1, &w, &h);
  return w + x1;
}

static void drawCentered(const char* text, int16_t baseline, const GFXfont* font, uint16_t color) {
  int16_t x = (tft.width() - (int16_t)textWidth(text, font)) / 2;
  drawText(text, x < 0 ? 0 : x, baseline, font, color);
}

static void drawRight(const char* text, int16_t right, int16_t baseline, const GFXfont* font, uint16_t color) {
  drawText(text, right - (int16_t)textWidth(text, font), baseline, font, color);
}

// ============================ settings (saved in flash) ============================
static bool muted = false;
static bool invertColors = false;
static uint8_t brightnessIndex = 0;

static void applyBrightness() {
  ledcWrite(TFT_BL, BRIGHTNESS_LEVELS[brightnessIndex]);
}

// ============================ beeps (non-blocking) ============================
static int beepsLeft = 0;
static uint32_t nextBeepMs = 0;

static void startBeeps(int count) {
  if (muted) return;
  beepsLeft = count;
  nextBeepMs = millis();
}

static void serviceBeeps(uint32_t now) {
  if (beepsLeft <= 0 || (int32_t)(now - nextBeepMs) < 0) return;
  tone(SPEAKER_PIN, BEEP_HZ, 110);
  beepsLeft--;
  nextBeepMs = now + 230;
}

// ============================ screen ============================
// Layout (320 x 240): header 0-35, main 36-199, footer 200-239
const int HEADER_H = 36, MAIN_Y = 36, MAIN_H = 164, FOOTER_Y = 200;

static int alertsSinceBoot = 0;
static uint32_t lastPoliceMs = 0;
static bool everSeenPolice = false;

// What's currently drawn, so we only redraw what changed (no flicker).
static int shownPhone = -1, shownMuted = -1, shownMode = -1, shownAlerts = -1;
static int shownBars = -1, shownCount = -1, shownRssi = 1000, shownMinutes = -2;
static char shownName[24] = "";
static uint32_t lastDetailDrawMs = 0;

static void drawHeader() {
  tft.fillRect(0, 0, tft.width(), HEADER_H, C_BAR);
  drawText("RScan", 10, 25, &FreeSansBold12pt7b, C_TEXT);
  if (muted) drawCentered("MUTED", 24, &FreeSans9pt7b, C_AMBER);
  bool connected = phoneConnected;
  uint16_t dot = connected ? C_GREEN : C_DIM;
  tft.fillCircle(tft.width() - 16, 18, 6, dot);
  drawRight(connected ? "Phone" : "No phone", tft.width() - 30, 24, &FreeSans9pt7b, connected ? C_TEXT : C_DIM);
  shownPhone = connected;
  shownMuted = muted;
}

static void drawFooter() {
  tft.fillRect(0, FOOTER_Y, tft.width(), tft.height() - FOOTER_Y, C_BAR);
  char text[24];
  snprintf(text, sizeof(text), "Alerts: %d", alertsSinceBoot);
  drawText(text, 10, 226, &FreeSans9pt7b, C_TEXT);
  drawRight("Tap: mute  Hold: dim", tft.width() - 10, 226, &FreeSans9pt7b, C_DIM);
  shownAlerts = alertsSinceBoot;
}

static int minutesSinceLastPolice(uint32_t now) {
  if (!everSeenPolice) return -1;
  return (now - lastPoliceMs) / 60000;
}

static void drawIdleLastSeen(uint32_t now) {
  int minutes = minutesSinceLastPolice(now);
  tft.fillRect(0, 152, tft.width(), 30, C_BG);
  char text[40];
  if (minutes < 0) {
    snprintf(text, sizeof(text), "No police gear seen yet");
  } else if (minutes == 0) {
    snprintf(text, sizeof(text), "Last seen: under a minute ago");
  } else if (minutes < 60) {
    snprintf(text, sizeof(text), "Last seen: %d min ago", minutes);
  } else {
    snprintf(text, sizeof(text), "Last seen: %d hr %d min ago", minutes / 60, minutes % 60);
  }
  drawCentered(text, 172, &FreeSans9pt7b, C_DIM);
  shownMinutes = minutes;
}

static void drawIdle(uint32_t now) {
  tft.fillRect(0, MAIN_Y, tft.width(), MAIN_H, C_BG);
  drawCentered("ALL CLEAR", 110, &FreeSansBold24pt7b, C_GREEN);
  drawCentered("Scanning for police gear", 140, &FreeSans9pt7b, C_TEXT);
  drawIdleLastSeen(now);
  shownMode = 0;
}

static void drawAlertDetails(const Nearby& n) {
  // Everything below the "POLICE GEAR" title.
  tft.fillRect(0, 100, tft.width(), FOOTER_Y - 100, C_ALERT);
  drawCentered(rangeLabel(n.rssi), 128, &FreeSansBold12pt7b, C_TEXT);

  int bars = signalBars(n.rssi);
  const int barW = 18, gap = 8, bottom = 172;
  int x = (tft.width() - (5 * barW + 4 * gap)) / 2;
  for (int i = 0; i < 5; i++) {
    int h = 8 + i * 7;
    if (i < bars) {
      tft.fillRect(x, bottom - h, barW, h, C_TEXT);
    } else {
      tft.drawRect(x, bottom - h, barW, h, C_ALERT_DIM);
    }
    x += barW + gap;
  }

  // "Axon Body 4, -58 dBm  (+1 more)", shortening the name until it fits the screen.
  char name[24];
  strlcpy(name, n.name[0] ? n.name : "Axon device", sizeof(name));
  char line[64];
  for (;;) {
    if (n.count > 1) {
      snprintf(line, sizeof(line), "%s, %d dBm  (+%d more)", name, n.rssi, n.count - 1);
    } else {
      snprintf(line, sizeof(line), "%s, %d dBm", name, n.rssi);
    }
    size_t len = strlen(name);
    if (textWidth(line, &FreeSans9pt7b) <= tft.width() - 12 || len <= 3) break;
    name[len - 1] = '\0';
    for (size_t k = strlen(name); k > 0 && name[k - 1] == ' '; k--) name[k - 1] = '\0';
  }
  drawCentered(line, 194, &FreeSans9pt7b, C_TEXT);

  shownBars = bars;
  shownCount = n.count;
  shownRssi = n.rssi;
  strlcpy(shownName, n.name, sizeof(shownName));
  lastDetailDrawMs = millis();
}

static void drawAlert(const Nearby& n) {
  tft.fillRect(0, MAIN_Y, tft.width(), MAIN_H, C_ALERT);
  drawCentered("POLICE GEAR", 84, &FreeSansBold18pt7b, C_TEXT);
  drawAlertDetails(n);
  shownMode = 1;
}

static void drawSplash() {
  tft.fillScreen(C_BG);
  drawCentered("RScan", 90, &FreeSansBold24pt7b, C_GREEN);
  drawCentered("Police-gear sensor for Road Scanner", 125, &FreeSans9pt7b, C_TEXT);
  drawCentered("Tap: mute     Hold: brightness", 165, &FreeSans9pt7b, C_DIM);
  drawCentered("Colors look wrong? Press BOOT", 190, &FreeSans9pt7b, C_DIM);
}

static void drawAll(uint32_t now, const Nearby& n) {
  tft.fillScreen(C_BG);
  drawHeader();
  drawFooter();
  if (n.count > 0) drawAlert(n); else drawIdle(now);
}

static void updateScreen(uint32_t now, const Nearby& n) {
  if (shownPhone != (int)phoneConnected || shownMuted != (int)muted) drawHeader();
  if (shownAlerts != alertsSinceBoot) drawFooter();

  if (n.count > 0) {
    if (shownMode != 1) {
      drawAlert(n);
    } else {
      bool changed = signalBars(n.rssi) != shownBars || n.count != shownCount || strcmp(n.name, shownName) != 0 ||
                     abs(n.rssi - shownRssi) >= 3;
      if (changed && now - lastDetailDrawMs >= 500) drawAlertDetails(n);  // max 2 redraws a second
    }
  } else {
    if (shownMode != 0) {
      drawIdle(now);
    } else if (minutesSinceLastPolice(now) != shownMinutes) {
      drawIdleLastSeen(now);
    }
  }
}

// ============================ touch + BOOT button ============================
static bool touchDown = false;
static bool holdHandled = false;
static uint32_t touchStartMs = 0;

static void handleTouch(uint32_t now) {
  bool pressed = touch.touched();
  if (pressed && !touchDown) {
    touchDown = true;
    holdHandled = false;
    touchStartMs = now;
  } else if (pressed && touchDown) {
    if (!holdHandled && now - touchStartMs >= 800) {  // hold: next brightness level
      holdHandled = true;
      brightnessIndex = (brightnessIndex + 1) % sizeof(BRIGHTNESS_LEVELS);
      applyBrightness();
      prefs.putUChar("bright", brightnessIndex);
    }
  } else if (!pressed && touchDown) {
    touchDown = false;
    if (!holdHandled && now - touchStartMs >= 30) {  // tap: mute / unmute
      muted = !muted;
      if (muted) {
        beepsLeft = 0;
        noTone(SPEAKER_PIN);
      }
      prefs.putBool("muted", muted);
    }
  }
}

static bool bootWasDown = false;
static uint32_t bootChangedMs = 0;

static void handleBootButton(uint32_t now) {
  bool down = digitalRead(BOOT_BUTTON) == LOW;
  if (down == bootWasDown || now - bootChangedMs < 50) return;  // debounce
  bootWasDown = down;
  bootChangedMs = now;
  if (down) {
    invertColors = !invertColors;
    tft.invertDisplay(invertColors);
    prefs.putBool("invert", invertColors);
  }
}

// ============================ setup / loop ============================
void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setLed(false, false, false);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  hitQueue = xQueueCreate(8, sizeof(Hit));

  prefs.begin("rscan", false);
  muted = prefs.getBool("muted", false);
  invertColors = prefs.getBool("invert", false);
  brightnessIndex = prefs.getUChar("bright", 0) % sizeof(BRIGHTNESS_LEVELS);

  // Screen + backlight
  tftSPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(SCREEN_ROTATION);
  tft.invertDisplay(invertColors);
  ledcAttach(TFT_BL, 5000, 8);
  applyBrightness();
  drawSplash();

  // Touch
  touchSPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSPI);

  // Bluetooth: GATT server the iPhone connects to
  NimBLEDevice::init("RScan");
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);
  NimBLEService* service = server->createService(SERVICE_UUID);
  alertChar = service->createCharacteristic(ALERT_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  alertChar->setValue("READY");
  server->start();  // also starts the service

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setName("RScan");
  advertising->start();

  // Passive scan for Axon gear, about 50% duty so the phone connection stays solid.
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCallbacks(), true);  // true = report repeats, used for the live signal meter
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(50);
  scan->start(0, false, true);

  Serial.println("RScan ready: scanning for Axon devices (00:25:DF)...");
  delay(2500);  // show the splash; detections queue up meanwhile

  uint32_t now = millis();
  drawAll(now, readNearby(now));
}

void loop() {
  uint32_t now = millis();

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
    alertsSinceBoot++;
    startBeeps(3);
  }

  Nearby nearby = readNearby(now);
  if (nearby.count > 0) {
    lastPoliceMs = now;
    everSeenPolice = true;
  }
  setLed(nearby.count > 0, false, false);

  handleTouch(now);
  handleBootButton(now);
  serviceBeeps(now);
  updateScreen(now, nearby);

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan->isScanning()) scan->start(0, false, true);
  delay(20);
}
