#include "app_schedule.h"

// The order of these tests is the contract, not an implementation detail: every rejection
// above the arithmetic returns true, so a caller that gets `false` has been told the schedule
// is on, the hour is real and the window is sane.
bool app_schedule_active(bool schedule_on, int hour, int start_hour, int end_hour)
{
    if (!schedule_on) {
        return true;
    }
    // APP_SCHEDULE_NO_CLOCK arrives here, and so would a clock that decoded into nonsense.
    // Both are the same answer.
    if (hour < 0 || hour > 23) {
        return true;
    }
    if (start_hour < 0 || start_hour > 23 || end_hour < 0 || end_hour > 23) {
        return true;
    }
    if (start_hour == end_hour) {
        return true;
    }
    if (start_hour < end_hour) {
        return hour >= start_hour && hour < end_hour;
    }
    // Wraps midnight. Not `&&` with the operands swapped -- that is the empty set, and it is
    // the bug this function exists to be tested for.
    return hour >= start_hour || hour < end_hour;
}
