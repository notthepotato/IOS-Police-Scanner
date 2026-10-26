// SD card: settings file (rscan/config.txt) and detection log (rscan/log.csv).
// Everything works without a card; it just won't log or read custom settings.
#pragma once
#include <stdint.h>

#include "types.h"

namespace sdcard {

enum class Status { NoCard, Ready, Error };

// Mounts the card, creates rscan/ and a commented config.txt if missing.
Status begin();

Status status();

// Reads rscan/config.txt into `settings` (unknown or bad lines are reported over Serial).
// Returns how many lines had problems.
int loadConfig(Settings& settings);

// Appends one row to rscan/log.csv. Returns false if the write failed.
bool logHit(const Hit& hit, uint32_t uptimeSeconds);

}  // namespace sdcard
