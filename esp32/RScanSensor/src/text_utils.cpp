#include "text_utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

void copyText(char* dst, size_t dstLen, const char* src) {
  if (dstLen == 0) return;
  size_t n = strlen(src);
  if (n >= dstLen) n = dstLen - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Trims spaces/tabs/CR/LF from both ends, in place. Returns the start.
char* trim(char* s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char* end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
  return s;
}

bool parseLong(const char* text, long minValue, long maxValue, long& out) {
  errno = 0;
  char* end = nullptr;
  long v = strtol(text, &end, 10);
  if (errno != 0 || end == text || *trim(end) != '\0') return false;
  if (v < minValue || v > maxValue) return false;
  out = v;
  return true;
}

}  // namespace

bool parseMacPrefix(const char* text, WatchPrefix& out) {
  WatchPrefix p = {};
  const char* s = text;
  while (*s) {
    int hi = hexValue(s[0]);
    int lo = s[1] ? hexValue(s[1]) : -1;
    if (hi < 0 || lo < 0 || p.len >= 6) return false;
    p.bytes[p.len++] = (uint8_t)(hi * 16 + lo);
    s += 2;
    if (*s == ':' || *s == '-') {
      s++;
      if (*s == '\0') return false;  // trailing separator
    } else if (*s != '\0' && hexValue(*s) < 0) {
      return false;  // allows "0025DF" as well as "00:25:DF"
    }
  }
  if (p.len < 3) return false;
  out = p;
  return true;
}

bool matchesPrefix(const uint8_t* addrLE, const WatchPrefix& prefix) {
  for (int i = 0; i < prefix.len; i++) {
    if (addrLE[5 - i] != prefix.bytes[i]) return false;
  }
  return true;
}

void formatMac(const uint8_t* a, char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

void sanitizeName(char* text) {
  for (char* p = text; *p; p++) {
    if (*p == ',' || *p == '"' || (unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) *p = ' ';
  }
  char* start = trim(text);
  if (start != text) memmove(text, start, strlen(start) + 1);
}

ConfigLine applyConfigLine(const char* rawLine, Settings& s, bool& customWatchList, char* why, size_t whyLen) {
  char buf[128];
  copyText(buf, sizeof(buf), rawLine);
  char* hash = strchr(buf, '#');
  if (hash) *hash = '\0';
  char* line = trim(buf);
  if (*line == '\0') return ConfigLine::Blank;

  char* eq = strchr(line, '=');
  if (!eq) {
    copyText(why, whyLen, "expected key = value");
    return ConfigLine::Error;
  }
  *eq = '\0';
  char* key = trim(line);
  char* value = trim(eq + 1);
  for (char* p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
  long n = 0;

  if (strcmp(key, "beep_hz") == 0) {
    if (!parseLong(value, 200, 8000, n)) { copyText(why, whyLen, "beep_hz must be 200-8000"); return ConfigLine::Error; }
    s.beepHz = (uint16_t)n;
  } else if (strcmp(key, "beeps") == 0) {
    if (!parseLong(value, 0, 10, n)) { copyText(why, whyLen, "beeps must be 0-10"); return ConfigLine::Error; }
    s.beepCount = (uint8_t)n;
  } else if (strcmp(key, "still_here_seconds") == 0) {
    if (!parseLong(value, 5, 600, n)) { copyText(why, whyLen, "still_here_seconds must be 5-600"); return ConfigLine::Error; }
    s.activeMs = (uint32_t)n * 1000;
  } else if (strcmp(key, "realert_minutes") == 0) {
    if (!parseLong(value, 1, 120, n)) { copyText(why, whyLen, "realert_minutes must be 1-120"); return ConfigLine::Error; }
    s.forgetMs = (uint32_t)n * 60000;
  } else if (strcmp(key, "rotation") == 0) {
    if (!parseLong(value, 1, 3, n) || n == 2) { copyText(why, whyLen, "rotation must be 1 or 3"); return ConfigLine::Error; }
    s.rotation = (uint8_t)n;
  } else if (strcmp(key, "log") == 0) {
    for (char* p = value; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strcmp(value, "on") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
      s.logToSd = true;
    } else if (strcmp(value, "off") == 0 || strcmp(value, "no") == 0 || strcmp(value, "0") == 0) {
      s.logToSd = false;
    } else {
      copyText(why, whyLen, "log must be on or off");
      return ConfigLine::Error;
    }
  } else if (strcmp(key, "watch") == 0) {
    // "00:25:DF Axon" -> prefix + label (label optional)
    char* space = value;
    while (*space && !isspace((unsigned char)*space)) space++;
    char* label = space;
    if (*space) {
      *space = '\0';
      label = trim(space + 1);
    }
    WatchPrefix p;
    if (!parseMacPrefix(value, p)) { copyText(why, whyLen, "watch needs a prefix like 00:25:DF"); return ConfigLine::Error; }
    if (!customWatchList) {  // first watch line replaces the built-in list
      customWatchList = true;
      s.watchCount = 0;
    }
    if (s.watchCount >= kMaxWatch) { copyText(why, whyLen, "too many watch lines (max 8)"); return ConfigLine::Error; }
    copyText(p.label, sizeof(p.label), *label ? label : "Watched device");
    sanitizeName(p.label);
    s.watch[s.watchCount++] = p;
  } else {
    copyText(why, whyLen, "unknown setting");
    return ConfigLine::Error;
  }
  return ConfigLine::Applied;
}

void ensureDefaultWatchList(Settings& s) {
  if (s.watchCount > 0) return;
  WatchPrefix axon = {};
  parseMacPrefix("00:25:DF", axon);
  copyText(axon.label, sizeof(axon.label), "Axon");
  s.watch[0] = axon;
  s.watchCount = 1;
}

const char* defaultConfigText() {
  return "# RScan settings. Edit this file on your computer, put the card back in, then\n"
         "# restart the board (unplug it or press RST). Lines starting with # are ignored.\n"
         "\n"
         "# Beep pitch in Hz, and beeps per new detection (0 = silent).\n"
         "# Beeps need a small speaker on the 2-pin SPEAK connector.\n"
         "beep_hz = 2600\n"
         "beeps = 3\n"
         "\n"
         "# Seconds a device still counts as \"in range\" after its last signal.\n"
         "still_here_seconds = 20\n"
         "\n"
         "# Minutes a device has to be gone before it alerts again.\n"
         "realert_minutes = 2\n"
         "\n"
         "# Screen orientation: 1 = landscape, 3 = upside down.\n"
         "rotation = 1\n"
         "\n"
         "# Save every detection to rscan/log.csv on this card (on / off).\n"
         "log = on\n"
         "\n"
         "# Bluetooth address prefixes to watch for, each with a label.\n"
         "# Axon (body cams, Tasers, in-car systems) is the default. You can list up to 8.\n"
         "# Broad prefixes (phones, headphones, cars) will cause constant false alarms.\n"
         "watch = 00:25:DF Axon\n";
}

const char* logHeader() {
  return "date,time,uptime_s,mac,rssi,name,watch\n";
}

int formatLogRow(char* out, size_t outLen, const Hit& hit, uint32_t uptimeSeconds, const struct tm* local) {
  char date[16] = "";
  char clock[16] = "";
  if (local) {
    strftime(date, sizeof(date), "%Y-%m-%d", local);
    strftime(clock, sizeof(clock), "%H:%M:%S", local);
  }
  return snprintf(out, outLen, "%s,%s,%lu,%s,%d,%s,%s\n", date, clock, (unsigned long)uptimeSeconds, hit.mac,
                  (int)hit.rssi, hit.name, hit.label);
}

int formatPhoneMessage(char* out, size_t outLen, const Hit& hit) {
  return snprintf(out, outLen, "AXON,%s,%d,%s", hit.mac, (int)hit.rssi, hit.name[0] ? hit.name : hit.label);
}

bool parseTimeSync(const char* text, int64_t& unixSeconds, int32_t& utcOffsetSeconds) {
  if (strncmp(text, "TIME,", 5) != 0) return false;
  const char* p = text + 5;
  char* end = nullptr;
  errno = 0;
  long long t = strtoll(p, &end, 10);
  if (errno != 0 || end == p || *end != ',') return false;
  p = end + 1;
  long off = strtol(p, &end, 10);
  if (errno != 0 || end == p || (*end != '\0' && *end != '\n' && *end != '\r')) return false;
  if (t < 1600000000LL || t > 4102444800LL) return false;  // 2020..2100: rejects garbage
  if (off < -14 * 3600 || off > 14 * 3600) return false;
  unixSeconds = t;
  utcOffsetSeconds = (int32_t)off;
  return true;
}

void formatClock12h(const struct tm& local, char* out, size_t outLen) {
  int hour = local.tm_hour % 12;
  if (hour == 0) hour = 12;
  snprintf(out, outLen, "%d:%02d %s", hour, local.tm_min, local.tm_hour < 12 ? "AM" : "PM");
}
