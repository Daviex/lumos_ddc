#ifndef MONITOR_WORKER_H
#define MONITOR_WORKER_H

#include "monitor.h"

#define WM_MONITOR_RESULT (WM_APP + 2)

/* Results carry values, never ownership of physical handles. */
typedef struct {
    DWORD generation;
    DWORD sequence;
    int index;
    BOOL success;
    DWORD minimum, current, maximum;
} MonitorResult;

typedef struct {
    BrightMonitor monitor; /* identity only; does not own a handle lease */
    DWORD percent;
} MonitorTarget;

BOOL MonitorWorker_Start(HWND owner, MonitorList *view);
void MonitorWorker_Stop(void);
BOOL MonitorWorker_Running(void);
BOOL MonitorWorker_Set(BrightMonitor *monitor, DWORD percent);
void MonitorWorker_Refresh(const MonitorList *view);
void MonitorWorker_Reset(void);
/* Preserve outstanding user intent before replacing the monitor topology.
   Bit i indicates a target populated at index i. */
DWORD MonitorWorker_PendingTargets(MonitorTarget targets[MAX_MONITORS]);
BOOL MonitorWorker_Accept(const MonitorResult *result);
void MonitorWorker_Wake(void);

#endif
