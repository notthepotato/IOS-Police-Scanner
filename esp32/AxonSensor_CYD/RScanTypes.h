// Shared data types. These live in their own header because the Arduino IDE auto-generates
// function prototypes at the top of the sketch, above any struct defined in the .ino file.
#pragma once
#include <stdint.h>

// One new detection, passed from the Bluetooth task to loop().
struct Hit {
  char mac[18];
  int8_t rssi;
  char name[24];
};

// An Axon device seen recently.
struct Device {
  uint8_t addr[6];
  uint32_t lastMs;
  int8_t rssi;
  char name[24];
};

// Police gear in range right now: how many devices, and the strongest one.
struct Nearby {
  int count;
  int8_t rssi;
  char name[24];
};
