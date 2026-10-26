// Parsing and formatting helpers. Plain C++ with no Arduino dependencies, so they're
// unit-tested on a computer (see test/host_tests.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "types.h"

// "00:25:DF" (or 00-25-DF, 3 to 6 bytes) -> prefix. Returns false if malformed.
bool parseMacPrefix(const char* text, WatchPrefix& out);

// NimBLE stores addresses little-endian: addr[5] is the first byte you'd read in "00:25:DF:..".
bool matchesPrefix(const uint8_t* addrLE, const WatchPrefix& prefix);
void formatMac(const uint8_t* addrLE, char out[18]);

// Replaces commas and non-printable characters so names are safe for CSV and the screen font.
void sanitizeName(char* text);

enum class ConfigLine { Blank, Applied, Error };

// Applies one line of config.txt to `settings`. The first "watch" line replaces the built-in
// watch list. On error, writes a short reason to `why`.
ConfigLine applyConfigLine(const char* line, Settings& settings, bool& customWatchList, char* why,
                           size_t whyLen);

// Adds the default Axon prefix when the watch list is empty.
void ensureDefaultWatchList(Settings& settings);

// The commented config.txt written to a new SD card.
const char* defaultConfigText();

// CSV header and one row for rscan/log.csv. `local` is nullptr when the clock isn't set yet.
const char* logHeader();
int formatLogRow(char* out, size_t outLen, const Hit& hit, uint32_t uptimeSeconds, const struct tm* local);

// "AXON,<mac>,<rssi>,<name>" — the notification the iPhone app understands.
int formatPhoneMessage(char* out, size_t outLen, const Hit& hit);

// Parses "TIME,<unix seconds>,<UTC offset seconds>" written by the phone.
bool parseTimeSync(const char* text, int64_t& unixSeconds, int32_t& utcOffsetSeconds);

// "1:42 PM" from a local time.
void formatClock12h(const struct tm& local, char* out, size_t outLen);
