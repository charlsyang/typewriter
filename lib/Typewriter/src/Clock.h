// Local date and time from the board's RTC chip, which keeps UTC while the
// board is off. The time zone is a POSIX TZ string saved in settings.
#pragma once

#include <Inkplate.h>
#include <time.h>

namespace tw {

namespace wallclock {

// Reads the RTC into the system clock and applies the saved time zone.
void begin(Inkplate &display);

// True once the RTC has been set (it loses time only without power and battery).
bool isSet();

// Sets the RTC and system clock to a UTC epoch, and saves the time zone.
void set(Inkplate &display, uint32_t epoch, const char *tz);

// Today's local date as "YYYY-MM-DD" (buffer of at least 11 bytes).
void today(char *out);

} // namespace wallclock

} // namespace tw
