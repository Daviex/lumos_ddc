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
    WCHAR    deviceInstance[256]; /* stable PnP key for matching across rescans */
} BrightMonitor;

typedef struct {
    BrightMonitor monitors[MAX_MONITORS];
    int           count;
    int           active;  /* index of "active" monitor for hotkey control */
} MonitorList;

/* Enumerate all physical monitors with DDC/CI support */
void Monitor_Enumerate(MonitorList *ml);

/* Release this list's leases and clear it. Other lists and queued requests
   retain independent ownership even when dxva2 reuses the same handle. */
void Monitor_Cleanup(MonitorList *ml);

/* Refresh brightness values from hardware */
void Monitor_RefreshBrightness(MonitorList *ml);

/* Set brightness for a single monitor (0-100 percentage) */
BOOL Monitor_SetBrightness(BrightMonitor *mon, DWORD percent);

/* TRUE when at least one monitor in the list can actually be set. Tells a real
   enumeration apart from the placeholder Windows reports while a display is
   still coming back after sleep. */
BOOL Monitor_HasControllable(const MonitorList *ml);

/* Set brightness for all monitors (base percent, can exceed 0-100 with deltas) */
void Monitor_SetAllBrightness(MonitorList *ml, int percent);

/* Adjust active monitor brightness by delta (-10 or +10 etc) */
void Monitor_AdjustActive(MonitorList *ml, int delta);

/* Cycle active monitor forward/backward */
void Monitor_CycleActive(MonitorList *ml, int direction);

/* UI setters/refreshes enqueue work after MonitorWorker_Start. These helpers
   are used only by the hardware worker and the initial startup enumeration. */
BOOL Monitor_SetBrightnessSync(BrightMonitor *mon, DWORD percent);
/* Bit i is set only when monitor i was successfully read and validated. */
DWORD Monitor_RefreshBrightnessSync(MonitorList *ml);
void Monitor_PreviewBrightness(BrightMonitor *mon, DWORD percent);

/* A queued request owns a lease independently of the UI's monitor list.
   Handle destruction is deferred to the worker, including during rescans. */
void Monitor_Retain(const BrightMonitor *mon);
void Monitor_Release(const BrightMonitor *mon);
void Monitor_FlushRetiredHandles(void);

#endif /* MONITOR_H */
