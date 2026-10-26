#include "screen.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <SPI.h>

#include "config.h"

namespace screen {
namespace {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t C_BG = rgb(0, 0, 0);
constexpr uint16_t C_BAR = rgb(28, 32, 40);
constexpr uint16_t C_TEXT = rgb(255, 255, 255);
constexpr uint16_t C_DIM = rgb(140, 146, 158);
constexpr uint16_t C_GREEN = rgb(64, 220, 140);
constexpr uint16_t C_AMBER = rgb(255, 176, 32);
constexpr uint16_t C_ALERT = rgb(180, 16, 24);
constexpr uint16_t C_ALERT_DIM = rgb(110, 40, 44);

// Layout (320 x 240): header 0-35, main 36-199, footer 200-239
constexpr int HEADER_H = 36, MAIN_Y = 36, MAIN_H = 164, FOOTER_Y = 200;

SPIClass gTftSpi(HSPI);
Adafruit_ILI9341 tft(&gTftSpi, Pin::TftDc, Pin::TftCs, -1);

// What's on screen now, so only changes get redrawn.
int shownHeader = -1;
int shownAlerts = -1;
int shownMode = -1;  // 0 = all clear, 1 = police gear
char shownLastSeen[48] = "";
int shownBars = -1, shownCount = -1, shownRssi = 1000;
char shownName[24] = "";
uint32_t lastDetailDrawMs = 0;

void drawText(const char* text, int16_t x, int16_t baseline, const GFXfont* font, uint16_t color) {
  tft.setFont(font);
  tft.setTextColor(color);
  tft.setCursor(x, baseline);
  tft.print(text);
}

int16_t textWidth(const char* text, const GFXfont* font) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setFont(font);
  tft.getTextBounds(text, 0, 100, &x1, &y1, &w, &h);
  return (int16_t)w + x1;
}

void drawCentered(const char* text, int16_t baseline, const GFXfont* font, uint16_t color) {
  int16_t x = (tft.width() - textWidth(text, font)) / 2;
  drawText(text, x < 0 ? 0 : x, baseline, font, color);
}

void drawRight(const char* text, int16_t right, int16_t baseline, const GFXfont* font, uint16_t color) {
  drawText(text, right - textWidth(text, font), baseline, font, color);
}

const char* rangeLabel(int rssi) {
  if (rssi >= -60) return "VERY CLOSE";
  if (rssi >= -75) return "CLOSE";
  if (rssi >= -88) return "NEARBY";
  return "IN RANGE";
}

int signalBars(int rssi) {
  if (rssi >= -60) return 5;
  if (rssi >= -68) return 4;
  if (rssi >= -76) return 3;
  if (rssi >= -84) return 2;
  return 1;
}

int headerKey(const Status& s) {
  return (s.phoneConnected ? 1 : 0) | (s.muted ? 2 : 0) | ((int)s.sd << 2);
}

void drawHeader(const Status& s) {
  tft.fillRect(0, 0, tft.width(), HEADER_H, C_BAR);
  drawText("RScan", 10, 25, &FreeSansBold12pt7b, C_TEXT);
  int16_t badgeX = 10 + textWidth("RScan", &FreeSansBold12pt7b) + 12;
  if (s.sd == SdBadge::Logging) drawText("SD", badgeX, 24, &FreeSans9pt7b, C_GREEN);
  if (s.sd == SdBadge::Error) drawText("SD!", badgeX, 24, &FreeSans9pt7b, C_AMBER);
  tft.fillCircle(tft.width() - 16, 18, 6, s.phoneConnected ? C_GREEN : C_DIM);
  const char* phoneText = s.phoneConnected ? "Phone" : "No phone";
  int16_t phoneX = tft.width() - 30 - textWidth(phoneText, &FreeSans9pt7b);
  drawText(phoneText, phoneX, 24, &FreeSans9pt7b, s.phoneConnected ? C_TEXT : C_DIM);
  if (s.muted) drawRight("MUTED", phoneX - 14, 24, &FreeSans9pt7b, C_AMBER);
  shownHeader = headerKey(s);
}

void drawFooter(const Status& s) {
  tft.fillRect(0, FOOTER_Y, tft.width(), tft.height() - FOOTER_Y, C_BAR);
  char text[24];
  snprintf(text, sizeof(text), "Alerts: %d", s.alerts);
  drawText(text, 10, 226, &FreeSans9pt7b, C_TEXT);
  drawRight("Tap: mute  Hold: dim", tft.width() - 10, 226, &FreeSans9pt7b, C_DIM);
  shownAlerts = s.alerts;
}

void lastSeenText(uint32_t now, const Status& s, char* out, size_t outLen) {
  if (!s.everSeen) {
    snprintf(out, outLen, "No police gear seen yet");
    return;
  }
  uint32_t minutes = (now - s.lastSeenMs) / 60000;
  char ago[20];
  if (minutes == 0) {
    snprintf(ago, sizeof(ago), "just now");
  } else if (minutes < 60) {
    snprintf(ago, sizeof(ago), "%lu min ago", (unsigned long)minutes);
  } else {
    snprintf(ago, sizeof(ago), "%lu hr ago", (unsigned long)(minutes / 60));
  }
  if (s.lastSeenClock[0]) {
    snprintf(out, outLen, "Last seen %s (%s)", s.lastSeenClock, ago);
  } else {
    snprintf(out, outLen, "Last seen %s", ago);
  }
}

void drawLastSeen(const char* text) {
  tft.fillRect(0, 152, tft.width(), 30, C_BG);
  drawCentered(text, 172, &FreeSans9pt7b, C_DIM);
  strlcpy(shownLastSeen, text, sizeof(shownLastSeen));
}

void drawIdle(uint32_t now, const Status& s) {
  tft.fillRect(0, MAIN_Y, tft.width(), MAIN_H, C_BG);
  drawCentered("ALL CLEAR", 110, &FreeSansBold24pt7b, C_GREEN);
  drawCentered("Scanning for police gear", 140, &FreeSans9pt7b, C_TEXT);
  char text[48];
  lastSeenText(now, s, text, sizeof(text));
  drawLastSeen(text);
  shownMode = 0;
}

void drawAlertDetails(const Nearby& n) {
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

  // "Axon Body 4, -58 dBm  (+1 more)", shortening the name until it fits.
  char name[28];
  if (n.name[0]) {
    strlcpy(name, n.name, sizeof(name));
  } else {
    snprintf(name, sizeof(name), "%s device", n.label[0] ? n.label : "Unknown");
  }
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

void drawAlert(const Nearby& n) {
  tft.fillRect(0, MAIN_Y, tft.width(), MAIN_H, C_ALERT);
  drawCentered("POLICE GEAR", 84, &FreeSansBold18pt7b, C_TEXT);
  drawAlertDetails(n);
  shownMode = 1;
}

}  // namespace

void begin(uint8_t rotation, bool invertColors, uint8_t brightness) {
  gTftSpi.begin(Pin::TftSck, Pin::TftMiso, Pin::TftMosi, Pin::TftCs);
  tft.begin();
  tft.setRotation(rotation);
  tft.invertDisplay(invertColors);
  ledcAttach(Pin::TftBacklight, 5000, 8);
  setBrightness(brightness);
}

void setInvert(bool invertColors) { tft.invertDisplay(invertColors); }

void setBrightness(uint8_t brightness) { ledcWrite(Pin::TftBacklight, brightness); }

void showSplash(const char* sdLine, uint16_t sdLineColor565, const char* configLine) {
  tft.fillScreen(C_BG);
  drawCentered("RScan", 70, &FreeSansBold24pt7b, C_GREEN);
  drawCentered("Police-gear sensor for Road Scanner", 102, &FreeSans9pt7b, C_TEXT);
  drawCentered(sdLine, 134, &FreeSans9pt7b, sdLineColor565);
  if (configLine && configLine[0]) drawCentered(configLine, 156, &FreeSans9pt7b, C_AMBER);
  drawCentered("Tap: mute     Hold: brightness", 192, &FreeSans9pt7b, C_DIM);
  drawCentered("Colors look wrong? Press BOOT", 216, &FreeSans9pt7b, C_DIM);
}

void redrawAll(uint32_t nowMs, const Status& s, const Nearby& n) {
  tft.fillScreen(C_BG);
  drawHeader(s);
  drawFooter(s);
  if (n.count > 0) {
    drawAlert(n);
  } else {
    drawIdle(nowMs, s);
  }
}

void update(uint32_t nowMs, const Status& s, const Nearby& n) {
  if (headerKey(s) != shownHeader) drawHeader(s);
  if (s.alerts != shownAlerts) drawFooter(s);

  if (n.count > 0) {
    if (shownMode != 1) {
      drawAlert(n);
    } else {
      bool changed = signalBars(n.rssi) != shownBars || n.count != shownCount || strcmp(n.name, shownName) != 0 ||
                     abs(n.rssi - shownRssi) >= 3;
      if (changed && millis() - lastDetailDrawMs >= 500) drawAlertDetails(n);  // max 2 redraws/second
    }
  } else if (shownMode != 0) {
    drawIdle(nowMs, s);
  } else {
    char text[48];
    lastSeenText(nowMs, s, text, sizeof(text));
    if (strcmp(text, shownLastSeen) != 0) drawLastSeen(text);
  }
}

uint16_t green() { return C_GREEN; }
uint16_t amber() { return C_AMBER; }
uint16_t dim() { return C_DIM; }

}  // namespace screen
