// The 2.8" 320x240 display: header, main status area, footer.
#pragma once
#include <stdint.h>

#include "types.h"

namespace screen {

enum class SdBadge { None, Logging, Error };

struct Status {
  bool phoneConnected;
  bool muted;
  SdBadge sd;
  int alerts;
  bool everSeen;
  uint32_t lastSeenMs;
  char lastSeenClock[12];  // "1:42 PM", or empty when the phone hasn't sent the time
};

void begin(uint8_t rotation, bool invertColors, uint8_t brightness);
void setInvert(bool invertColors);
void setBrightness(uint8_t brightness);

// Start-up screen. `sdLine` describes the SD card state.
void showSplash(const char* sdLine, uint16_t sdLineColor565, const char* configLine);

// Full redraw, then cheap partial updates (only what changed is redrawn, so no flicker).
void redrawAll(uint32_t nowMs, const Status& status, const Nearby& nearby);
void update(uint32_t nowMs, const Status& status, const Nearby& nearby);

// Shows a short message in the footer (e.g. "Speaker test") for `durationMs`.
void showMessage(const char* text, uint32_t nowMs, uint32_t durationMs);

// Colors used outside the screen module.
uint16_t green();
uint16_t amber();
uint16_t dim();

}  // namespace screen
