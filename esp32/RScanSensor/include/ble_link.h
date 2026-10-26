// Bluetooth: scans for watched police gear, and runs the link to the iPhone app.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "types.h"

namespace ble {

// Starts the GATT server the phone connects to, and a continuous passive scan.
// `settings` must stay alive (it holds the watch list).
void begin(const Settings& settings);

bool phoneConnected();

// Next new detection, if any (a new device, or one back after `forgetMs`).
bool nextHit(Hit& out);

// Devices heard within `activeMs`, with the strongest one.
Nearby nearby(uint32_t nowMs);

// Sends a detection message to the phone (if connected).
void notifyPhone(const char* message, size_t length);

// Restarts the scan if the radio stopped it.
void keepScanning();

}  // namespace ble
