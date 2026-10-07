#ifndef MONITOR_WORKER_H
#define MONITOR_WORKER_H

#include "monitor.h"

#define WM_MONITOR_RESULT (WM_APP + 2)

/* Results carry values, never ownership of physical handles. */
typedef enum {
    MONITOR_RESULT_BRIGHTNESS = 0,
    MONITOR_RESULT_SOURCE,
    MONITOR_RESULT_SKIPPED,
    MONITOR_RESULT_CANCELLED
} MonitorResultKind;

typedef struct {
    DWORD generation;
    DWORD sequence;
    int index;
    BOOL success;
    DWORD minimum, current, maximum;
    MonitorResultKind kind;
    DWORD currentInput;
    BOOL sourceKnown;
    BOOL sourceUpdated; /* TRUE only when this operation actually read the source. */
    ULONGLONG sourceCheckedTick;
    MonitorWritePurpose purpose;
    DWORD idleEpoch;
    BOOL preIdleBrightnessValid;
    DWORD preIdleBrightness;
    BOOL brightnessWritten; /* Successful native write, never a brightness refresh. */
    BOOL brightnessUpdated; /* Validated payload, including recovery during source polling. */
    WCHAR deviceInstance[256]; /* Stable value-only identity for applied writes after a rescan. */
    MonitorBackend backend;
    BOOL sourceFilter;
    DWORD expectedInput;
} MonitorResult;

typedef struct {
    BrightMonitor monitor; /* identity only; does not own a handle lease */
    DWORD percent;
} MonitorTarget;

BOOL MonitorWorker_Start(HWND owner, MonitorList *view);
void MonitorWorker_Stop(void);
BOOL MonitorWorker_Running(void);
BOOL MonitorWorker_Set(BrightMonitor *monitor, DWORD percent);
BOOL MonitorWorker_SetIdle(BrightMonitor *monitor, DWORD percent);
BOOL MonitorWorker_RestoreIdle(BrightMonitor *monitor, DWORD rawBrightness);
BOOL MonitorWorker_ReleaseIdle(BrightMonitor *monitor, DWORD rawBrightness, DWORD otherInput);
/* Invalidates this monitor's pending/in-flight work without disturbing others. */
void MonitorWorker_Cancel(BrightMonitor *monitor);
void MonitorWorker_Refresh(const MonitorList *view);
/* Poll selected displays with filters or unresolved brightness capability,
   including temporarily skipped sources.
   allMonitors includes every DDC display while the chooser is open. */
void MonitorWorker_RefreshSources(const MonitorList *view, BOOL allMonitors);
void MonitorWorker_Reset(void);
/* Preserve outstanding user intent before replacing the monitor topology.
   Bit i indicates a target populated at index i. */
DWORD MonitorWorker_PendingTargets(MonitorTarget targets[MAX_MONITORS]);
BOOL MonitorWorker_Accept(const MonitorResult *result);
/* Applied hardware state from an older sequence still belongs to this topology.
   Use for idle ownership only; display telemetry uses the stricter Accept. */
BOOL MonitorWorker_AcceptState(const MonitorResult *result);
void MonitorWorker_Wake(void);

#endif
