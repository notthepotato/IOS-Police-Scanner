// Data types shared between the modules. Plain C++ (no Arduino headers), so the parsing and
// formatting code can also be unit-tested on a computer.
#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr int kMaxWatch = 8;

// A Bluetooth address prefix to watch for, e.g. 00:25:DF (Axon).
struct WatchPrefix {
  uint8_t bytes[6];
  uint8_t len;      // 3 to 6 bytes
  char label[20];   // shown on screen and in the log, e.g. "Axon"
};

// Settings: defaults here, optionally overridden by rscan/config.txt on the SD card.
struct Settings {
  uint8_t rotation = 1;         // 1 = landscape, 3 = upside down
  uint16_t beepHz = 2600;
  uint8_t beepCount = 3;        // beeps per new detection, 0 = silent
  uint32_t activeMs = 20000;    // a device counts as "still here" this long after its last signal
  uint32_t forgetMs = 120000;   // gone this long -> alerts again when it comes back
  bool logToSd = true;
  WatchPrefix watch[kMaxWatch] = {};
  uint8_t watchCount = 0;
};

// One new detection, passed from the Bluetooth task to the main loop.
struct Hit {
  char mac[18];
  int8_t rssi;
  char name[24];   // advertised name, may be empty
  char label[20];  // which watch-list entry matched
};

// A watched device seen recently.
struct Device {
  uint8_t addr[6];
  uint32_t lastMs;
  int8_t rssi;
  char name[24];
  char label[20];
};

// Police gear in range right now: how many devices, and the strongest one.
struct Nearby {
  int count;
  int8_t rssi;
  char name[24];
  char label[20];
};
