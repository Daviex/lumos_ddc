#ifndef MONITOR_H
#define MONITOR_H

#include <windows.h>

#undef MAX_MONITORS
#define MAX_MONITORS 16

/* Control backend for a monitor. DDC/CI for external displays (dxva2),
 * WMI backlight for internal laptop panels (root\WMI). */
typedef enum {
    BACKEND_NONE = 0,
    BACKEND_DDC,
    BACKEND_WMI
} MonitorBackend;

typedef struct {
    HANDLE   hPhysical;
    HMONITOR hMonitor;
    WCHAR    name[128];
    DWORD    brightnessMin;
    DWORD    brightnessCur;
    DWORD    brightnessMax;
    BOOL     controllable; /* TRUE if brightness is settable via any backend */
    BOOL     hasHandle;    /* TRUE if hPhysical is valid (can be 0!) */
    int      delta;        /* per-monitor brightness offset, -40..+40 */
    MonitorBackend backend;
    WCHAR    wmiInstance[256]; /* WMI InstanceName when backend == BACKEND_WMI */
} BrightMonitor;

typedef struct {
    BrightMonitor monitors[MAX_MONITORS];
    int           count;
    int           active;  /* index of "active" monitor for hotkey control */
} MonitorList;

/* Enumerate all physical monitors with DDC/CI support */
void Monitor_Enumerate(MonitorList *ml);

/* Free physical monitor handles */
void Monitor_Cleanup(MonitorList *ml);

/* Refresh brightness values from hardware */
void Monitor_RefreshBrightness(MonitorList *ml);

/* Current level of a monitor as a percentage of its own range (cached value,
   no DDC traffic). */
int Monitor_GetPercent(const BrightMonitor *mon);

/* Set brightness for a single monitor (0-100 percentage) */
BOOL Monitor_SetBrightness(BrightMonitor *mon, DWORD percent);

/* Release the handles of *ml that *keep does not also reference, and empty the
   list. dxva2 hands out the same physical monitor handle for the same display
   again, so releasing a list wholesale can invalidate a handle another list has
   just acquired: every write through it then fails with
   ERROR_GRAPHICS_INVALID_PHYSICAL_MONITOR_HANDLE. */
void Monitor_CleanupExcept(MonitorList *ml, const MonitorList *keep);

/* TRUE when at least one monitor in the list can actually be set. Tells a real
   enumeration apart from the placeholder Windows reports while a display is
   still coming back after sleep. */
BOOL Monitor_HasControllable(const MonitorList *ml);

/* Set brightness for all monitors (base percent, can exceed 0-100 with deltas).
   Returns FALSE when a controllable monitor refused the write (a stale DDC
   handle after sleep, for example). */
BOOL Monitor_SetAllBrightness(MonitorList *ml, int percent);

/* Adjust active monitor brightness by delta (-10 or +10 etc) */
void Monitor_AdjustActive(MonitorList *ml, int delta);

/* Cycle active monitor forward/backward */
void Monitor_CycleActive(MonitorList *ml, int direction);

#endif /* MONITOR_H */
