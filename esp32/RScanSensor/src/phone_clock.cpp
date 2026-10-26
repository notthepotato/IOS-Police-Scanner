#include "phone_clock.h"

#include <atomic>
#include <sys/time.h>

namespace phoneclock {
namespace {
std::atomic<bool> gSynced{false};
std::atomic<int32_t> gOffset{0};
}  // namespace

void set(int64_t unixSeconds, int32_t utcOffsetSeconds) {
  struct timeval tv;
  tv.tv_sec = (time_t)unixSeconds;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  gOffset = utcOffsetSeconds;
  gSynced = true;
}

bool synced() { return gSynced; }

bool localAt(time_t unixSeconds, struct tm& out) {
  if (!gSynced) return false;
  time_t local = unixSeconds + gOffset.load();
  return gmtime_r(&local, &out) != nullptr;
}

bool localNow(struct tm& out) { return localAt(time(nullptr), out); }

}  // namespace phoneclock
