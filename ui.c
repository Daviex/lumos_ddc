#include "ui_internal.h"
#include "ui_monitor_selection.h"

/* Each window lives in its own ui_*.c module. This file only starts them up
   and tears them down in one place. */

HINSTANCE g_uiInst;

/* ---- Public API ---- */

BOOL UI_Init(HINSTANCE hInst)
{
    g_uiInst = hInst;

    if (!UI_PopupInit(hInst) || !UI_MonitorSelectionInit(hInst))
        return FALSE;
    UiOsd_Init(hInst);
    UiMenu_Init(hInst);
    UiSched_Init(hInst);
    UiSettings_Init(hInst);
    UiAbout_Init(hInst);

    return TRUE;
}

void UI_Shutdown(void)
{
    UI_CloseMonitorSelection();
    UI_PopupShutdown();
    UiMenu_Shutdown();
    UiOsd_Shutdown();
    UiSched_Shutdown();
    UiSettings_Shutdown();
}

BOOL UI_HandleDialogMessage(MSG *message)
{
    return UI_MonitorSelectionMessage(message);
}
