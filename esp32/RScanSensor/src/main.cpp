// RScan — police-gear sensor for the Road Scanner iPhone app, on the ESP32-2432S028R
// "Cheap Yellow Display".
//
// Passively listens for Bluetooth LE advertisements from Axon equipment (body cams, Tasers,
// in-car systems; address prefix 00:25:DF), shows them on screen, beeps, logs them to the SD
// card, and sends each new detection to the phone.
//
//   Tap screen    mute / unmute beeps        (remembered)
//   Hold screen   brightness 100/50/20%      (remembered)
//   BOOT button   fix inverted colors        (remembered)
//   SD card       rscan/config.txt settings, rscan/log.csv detection log (optional)
#include <Arduino.h>
#include <Preferences.h>
#include <XPT2046_Bitbang.h>
#include <time.h>

#include "ble_link.h"
#include "config.h"
#include "phone_clock.h"
#include "screen.h"
#include "sd_storage.h"
#include "text_utils.h"
#include "types.h"

namespace {

Settings gSettings;
Preferences gPrefs;
XPT2046_Bitbang gTouch(Pin::TouchMosi, Pin::TouchMiso, Pin::TouchClk, Pin::TouchCs);

const uint8_t kBrightness[] = {255, 128, 50};
constexpr uint16_t kTouchPressure = 300;  // XPT2046 pressure reading that counts as a touch

bool gMuted = false;
bool gInvert = false;
uint8_t gBrightnessIndex = 0;

int gAlerts = 0;
bool gEverSeen = false;
uint32_t gLastSeenMs = 0;
time_t gLastSeenUnix = 0;  // 0 until the phone has sent the time

// ------------------------------------------------------------------ LED + beeps

void setLed(bool red, bool green, bool blue) {
  digitalWrite(Pin::LedRed, red ? LOW : HIGH);  // active LOW
  digitalWrite(Pin::LedGreen, green ? LOW : HIGH);
  digitalWrite(Pin::LedBlue, blue ? LOW : HIGH);
}

int gBeepsLeft = 0;
uint32_t gNextBeepMs = 0;

void startBeeps() {
  if (gMuted || gSettings.beepCount == 0) return;
  gBeepsLeft = gSettings.beepCount;
  gNextBeepMs = millis();
}

void serviceBeeps(uint32_t now) {
  if (gBeepsLeft <= 0 || (int32_t)(now - gNextBeepMs) < 0) return;
  tone(Pin::Speaker, gSettings.beepHz, 110);
  gBeepsLeft--;
  gNextBeepMs = now + 230;
}

// ------------------------------------------------------------------ touch + BOOT button

bool gTouchDown = false;
bool gHoldHandled = false;
uint32_t gTouchStartMs = 0;
uint32_t gLastTouchPollMs = 0;

void handleTouch(uint32_t now) {
  if (now - gLastTouchPollMs < 25) return;  // bit-banged reads take ~1 ms, so poll gently
  gLastTouchPollMs = now;

  bool pressed = gTouch.getTouch().zRaw >= kTouchPressure;
  if (pressed && !gTouchDown) {
    gTouchDown = true;
    gHoldHandled = false;
    gTouchStartMs = now;
  } else if (pressed && gTouchDown) {
    if (!gHoldHandled && now - gTouchStartMs >= 800) {  // hold: next brightness level
      gHoldHandled = true;
      gBrightnessIndex = (gBrightnessIndex + 1) % sizeof(kBrightness);
      screen::setBrightness(kBrightness[gBrightnessIndex]);
      gPrefs.putUChar("bright", gBrightnessIndex);
    }
  } else if (!pressed && gTouchDown) {
    gTouchDown = false;
    if (!gHoldHandled && now - gTouchStartMs >= 40) {  // tap: mute / unmute
      gMuted = !gMuted;
      if (gMuted) {
        gBeepsLeft = 0;
        noTone(Pin::Speaker);
      }
      gPrefs.putBool("muted", gMuted);
    }
  }
}

bool gBootWasDown = false;
uint32_t gBootChangedMs = 0;

void handleBootButton(uint32_t now) {
  bool down = digitalRead(Pin::BootButton) == LOW;
  if (down == gBootWasDown || now - gBootChangedMs < 50) return;  // debounce
  gBootWasDown = down;
  gBootChangedMs = now;
  if (down) {
    gInvert = !gInvert;
    screen::setInvert(gInvert);
    gPrefs.putBool("invert", gInvert);
  }
}

// ------------------------------------------------------------------ detections

void handleHit(const Hit& hit) {
  char message[80];
  int len = formatPhoneMessage(message, sizeof(message), hit);
  if (len < 0) return;
  if (len >= (int)sizeof(message)) len = sizeof(message) - 1;
  Serial.println(message);
  ble::notifyPhone(message, (size_t)len);

  if (gSettings.logToSd) sdcard::logHit(hit, millis() / 1000);

  gAlerts++;
  startBeeps();
}

screen::Status buildStatus() {
  screen::Status s = {};
  s.phoneConnected = ble::phoneConnected();
  s.muted = gMuted;
  sdcard::Status sd = sdcard::status();
  if (sd == sdcard::Status::Error) {
    s.sd = screen::SdBadge::Error;
  } else if (sd == sdcard::Status::Ready && gSettings.logToSd) {
    s.sd = screen::SdBadge::Logging;
  } else {
    s.sd = screen::SdBadge::None;
  }
  s.alerts = gAlerts;
  s.everSeen = gEverSeen;
  s.lastSeenMs = gLastSeenMs;
  struct tm local;
  if (gLastSeenUnix != 0 && phoneclock::localAt(gLastSeenUnix, local)) {
    formatClock12h(local, s.lastSeenClock, sizeof(s.lastSeenClock));
  }
  return s;
}

void printSummary(sdcard::Status sd, int configProblems) {
  Serial.println("\nRScan ready.");
  Serial.printf("SD card: %s\n", sd == sdcard::Status::Ready   ? "ready"
                                 : sd == sdcard::Status::Error ? "error"
                                                               : "none");
  if (configProblems) Serial.printf("config.txt: %d line(s) ignored (see above)\n", configProblems);
  Serial.printf("Logging: %s\n", (sd == sdcard::Status::Ready && gSettings.logToSd) ? "on (rscan/log.csv)" : "off");
  Serial.println("Watching for:");
  for (int i = 0; i < gSettings.watchCount; i++) {
    const WatchPrefix& w = gSettings.watch[i];
    Serial.print("  ");
    for (int b = 0; b < w.len; b++) Serial.printf("%s%02X", b ? ":" : "", w.bytes[b]);
    Serial.printf("  %s\n", w.label);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(Pin::LedRed, OUTPUT);
  pinMode(Pin::LedGreen, OUTPUT);
  pinMode(Pin::LedBlue, OUTPUT);
  setLed(false, false, false);
  pinMode(Pin::BootButton, INPUT_PULLUP);

  gPrefs.begin("rscan", false);
  gMuted = gPrefs.getBool("muted", false);
  gInvert = gPrefs.getBool("invert", false);
  gBrightnessIndex = gPrefs.getUChar("bright", 0) % sizeof(kBrightness);

  // SD card first: config.txt can change the screen rotation.
  sdcard::Status sd = sdcard::begin();
  int configProblems = (sd == sdcard::Status::Ready) ? sdcard::loadConfig(gSettings) : 0;
  ensureDefaultWatchList(gSettings);

  screen::begin(gSettings.rotation, gInvert, kBrightness[gBrightnessIndex]);
  const char* sdLine = "No SD card (optional): not logging";
  uint16_t sdColor = screen::dim();
  if (sd == sdcard::Status::Error) {
    sdLine = "SD card error: not logging";
    sdColor = screen::amber();
  } else if (sd == sdcard::Status::Ready) {
    sdLine = gSettings.logToSd ? "SD card: logging to rscan/log.csv" : "SD card: logging is off in config";
    sdColor = gSettings.logToSd ? screen::green() : screen::dim();
  }
  char configLine[48] = "";
  if (configProblems) {
    snprintf(configLine, sizeof(configLine), "config.txt: %d line%s ignored", configProblems,
             configProblems == 1 ? "" : "s");
  }
  screen::showSplash(sdLine, sdColor, configLine);

  gTouch.begin();
  gTouch.setCalibration(200, 3700, 240, 3800);  // only pressure is used, but keeps the math sane

  ble::begin(gSettings);
  printSummary(sd, configProblems);

  delay(configProblems ? 5000 : 2500);  // show the splash; detections queue up meanwhile

  uint32_t now = millis();
  screen::redrawAll(now, buildStatus(), ble::nearby(now));
}

void loop() {
  uint32_t now = millis();

  Hit hit;
  while (ble::nextHit(hit)) handleHit(hit);

  Nearby nearby = ble::nearby(now);
  if (nearby.count > 0) {
    gEverSeen = true;
    gLastSeenMs = now;
    gLastSeenUnix = phoneclock::synced() ? time(nullptr) : 0;
  }
  setLed(nearby.count > 0, false, false);

  handleTouch(now);
  handleBootButton(now);
  serviceBeeps(now);
  screen::update(now, buildStatus(), nearby);

  ble::keepScanning();
  delay(20);
}
