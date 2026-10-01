#ifndef UI_H
#define UI_H

#include <windows.h>
#include "monitor.h"
#include "presets.h"

/* Dark theme colors (0xRRGGBB) */
#define CLR_BG          0x1E1E2E   /* #1e1e2e */
#define CLR_TEXT        0xCDD6F4   /* #cdd6f4 */
#define CLR_ACCENT      0x89B4FA   /* #89b4fa */
#define CLR_TRACK       0x313244   /* #313244 */
#define CLR_SURFACE     0x2D2E3E   /* #2d2e3e */
#define CLR_SUBTEXT     0x9EA0B0   /* #9ea0b0 */

/* Popup dimensions */
#define POPUP_WIDTH     320
#define POPUP_ROW_H     72
#define POPUP_PADDING   16
#define POPUP_CORNER    12

#define SLIDER_TRACK_H  6
#define SLIDER_THUMB_R  8

/* Initialize window classes and shut down the UI subsystem. */
BOOL UI_Init(HINSTANCE hInst);

/* Shutdown UI subsystem */
void UI_Shutdown(void);

/* Popup lifecycle used by the UI subsystem. */
BOOL UI_PopupInit(HINSTANCE hInst);
void UI_PopupShutdown(void);

/* Create the popup window (hidden initially) */
HWND UI_CreatePopup(HINSTANCE hInst, MonitorList *ml);

/* Show popup above tray icon */
void UI_ShowPopup(HWND hwnd, MonitorList *ml);

/* Hide popup */
void UI_HidePopup(HWND hwnd);

/* Toggle popup visibility */
void UI_TogglePopup(HWND hwnd, MonitorList *ml);

/* Check if popup is visible */
BOOL UI_IsPopupVisible(HWND hwnd);

/* Refresh popup visuals (call after brightness changes) */
void UI_RefreshPopup(HWND hwnd, MonitorList *ml);

/* Preserve the application's base target when monitor deltas clamp the
   hardware values, so the master slider does not infer a different target. */
void UI_SetMasterTarget(int target);

/* Show brief OSD overlay on a monitor */
void UI_ShowOSD(HINSTANCE hInst, HMONITOR hMon, int percent);

/* Set callback invoked when delta buttons are clicked (for saving to INI) */
typedef void (*DeltaSaveCallback)(void);
void UI_SetDeltaSaveCallback(DeltaSaveCallback cb);

/* Called when the user manually changes brightness via the popup slider.
   row == -1 identifies the master slider and target is its extended base
   brightness; otherwise row is the monitor index and target is 0-100. */
typedef void (*ManualChangeCallback)(int row, int target);
void UI_SetManualChangeCallback(ManualChangeCallback cb);

/* Context menu dimensions */
#define CTXMENU_WIDTH   220
#define CTXMENU_ITEM_H  32
#define CTXMENU_SEP_H   9
#define CTXMENU_CORNER  8
#define CTXMENU_PAD     6

/* Show custom dark context menu at cursor */
void UI_ShowContextMenu(HWND hwndOwner, Settings *s);

/* Editor window dimensions */
#define SCHED_WIDTH     300
#define SCHED_ROW_H     34
#define SCHED_HEADER_H  40
#define SCHED_FOOTER_H  44
#define SCHED_CORNER    12

/* Show the schedule editor. On Save it updates s->schedule/scheduleCount and
   posts WM_COMMAND(IDM_SCHEDULE_SAVED) to hwndOwner. */
void UI_ShowScheduleEditor(HWND hwndOwner, Settings *s);

/* Settings window dimensions */
#define SET_WIDTH       320
#define SET_HEADER_H    40
#define SET_SECTION_H   26
#define SET_ROW_H       32
#define SET_FOOTER_H    48
#define SET_CORNER      12

/* Show the settings window. On Save it writes the edited values into *s and
   posts WM_COMMAND(IDM_SETTINGS_SAVED) to hwndOwner. */
void UI_ShowSettings(HWND hwndOwner, Settings *s);

/* About window dimensions */
#define ABOUT_WIDTH     300
#define ABOUT_HEIGHT    182
#define ABOUT_CORNER    12

/* Show the custom dark About window (version, author, clickable repo link). */
void UI_ShowAbout(HWND hwndOwner);

#endif /* UI_H */
