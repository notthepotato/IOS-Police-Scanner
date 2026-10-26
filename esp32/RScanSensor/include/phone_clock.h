// Real date and time, sent by the iPhone app when it connects (the board has no clock battery).
#pragma once
#include <stdint.h>
#include <time.h>

namespace phoneclock {

// Called from the Bluetooth task when the phone writes the time.
void set(int64_t unixSeconds, int32_t utcOffsetSeconds);

// True once the phone has sent the time since power-on.
bool synced();

// Current local time. Returns false (and leaves `out` alone) until synced.
bool localNow(struct tm& out);

// Local time for a given Unix time. Returns false until synced.
bool localAt(time_t unixSeconds, struct tm& out);

}  // namespace phoneclock
