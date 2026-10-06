#ifndef REMOTE_H
#define REMOTE_H

/* Executes lumosctl requests (see ipc.h) in the running Lumos.

   This module owns the protocol: validation, monitor lookup by number or
   name, and the reply text. The actions themselves stay in lumos.c, which
   hands them over as an AppControl, so this file never touches its state. */

#include <windows.h>
#include "monitor.h"
#include "presets.h"

typedef struct {
    MonitorList *(*monitors)(void);
    Settings    *(*settings)(void);
    int  (*masterLevel)(void);                  /* base level of "All monitors" */
    void (*setMaster)(int percent);             /* absolute, like a preset */
    void (*stepMaster)(int delta);              /* relative, like a hotkey, no OSD */
    void (*setMonitor)(int index, int percent); /* one monitor, absolute */
    void (*applyPreset)(int index);
    void (*setSchedule)(BOOL on);
    void (*setIdleDim)(BOOL on);
    void (*rescan)(void);
} AppControl;

void Remote_Init(const AppControl *app);

/* Call from the main window on WM_COPYDATA. Returns TRUE and sets *result
   when the message was a lumosctl request. */
BOOL Remote_HandleCopyData(HWND hwnd, WPARAM wParam, LPARAM lParam, LRESULT *result);

#endif /* REMOTE_H */
