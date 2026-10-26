#include "sd_storage.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"
#include "phone_clock.h"
#include "text_utils.h"

namespace sdcard {
namespace {

SPIClass gSdSpi(VSPI);
Status gStatus = Status::NoCard;

const char* kDir = "/rscan";
const char* kConfigPath = "/rscan/config.txt";
const char* kLogPath = "/rscan/log.csv";
const char* kOldLogPath = "/rscan/log-old.csv";
constexpr size_t kMaxLogBytes = 4 * 1024 * 1024;  // then log.csv becomes log-old.csv

}  // namespace

Status begin() {
  gSdSpi.begin(Pin::SdSck, Pin::SdMiso, Pin::SdMosi, Pin::SdCs);
  if (!SD.begin(Pin::SdCs, gSdSpi, 4000000) || SD.cardType() == CARD_NONE) {
    gStatus = Status::NoCard;
    return gStatus;
  }
  if (!SD.exists(kDir) && !SD.mkdir(kDir)) {
    gStatus = Status::Error;
    return gStatus;
  }
  if (!SD.exists(kConfigPath)) {
    File f = SD.open(kConfigPath, FILE_WRITE);
    if (f) {
      f.print(defaultConfigText());
      f.close();
    }
  }
  gStatus = Status::Ready;
  return gStatus;
}

Status status() { return gStatus; }

int loadConfig(Settings& settings) {
  if (gStatus != Status::Ready) return 0;
  File f = SD.open(kConfigPath, FILE_READ);
  if (!f) return 0;

  bool customWatchList = false;
  int problems = 0;
  int lineNumber = 0;
  char line[128];
  while (f.available()) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    lineNumber++;
    char why[48] = "";
    if (applyConfigLine(line, settings, customWatchList, why, sizeof(why)) == ConfigLine::Error) {
      problems++;
      Serial.printf("config.txt line %d ignored: %s\n", lineNumber, why);
    }
  }
  f.close();
  return problems;
}

bool logHit(const Hit& hit, uint32_t uptimeSeconds) {
  if (gStatus != Status::Ready) return false;

  File existing = SD.open(kLogPath, FILE_READ);
  bool isNew = !existing;
  if (existing) {
    bool full = existing.size() > kMaxLogBytes;
    existing.close();
    if (full) {
      SD.remove(kOldLogPath);
      SD.rename(kLogPath, kOldLogPath);
      isNew = true;
    }
  }

  File f = SD.open(kLogPath, FILE_APPEND);
  if (!f) {
    gStatus = Status::Error;
    return false;
  }
  if (isNew) f.print(logHeader());

  struct tm local;
  bool haveClock = phoneclock::localNow(local);
  char row[160];
  formatLogRow(row, sizeof(row), hit, uptimeSeconds, haveClock ? &local : nullptr);
  size_t written = f.print(row);
  f.close();  // close every time so a power cut in the car loses at most one row
  if (written == 0) {
    gStatus = Status::Error;
    return false;
  }
  return true;
}

}  // namespace sdcard
