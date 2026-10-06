#ifndef REMOTE_H
#define REMOTE_H

/* Executes lumosctl requests (see ipc.h) in the running Lumos.

   This module owns the protocol: validation, monitor lookup by number or
   name, and the reply text. The actions themselves stay in lumos.c, which
   hands them over as an AppControl, so this file never touches its state. */

#include <windows.h>
#include "monitor.h"
#include "presets.h"

/* Every action first counts as user activity: it ends an idle dim and restarts
   the idle countdown, as keyboard or mouse input would. The brightness actions
   return FALSE when a monitor refused the write. */
typedef struct {
    MonitorList *(*monitors)(void);
    Settings    *(*settings)(void);
    int  (*masterLevel)(void);                  /* base level of "All monitors" */
    BOOL (*setMaster)(int percent);             /* absolute, like a preset */
    BOOL (*stepMaster)(int delta);              /* relative, like a hotkey, no OSD */
    BOOL (*setMonitor)(int index, int percent); /* one monitor, absolute */
    BOOL (*applyPreset)(int index);
    void (*setSchedule)(BOOL on);
    void (*setIdleDim)(BOOL on);
    void (*rescan)(void);
} AppControl;

void Remote_Init(const AppControl *app);

/* Call from the main window on WM_COPYDATA. Returns TRUE and sets *result
   when the message was a lumosctl request. */
BOOL Remote_HandleCopyData(HWND hwnd, WPARAM wParam, LPARAM lParam, LRESULT *result);

#endif /* REMOTE_H */
