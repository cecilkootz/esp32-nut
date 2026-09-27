#ifndef RESTART_GUARD_H
#define RESTART_GUARD_H

#include <Arduino.h>

// A reboot takes NUT offline for about ten seconds. upsmon treats a UPS it can't reach
// as critical, and shuts its host down, when the UPS was last seen in one of these
// states (on battery only once DEADTIME, 15 s by default, has passed).
static const char* const RESTART_BLOCKING_STATES[] = {"FSD", "OB", "CAL", "BYPASS", "OFF", "OVER", "ALARM"};

// Why a restart someone asked for must wait, or nullptr. Stale or missing data
// already looks unreachable to upsmon, so a restart changes nothing for it.
inline const char* restartBlocker(const String& ups_status, bool data_fresh, bool forced_shutdown) {
    if (forced_shutdown) {
        return "FSD";
    }
    if (!data_fresh) {
        return nullptr;
    }
    String padded = " ";
    padded += ups_status;
    padded += " ";
    for (const char* state : RESTART_BLOCKING_STATES) {
        String token = " ";
        token += state;
        token += " ";
        if (padded.indexOf(token) >= 0) {
            return state;
        }
    }
    return nullptr;
}

#endif // RESTART_GUARD_H
