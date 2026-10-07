#include "idle_activity.h"

void IdleActivity_Record(IdleActivity *activity, ULONGLONG now)
{
    activity->activityValid = TRUE;
    activity->activityTick = now;
}

DWORD IdleActivity_Sample(IdleActivity *activity, ULONGLONG now,
                          BOOL inputAvailable, DWORD inputTick)
{
    if (!inputAvailable) {
        /* Leave the desktop visible on a failed query. The first good sample
           after recovery must not resurrect an old inactivity interval. */
        IdleActivity_Record(activity, now);
        activity->sampled = TRUE;
        activity->inputValid = FALSE;
        return 0;
    }

    if (!activity->sampled) {
        DWORD elapsed = (DWORD)now - inputTick;
        /* Preserve a plausible first sample, including the ordinary DWORD
           wrap. More than half a DWORD cycle is ambiguous with a future tick. */
        ULONGLONG initial = elapsed <= 0x7fffffffUL && elapsed <= now
                            ? now - elapsed : now;
        if (!activity->activityValid || initial > activity->activityTick)
            IdleActivity_Record(activity, initial);
    } else if (!activity->inputValid || inputTick != activity->inputTick) {
        /* Any changed value is newly observed input, even if it is retrograde
           or supplied by SendInput with its own timestamp. */
        IdleActivity_Record(activity, now);
    }

    activity->sampled = TRUE;
    activity->inputValid = TRUE;
    activity->inputTick = inputTick;
    if (now < activity->activityTick) IdleActivity_Record(activity, now);
    ULONGLONG elapsed = now - activity->activityTick;
    return elapsed > MAXDWORD ? MAXDWORD : (DWORD)elapsed;
}
