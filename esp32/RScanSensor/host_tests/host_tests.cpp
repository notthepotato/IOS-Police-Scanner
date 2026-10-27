// Unit tests for the parsing/formatting code (src/text_utils.cpp). They run on your computer,
// not the board. From the RScanSensor folder:
//
//   g++ -std=c++17 -Iinclude host_tests/host_tests.cpp src/text_utils.cpp -o host_tests/run && host_tests/run
#include <stdio.h>
#include <string.h>

#include "text_utils.h"

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
      failures++;                                                     \
    }                                                                 \
  } while (0)

static void testMacPrefix() {
  WatchPrefix p;
  CHECK(parseMacPrefix("00:25:DF", p) && p.len == 3 && p.bytes[0] == 0x00 && p.bytes[1] == 0x25 && p.bytes[2] == 0xDF);
  CHECK(parseMacPrefix("00-25-df", p) && p.len == 3 && p.bytes[2] == 0xDF);
  CHECK(parseMacPrefix("0025DF", p) && p.len == 3);
  CHECK(parseMacPrefix("AA:BB:CC:DD:EE:FF", p) && p.len == 6);
  CHECK(!parseMacPrefix("00:25", p));           // too short
  CHECK(!parseMacPrefix("00:25:DF:", p));       // trailing separator
  CHECK(!parseMacPrefix("00:25:GG", p));        // not hex
  CHECK(!parseMacPrefix("0:25:DF", p));         // half byte
  CHECK(!parseMacPrefix("AA:BB:CC:DD:EE:FF:00", p));  // too long

  // NimBLE little-endian address of 00:25:DF:12:34:56
  const uint8_t axon[6] = {0x56, 0x34, 0x12, 0xDF, 0x25, 0x00};
  const uint8_t other[6] = {0x56, 0x34, 0x12, 0xDF, 0x25, 0x01};
  parseMacPrefix("00:25:DF", p);
  CHECK(matchesPrefix(axon, p));
  CHECK(!matchesPrefix(other, p));
  char mac[18];
  formatMac(axon, mac);
  CHECK(strcmp(mac, "00:25:DF:12:34:56") == 0);
}

static void testSanitize() {
  char a[32] = "  Axon,Body\t4\"  ";
  sanitizeName(a);
  CHECK(strcmp(a, "Axon Body 4") == 0);
  char b[8] = "\xC3\xA9x";  // UTF-8 é + x
  sanitizeName(b);
  CHECK(strcmp(b, "x") == 0);
}

static void testConfig() {
  Settings s;
  bool custom = false;
  char why[48];

  CHECK(applyConfigLine("", s, custom, why, sizeof(why)) == ConfigLine::Blank);
  CHECK(applyConfigLine("   # just a comment", s, custom, why, sizeof(why)) == ConfigLine::Blank);
  CHECK(applyConfigLine("beep_hz = 3000   # higher", s, custom, why, sizeof(why)) == ConfigLine::Applied && s.beepHz == 3000);
  CHECK(applyConfigLine("BEEPS=0\r", s, custom, why, sizeof(why)) == ConfigLine::Applied && s.beepCount == 0);
  CHECK(applyConfigLine("still_here_seconds = 45", s, custom, why, sizeof(why)) == ConfigLine::Applied && s.activeMs == 45000);
  CHECK(applyConfigLine("realert_minutes = 5", s, custom, why, sizeof(why)) == ConfigLine::Applied && s.forgetMs == 300000);
  CHECK(applyConfigLine("rotation = 3", s, custom, why, sizeof(why)) == ConfigLine::Applied && s.rotation == 3);
  CHECK(applyConfigLine("log = OFF", s, custom, why, sizeof(why)) == ConfigLine::Applied && !s.logToSd);

  CHECK(applyConfigLine("rotation = 2", s, custom, why, sizeof(why)) == ConfigLine::Error && s.rotation == 3);
  CHECK(applyConfigLine("beep_hz = loud", s, custom, why, sizeof(why)) == ConfigLine::Error && s.beepHz == 3000);
  CHECK(applyConfigLine("beep_hz = 99999", s, custom, why, sizeof(why)) == ConfigLine::Error);
  CHECK(applyConfigLine("beeps = 3x", s, custom, why, sizeof(why)) == ConfigLine::Error);
  CHECK(applyConfigLine("volume = 11", s, custom, why, sizeof(why)) == ConfigLine::Error && strcmp(why, "unknown setting") == 0);
  CHECK(applyConfigLine("no equals sign", s, custom, why, sizeof(why)) == ConfigLine::Error);

  // Watch list: no lines -> default Axon
  Settings d;
  ensureDefaultWatchList(d);
  CHECK(d.watchCount == 1 && strcmp(d.watch[0].label, "Axon") == 0 && d.watch[0].bytes[2] == 0xDF);

  // First watch line replaces the default; label optional
  Settings w;
  bool wc = false;
  ensureDefaultWatchList(w);
  CHECK(applyConfigLine("watch = 00:25:DF Axon gear", w, wc, why, sizeof(why)) == ConfigLine::Applied);
  CHECK(applyConfigLine("watch = AA:BB:CC", w, wc, why, sizeof(why)) == ConfigLine::Applied);
  CHECK(w.watchCount == 2 && strcmp(w.watch[0].label, "Axon gear") == 0 && strcmp(w.watch[1].label, "Watched device") == 0);
  CHECK(applyConfigLine("watch = nope", w, wc, why, sizeof(why)) == ConfigLine::Error && w.watchCount == 2);
  for (int i = 0; i < 6; i++) applyConfigLine("watch = 11:22:33 x", w, wc, why, sizeof(why));
  CHECK(w.watchCount == 8);
  CHECK(applyConfigLine("watch = 11:22:33 x", w, wc, why, sizeof(why)) == ConfigLine::Error);

  // The default config file parses cleanly line by line
  Settings fromDefault;
  bool dc = false;
  int problems = 0;
  char line[200];
  const char* text = defaultConfigText();
  while (*text) {
    const char* nl = strchr(text, '\n');
    size_t len = nl ? (size_t)(nl - text) : strlen(text);
    memcpy(line, text, len);
    line[len] = '\0';
    if (applyConfigLine(line, fromDefault, dc, why, sizeof(why)) == ConfigLine::Error) {
      printf("default config line failed: '%s' (%s)\n", line, why);
      problems++;
    }
    text += len + (nl ? 1 : 0);
  }
  ensureDefaultWatchList(fromDefault);
  Settings defaults;
  CHECK(problems == 0);
  CHECK(fromDefault.beepHz == defaults.beepHz && fromDefault.beepCount == defaults.beepCount);
  CHECK(fromDefault.activeMs == defaults.activeMs && fromDefault.forgetMs == defaults.forgetMs);
  CHECK(fromDefault.rotation == defaults.rotation && fromDefault.logToSd == defaults.logToSd);
  CHECK(fromDefault.watchCount == 1 && strcmp(fromDefault.watch[0].label, "Axon") == 0);
}

static void testLogAndMessages() {
  Hit hit = {};
  strcpy(hit.mac, "00:25:DF:12:34:56");
  hit.rssi = -58;
  strcpy(hit.name, "Axon Body 4");
  strcpy(hit.label, "Axon");

  char row[160];
  struct tm t = {};
  t.tm_year = 2026 - 1900;
  t.tm_mon = 9;
  t.tm_mday = 26;
  t.tm_hour = 13;
  t.tm_min = 42;
  t.tm_sec = 5;
  formatLogRow(row, sizeof(row), hit, 5234, &t);
  CHECK(strcmp(row, "2026-10-26,13:42:05,5234,00:25:DF:12:34:56,-58,Axon Body 4,Axon\n") == 0);
  formatLogRow(row, sizeof(row), hit, 61, nullptr);
  CHECK(strcmp(row, ",,61,00:25:DF:12:34:56,-58,Axon Body 4,Axon\n") == 0);
  CHECK(strcmp(logHeader(), "date,time,uptime_s,mac,rssi,name,watch\n") == 0);

  char msg[80];
  formatPhoneMessage(msg, sizeof(msg), hit);
  CHECK(strcmp(msg, "AXON,00:25:DF:12:34:56,-58,Axon Body 4") == 0);
  hit.name[0] = '\0';
  formatPhoneMessage(msg, sizeof(msg), hit);
  CHECK(strcmp(msg, "AXON,00:25:DF:12:34:56,-58,Axon") == 0);  // label when the device has no name
}

static void testTimeSync() {
  int64_t unixSeconds = 0;
  int32_t offset = 0;
  CHECK(parseTimeSync("TIME,1793046459,-25200", unixSeconds, offset) && unixSeconds == 1793046459LL && offset == -25200);
  CHECK(parseTimeSync("TIME,1793046459,0\n", unixSeconds, offset) && offset == 0);
  CHECK(!parseTimeSync("TIME,abc,0", unixSeconds, offset));
  CHECK(!parseTimeSync("TIME,1793046459", unixSeconds, offset));
  CHECK(!parseTimeSync("TIME,12,0", unixSeconds, offset));                  // before 2020
  CHECK(!parseTimeSync("TIME,1793046459,99999", unixSeconds, offset));     // offset > 14 h
  CHECK(!parseTimeSync("TIME,1793046459,-25200x", unixSeconds, offset));
  CHECK(!parseTimeSync("HELLO", unixSeconds, offset));

  struct tm t = {};
  char clock[12];
  t.tm_hour = 0; t.tm_min = 5;
  formatClock12h(t, clock, sizeof(clock));
  CHECK(strcmp(clock, "12:05 AM") == 0);
  t.tm_hour = 13; t.tm_min = 42;
  formatClock12h(t, clock, sizeof(clock));
  CHECK(strcmp(clock, "1:42 PM") == 0);
  t.tm_hour = 12; t.tm_min = 0;
  formatClock12h(t, clock, sizeof(clock));
  CHECK(strcmp(clock, "12:00 PM") == 0);
}

static void testCommands() {
  CHECK(parseCommand("TEST,SPEAKER") == Command::SpeakerTest);
  CHECK(parseCommand("test,speaker\r\n") == Command::SpeakerTest);
  CHECK(parseCommand("  TEST,SPEAKER ") == Command::SpeakerTest);
  CHECK(parseCommand("TEST,SPEAKERS") == Command::None);
  CHECK(parseCommand("TIME,1793046459,0") == Command::None);
  CHECK(parseCommand("") == Command::None);
}

int main() {
  testCommands();
  testMacPrefix();
  testSanitize();
  testConfig();
  testLogAndMessages();
  testTimeSync();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("All host tests passed\n");
  return 0;
}
