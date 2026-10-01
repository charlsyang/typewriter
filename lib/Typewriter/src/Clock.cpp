#include "Clock.h"

#include <Preferences.h>
#include <sys/time.h>

namespace tw {

namespace wallclock {

static bool rtcSet = false;

static void applyZone(const char *tz) {
    setenv("TZ", tz && *tz ? tz : "UTC0", 1);
    tzset();
}

static void setSystemTime(uint32_t epoch) {
    timeval tv = {static_cast<time_t>(epoch), 0};
    settimeofday(&tv, nullptr);
}

void begin(Inkplate &display) {
    Preferences prefs;
    prefs.begin("clock", true);
    String tz = prefs.getString("tz", "UTC0");
    prefs.end();

    // The RTC library converts with the current time zone, so talk to it in UTC.
    applyZone("UTC0");
    rtcSet = display.rtc.isSet();
    if (rtcSet)
        setSystemTime(display.rtc.getEpoch());
    applyZone(tz.c_str());
    time_t now = time(nullptr);
    tm local;
    localtime_r(&now, &local);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M %Z", &local);
    Serial.printf("clock: rtc %s (epoch %lu), tz \"%s\", local %s\n", rtcSet ? "set" : "not set",
                  (unsigned long)now, tz.c_str(), stamp);
}

bool isSet() {
    return rtcSet;
}

void set(Inkplate &display, uint32_t epoch, const char *tz) {
    applyZone("UTC0"); // the RTC library converts with the current time zone
    display.rtc.setEpoch(epoch);
    setSystemTime(epoch);
    Preferences prefs;
    prefs.begin("clock", false);
    prefs.putString("tz", tz);
    prefs.end();
    applyZone(tz);
    rtcSet = true;
}

String zone() {
    Preferences prefs;
    prefs.begin("clock", true);
    String tz = prefs.getString("tz", "UTC0");
    prefs.end();
    return tz;
}

void today(char *out) {
    time_t now = time(nullptr);
    tm local;
    localtime_r(&now, &local);
    strftime(out, 11, "%Y-%m-%d", &local);
}

} // namespace wallclock

} // namespace tw
