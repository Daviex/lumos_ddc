#ifndef RESOURCE_H
#define RESOURCE_H

#define IDI_LUMOS           100

#define WM_TRAYICON         (WM_USER + 1)
#define WM_HOTKEY_BRIGHTEN  1
#define WM_HOTKEY_DIM       2
#define WM_HOTKEY_PREV_MON  3
#define WM_HOTKEY_NEXT_MON  4
#define WM_HOTKEY_POPUP     5

#define IDM_RESCAN          2001
#define IDM_AUTOSTART       2002
#define IDM_EXIT            2003
#define IDM_SCHEDULE_TOGGLE 2004
#define IDM_SCHEDULE_EDIT   2005
#define IDM_SCHEDULE_SAVED  2006
#define IDM_ABOUT           2007
#define IDM_IDLEDIM_TOGGLE  2008
#define IDM_SETTINGS        2009
#define IDM_SETTINGS_SAVED  2010
#define IDM_PRESET_BASE     3000

/* ---- App identity (shown in the About window) ---- */
#define APP_NAME            L"Lumos"
#define APP_VERSION         L"1.1.0"
#define APP_AUTHOR          L"sfortis"
#define APP_REPO_DISPLAY    L"github.com/sfortis/lumos_ddc"
#define APP_REPO_URL        L"https://github.com/sfortis/lumos_ddc"

#endif /* RESOURCE_H */
