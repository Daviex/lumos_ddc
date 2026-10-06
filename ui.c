#include "ui_internal.h"

/* Each window lives in its own ui_*.c module. This file only starts them up
   and tears them down in one place. */

HINSTANCE g_uiInst;

/* ---- Public API ---- */

BOOL UI_Init(HINSTANCE hInst)
{
    g_uiInst = hInst;

    if (!UiPopup_Init(hInst))
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
    UiMenu_Shutdown();
    UiOsd_Shutdown();
    UiSched_Shutdown();
    UiSettings_Shutdown();
}
