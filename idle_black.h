#ifndef IDLE_BLACK_H
#define IDLE_BLACK_H

#include "monitor.h"

#define WM_IDLE_BLACK_WAKE (WM_APP + 3)

void IdleBlack_Init(HINSTANCE instance, HWND owner);
/* All calls belong to the UI thread. enabled is the global idle setting. */
void IdleBlack_Update(const MonitorList *view, BOOL enabled, BOOL idle);
/* Revalidate the input sample which led to the idle decision before covering
   the desktop. A changed or unavailable sample requests a main-thread wake. */
void IdleBlack_UpdateForInput(const MonitorList *view, BOOL enabled, BOOL idle,
                             DWORD decisionInput);
/* Hide immediately on user activity/topology changes; preserve the power request. */
void IdleBlack_Clear(void);
void IdleBlack_SetSessionLocked(BOOL locked);
void IdleBlack_Shutdown(void);
BOOL IdleBlack_Active(void);
/* TRUE only after a display request was acquired and until it is released.
   A failed release retains ownership and is retried by later updates. */
BOOL IdleBlack_HoldsDisplayRequest(void);

#endif
