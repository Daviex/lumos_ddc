#ifndef IDLE_ACTIVITY_H
#define IDLE_ACTIVITY_H

#include <windows.h>

/* Zero-initialize once. Windows input timestamps are DWORDs and can move
   backwards; elapsed inactivity is measured on the caller's monotonic clock. */
typedef struct {
    BOOL sampled;
    BOOL inputValid;
    BOOL activityValid;
    DWORD inputTick;
    ULONGLONG activityTick;
} IdleActivity;

void IdleActivity_Record(IdleActivity *activity, ULONGLONG now);
DWORD IdleActivity_Sample(IdleActivity *activity, ULONGLONG now,
                          BOOL inputAvailable, DWORD inputTick);

#endif
