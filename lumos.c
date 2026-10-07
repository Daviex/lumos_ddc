#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <shlobj.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "resource.h"
#include "monitor.h"
#include "brightmap.h"
#include "ui.h"
#include "presets.h"
#include "capture.h"
#include "remote.h"

/* GUID_CONSOLE_DISPLAY_STATE {6FE69556-704A-47A0-8F24-C28D936FDA47}
   Defined manually because some MinGW headers omit it. Fires on display
   power on/off (including the transition back on after a lock screen). */
static const GUID kGuidConsoleDisplayState =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

/* Coalesce the near-simultaneous unlock + display-power triggers into one
   re-enumeration so we don't hammer the DDC/I2C bus. */
#define RESCAN_TIMER_ID     0xB100
#define RESCAN_DEBOUNCE_MS  600

/* Posted by the rescan worker thread when the fresh MonitorList is ready.
   lParam = MonitorList* (heap, adopted and freed by the main thread). */
#define WM_APP_RESCAN_DONE  (WM_APP + 1)

/* Posted by the mouse hook when wheel notches over the tray icon start to
   pile up; the notches themselves are counted in g_wheelPending. */
#define WM_APP_WHEEL        (WM_APP + 2)

/* Schedule tick: recompute the interpolated brightness once a minute. */
#define SCHEDULE_TIMER_ID   0xB101
#define SCHEDULE_TICK_MS    60000

/* A rescan worker can hang for minutes inside a dxva2 call while a display is
   coming back from sleep. The watchdog writes such a worker off so the
   single-flight guard cannot jam the app out of ever rescanning again. */
#define RESCAN_WATCHDOG_TIMER_ID  0xB103
#define RESCAN_WATCHDOG_MS        10000

/* Windows reports either no monitor or a generic placeholder panel for the
   first few seconds after a DisplayPort link returns. Rather than adopt that,
   we keep the list we have and rescan on this backoff. */
#define RESCAN_RETRY_TIMER_ID     0xB104
static const DWORD kRescanBackoffMs[] = { 2000, 5000, 10000, 20000 };
#define RESCAN_MAX_RETRIES ((int)(sizeof(kRescanBackoffMs) / sizeof(kRescanBackoffMs[0])))

/* An external monitor can come back from sleep slower than the laptop panel
   beside it: the list then has a controllable monitor and is adopted, while the
   external one does not answer DDC/CI yet. Such a monitor is rescanned on the
   backoff above and then at this interval until it answers. A retry that falls
   due while the popup is open waits for it to close, because adopting a list
   rebuilds the popup and would close it under the user. */
#define RESCAN_AWAIT_STEADY_MS    60000
#define RESCAN_AWAIT_DEFER_MS     2000

/* A written-off worker stays parked in the driver call forever, so retrying
   without a bound would leak one thread every watchdog period for as long as
   the display stays broken. After this many in a row we stop on our own and
   wait for the next real trigger: a display change, an unlock, or the manual
   Re-scan Monitors. */
#define RESCAN_MAX_WRITEOFFS 3

/* Some displays report a topology change every half minute or so, because the
   link keeps retraining (a Samsung G9 with VRR on DisplayPort, observed at 350
   changes an hour, day and night). Reacting to each one costs a full
   enumeration, so display changes get a floor on how often they may start a
   scan. Power, unlock and the manual re-scan are not throttled. */
#define RESCAN_MIN_INTERVAL_MS 30000

/* Idle auto-dim poll. GetLastInputInfo costs nothing and we only touch the
   monitors on a state transition, so the interval is set by how fast the
   brightness must come back once the user returns, not by polling cost. */
#define IDLE_TIMER_ID       0xB102
#define IDLE_TICK_MS        2000

static HINSTANCE    g_hInst;
static HWND         g_hwndHidden;    /* Hidden top-level window (receives broadcasts + notifications) */
static HWND         g_hwndPopup;
static MonitorList  g_monitors;
static Settings     g_settings;
static NOTIFYICONDATAW g_nid;
static HHOOK        g_mouseHook;
static HPOWERNOTIFY g_hPowerNotify;   /* GUID_CONSOLE_DISPLAY_STATE registration */
static UINT         g_wmTakeover;     /* cross-process "quit, I'm replacing you" message */
static volatile LONG g_rescanBusy;    /* 1 while a rescan worker thread is in flight */
static BOOL         g_rescanPending;  /* a trigger arrived mid-rescan; run once more (main thread only) */
static BOOL         g_reapplyOnRescan; /* set by wake/unlock/display-on: re-push brightness after the rescan */
static BOOL         g_scheduleSuspended = FALSE;
static int          g_scheduleSuspendMinute = 0;   /* minute-of-day at suspend */
static int          g_scheduleResumeMinute = 0;    /* next anchor to resume at */
static int          g_scheduleLastApplied = -1;    /* last brightness pushed by the schedule */
static int          g_masterTarget = -1;      /* intended base percent, tracked across hotkey presses */
static BOOL         g_idleDimmed = FALSE;     /* TRUE while the idle level is on the monitors */
static DWORD        g_rescanStartTick = 0;    /* when the current worker was launched */
static DWORD        g_rescanGeneration = 0;   /* incremented per launch */
static DWORD        g_rescanAwaitedGen = 0;   /* the only generation whose result we accept */
static int          g_rescanRetry = 0;        /* index into kRescanBackoffMs */
static int          g_awaitRetry = 0;         /* index into kRescanBackoffMs for unanswered monitors */
static int          g_rescanWriteOffs = 0;    /* consecutive workers the watchdog gave up on */
static DWORD        g_lastRescanTick = 0;     /* when the last worker was launched */
static Hotkey       g_hotkeysActive[HOTKEY_COUNT]; /* what RegisterHotKey currently holds */
static int          g_hotkeyStartupFailure = -1;   /* first action we could not register, or -1 */
static BOOL         g_hotkeysSuspended = FALSE;    /* released while Settings captures keys */
static DWORD        g_trayRightClickTick = 0;      /* last right button down or up on the icon */
static DWORD        g_trayKeySelectTick = 0;       /* last NIN_KEYSELECT */
static DWORD        g_lastRemoteTick = 0;          /* last lumosctl command, counted as activity */
static int          g_wheelPending = 0;            /* tray wheel notches not applied yet (hook and handler share the UI thread) */
static BOOL         g_remoteSeen = FALSE;          /* g_lastRemoteTick is valid */
#define TRAY_CLICK_WINDOW_MS 1000

/* RegisterHotKey id per HOTKEY_* action, which is also the WM_HOTKEY wParam. */
static const int kHotkeyIds[HOTKEY_COUNT] = {
    WM_HOTKEY_BRIGHTEN, WM_HOTKEY_DIM, WM_HOTKEY_POPUP
};

static const WCHAR APPCLASS[] = L"LumosMain";

/* Forward declarations */
static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
static void CreateTrayIcon(HWND hwnd);
static void RemoveTrayIcon(void);
static void RegisterHotkeys(HWND hwnd);
static void UnregisterHotkeys(HWND hwnd);
static int  ApplyHotkeys(const Hotkey *hotkeys);
static void SuspendHotkeys(BOOL suspended);
static int  FirstFailedHotkey(void);
static void ShowContextMenu(HWND hwnd, const POINT *anchor, BOOL fromKeyboard);
static void HandleHotkey(int id);
static BOOL ApplyPreset(int index);
static void InstallMouseHook(void);
static void RemoveMouseHook(void);
static void ScheduleRescan(HWND hwnd);
static void ScheduleRescanFromTrigger(HWND hwnd);
static const AppControl kAppControl;   /* lumosctl actions, defined below */
static void StartRescan(HWND hwnd);
static DWORD WINAPI RescanThreadProc(LPVOID param);

/* Heap-passed to the rescan worker: which window to post back to, and which
   generation the result belongs to. */
typedef struct { HWND hwnd; DWORD gen; } RescanArgs;
static void Schedule_ApplyNow(void);
static void Schedule_Suspend(void);
static void ManualChange(void);
static void PopupChange(int masterLevel);
static int  MasterTargetFromMonitors(void);
static void Idle_Tick(void);
static void Idle_Restore(void);

static void SaveDeltasCallback(void)
{
    Settings_SaveDeltas(&g_settings, &g_monitors);
    Settings_Save(&g_settings);
}

/* ---- Entry Point ---- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLineA, int showCmd)
{
    (void)hPrev; (void)cmdLineA; (void)showCmd;
    g_hInst = hInst;

    /* Unique cross-process message id (same value in every Lumos build).
       Used both to signal an older instance to quit and to receive that signal. */
    g_wmTakeover = RegisterWindowMessageW(L"Lumos_TakeoverQuit");

    /* Single-instance with clean handoff: request initial ownership so the mutex
       stays non-signaled while we run. If it already exists, another instance is
       live: ask it to quit, then wait (bounded) to take over. WAIT_ABANDONED means
       the old owner died holding it; both that and WAIT_OBJECT_0 mean we acquired. */
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Lumos_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, g_wmTakeover, 0, 0);
        DWORD w = WaitForSingleObject(hMutex, 5000);
        if (w == WAIT_TIMEOUT || w == WAIT_FAILED) {
            /* Old instance did not release in time (likely a pre-handoff build).
               Refuse to run a second copy rather than fight over the DDC bus. */
            CloseHandle(hMutex);
            return 0;
        }
    }

    /* Initialize COM for Shell */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* Initialize common controls */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    /* Init subsystems */
    memset(&g_monitors, 0, sizeof(g_monitors));
    Monitor_Enumerate(&g_monitors);
    Settings_Init(&g_settings);
    Settings_LoadDeltas(&g_settings, &g_monitors);

    if (!UI_Init(hInst)) {
        MessageBoxW(NULL, L"Failed to initialize UI", APP_NAME, MB_ICONERROR);
        return 1;
    }

    /* Register hidden window class */
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APPCLASS;
    RegisterClassExW(&wc);

    /* Create hidden top-level window. Not HWND_MESSAGE: message-only windows
       do not receive broadcast messages like WM_DISPLAYCHANGE. WS_EX_TOOLWINDOW
       keeps it off the taskbar/alt-tab; it is never shown. */
    g_hwndHidden = CreateWindowExW(WS_EX_TOOLWINDOW, APPCLASS, APP_NAME,
                                    WS_POPUP, 0, 0, 0, 0,
                                    NULL, NULL, hInst, NULL);

    /* Re-enumerate on session unlock and on display power-on. Windows does not
       reliably send WM_DISPLAYCHANGE across a lock screen, so cached DDC handles
       go stale; these notifications trigger a rescan to re-acquire them. */
    WTSRegisterSessionNotification(g_hwndHidden, NOTIFY_FOR_THIS_SESSION);
    g_hPowerNotify = RegisterPowerSettingNotification(
        g_hwndHidden, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);

    /* Create popup (hidden) */
    g_hwndPopup = UI_CreatePopup(hInst, &g_monitors);
    UI_SetDeltaSaveCallback(SaveDeltasCallback);
    static const HotkeyHost hotkeyHost = { ApplyHotkeys, SuspendHotkeys, FirstFailedHotkey };
    UI_SetHotkeyHost(&hotkeyHost);
    Remote_Init(&kAppControl);

    /* Tray icon, hotkeys, mouse hook */
    CreateTrayIcon(g_hwndHidden);
    RegisterHotkeys(g_hwndHidden);
    InstallMouseHook();

    /* Brightness schedule: suspend on manual slider changes, tick every minute,
       and apply the current time slot immediately at startup. */
    UI_SetManualChangeCallback(PopupChange);
    SetTimer(g_hwndHidden, SCHEDULE_TIMER_ID, SCHEDULE_TICK_MS, NULL);
    Schedule_ApplyNow();

    /* Idle auto-dim tick. Always armed: the handler returns at once when the
       feature is off, which keeps enable/disable free of timer bookkeeping. */
    SetTimer(g_hwndHidden, IDLE_TIMER_ID, IDLE_TICK_MS, NULL);

    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Cleanup */
    RemoveMouseHook();
    UnregisterHotkeys(g_hwndHidden);
    if (g_hPowerNotify) UnregisterPowerSettingNotification(g_hPowerNotify);
    WTSUnRegisterSessionNotification(g_hwndHidden);
    RemoveTrayIcon();
    Monitor_Cleanup(&g_monitors);
    UI_Shutdown();
    CoUninitialize();
    /* Release before closing so a replacing instance sees WAIT_OBJECT_0 promptly
       instead of waiting for abandonment. */
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return (int)msg.wParam;
}

/* ---- Tray Icon ---- */

static void CreateTrayIcon(HWND hwnd)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    /* NIF_SHOWTIP: with NOTIFYICON_VERSION_4 (below) the shell shows the
       standard tooltip only when asked to; without it hovering showed nothing. */
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_LUMOS));
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, APP_NAME L" - Monitor Brightness");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    /* Version 4 is what makes the icon usable from the keyboard: Win+B, then
       Enter or Space arrives as NIN_KEYSELECT and Shift+F10 or the Menu key as
       WM_CONTEXTMENU. It also moves the event into LOWORD(lParam) and puts the
       icon's anchor point into wParam. */
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void RemoveTrayIcon(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

/* ---- Mouse wheel on tray icon ---- */

#ifdef DEBUG
static FILE *g_dbgLog = NULL;
static void DbgLog(const char *fmt, ...)
{
    if (!g_dbgLog) {
        /* Same folder as config.ini, for the reason explained in monitor.c. */
        WCHAR path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) {
            wcscat(path, L"\\Lumos");
            CreateDirectoryW(path, NULL);
            wcscat(path, L"\\lumos-app.log");
        } else {
            wcscpy(path, L".\\lumos-app.log");
        }
        g_dbgLog = _wfopen(path, L"a");
    }
    if (!g_dbgLog) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_dbgLog, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_dbgLog, fmt, ap);
    va_end(ap);
    fprintf(g_dbgLog, "\n");
    fflush(g_dbgLog);
}
#else
#define DbgLog(...) ((void)0)
#endif

/* Times a UI-thread operation into the debug log. Several operations here
   talk to the display over DDC/CI, which can block for seconds when the
   display is asleep or the handle is stale, and a blocked UI thread is
   indistinguishable from a hung app. Compiled out of the release build. */
#ifdef DEBUG
#define TIMED(label, ...) do {                                  \
        DWORD t0_ = GetTickCount();                             \
        __VA_ARGS__;                                            \
        DbgLog("%s: %lu ms", label, GetTickCount() - t0_);      \
    } while (0)
#else
#define TIMED(label, ...) do { __VA_ARGS__; } while (0)
#endif

static BOOL IsCursorOverTrayIcon(POINT ptPhysical)
{
    NOTIFYICONIDENTIFIER nii;
    memset(&nii, 0, sizeof(nii));
    nii.cbSize = sizeof(nii);
    nii.hWnd = g_nid.hWnd;
    nii.uID = g_nid.uID;
    RECT rcIcon;
    HRESULT hr = Shell_NotifyIconGetRect(&nii, &rcIcon);
    if (SUCCEEDED(hr)) {
        /* Both rcIcon and ptPhysical are in physical (unscaled) pixels */
        BOOL hit = PtInRect(&rcIcon, ptPhysical);
        DbgLog("GetRect OK: icon=[%d,%d,%d,%d] cursor=[%d,%d] hit=%d",
               rcIcon.left, rcIcon.top, rcIcon.right, rcIcon.bottom,
               ptPhysical.x, ptPhysical.y, hit);
        return hit;
    }
    DbgLog("GetRect FAILED hr=0x%08X hwnd=%p id=%u",
           (unsigned)hr, (void*)nii.hWnd, nii.uID);
    return FALSE;
}

static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT *mhs = (MSLLHOOKSTRUCT *)lParam;
        DbgLog("WM_MOUSEWHEEL at [%d,%d] mouseData=0x%08X",
               (int)mhs->pt.x, (int)mhs->pt.y, (unsigned)mhs->mouseData);
        if (IsCursorOverTrayIcon(mhs->pt)) {
            /* Notches are counted, not posted one by one. A brightness step
               keeps the UI thread busy for 150 ms or more (DDC and WMI writes),
               and a fast scroll sends 10 to 20 notches a second; one message per
               notch built a queue that kept the brightness moving long after
               the wheel stopped. The handler applies all pending notches in
               one write. */
            short delta = (short)HIWORD(mhs->mouseData);
            int before = g_wheelPending;
            g_wheelPending += (delta > 0) ? 1 : -1;
            if (before == 0)
                PostMessageW(g_hwndHidden, WM_APP_WHEEL, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static void InstallMouseHook(void)
{
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_hInst, 0);
    DbgLog("InstallMouseHook: handle=%p err=%u", (void*)g_mouseHook, (unsigned)GetLastError());
}

static void RemoveMouseHook(void)
{
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = NULL;
    }
}

/* ---- Hotkeys ---- */

/* Holding the brighten or dim combination keeps stepping, as it always has.
   The popup hotkey gets MOD_NOREPEAT, because a held key would otherwise open
   and close the popup over and over. */
static BOOL RegisterOne(int action, Hotkey hk)
{
    if (hk.vk == 0)
        return TRUE;   /* disabled, nothing to hold */
    UINT mods = hk.mods | (action == HOTKEY_POPUP ? MOD_NOREPEAT : 0);
    return RegisterHotKey(g_hwndHidden, kHotkeyIds[action], mods, hk.vk);
}

static void ReleaseAll(void)
{
    for (int i = 0; i < HOTKEY_COUNT; i++)
        UnregisterHotKey(g_hwndHidden, kHotkeyIds[i]);
}

/* Put g_hotkeysActive back after a capture or a rejected Save. Another program
   can take a combination in the meantime; such a hotkey is dropped and
   reported, so the Settings window shows it instead of claiming it works. */
static void ReregisterActive(void)
{
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        if (!RegisterOne(i, g_hotkeysActive[i])) {
            DbgLog("hotkey %d was taken by another program", i);
            g_hotkeysActive[i].vk = 0;
            if (g_hotkeyStartupFailure < 0)
                g_hotkeyStartupFailure = i;
        }
    }
}

/* Startup: register what the settings ask for. A combination another program
   already holds is skipped, since there is nobody to ask at this point. The
   Settings window shows the conflict on the row when it next opens. */
static void RegisterHotkeys(HWND hwnd)
{
    (void)hwnd;
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        g_hotkeysActive[i] = g_settings.hotkeys[i];
        if (!RegisterOne(i, g_settings.hotkeys[i])) {
            DbgLog("hotkey %d is in use by another program", i);
            g_hotkeysActive[i].vk = 0;
            if (g_hotkeyStartupFailure < 0)
                g_hotkeyStartupFailure = i;
        }
    }
}

static void UnregisterHotkeys(HWND hwnd)
{
    (void)hwnd;
    ReleaseAll();
}

/* Swap in a whole new set, or none of it. Our own registrations are released
   first, because RegisterHotKey refuses a combination this window already
   holds under another id. On failure the previous set goes back, so a rejected
   Save never leaves the user without working hotkeys. */
static int ApplyHotkeys(const Hotkey *hotkeys)
{
    ReleaseAll();
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        if (!RegisterOne(i, hotkeys[i])) {
            ReleaseAll();
            ReregisterActive();
            return i;
        }
    }
    for (int i = 0; i < HOTKEY_COUNT; i++)
        g_hotkeysActive[i] = hotkeys[i];
    g_hotkeysSuspended = FALSE;
    g_hotkeyStartupFailure = -1;
    return -1;
}

static int FirstFailedHotkey(void)
{
    return g_hotkeyStartupFailure;
}

static void SuspendHotkeys(BOOL suspended)
{
    if (suspended == g_hotkeysSuspended)
        return;
    g_hotkeysSuspended = suspended;
    if (suspended)
        ReleaseAll();
    else
        ReregisterActive();
}

/* ---- Context Menu ---- */

static void ShowContextMenu(HWND hwnd, const POINT *anchor, BOOL fromKeyboard)
{
    UI_ShowContextMenu(hwnd, &g_settings, anchor, fromKeyboard);
}

/* ---- Hotkey Handler ---- */

/* Recover the All Monitors level from what the monitors currently report:
   map each reading back through its offset and average the results. */
static int MasterTargetFromMonitors(void)
{
    int sum = 0, cnt = 0;
    for (int i = 0; i < g_monitors.count; i++) {
        BrightMonitor *mon = &g_monitors.monitors[i];
        if (!mon->controllable) continue;
        sum += BrightMap_Master(Monitor_GetPercent(mon), mon->delta);
        cnt++;
    }
    return cnt > 0 ? sum / cnt : 50;
}

/* Move the master level by delta, the way the All Monitors slider would.
   Shared by the hotkeys, the tray wheel and lumosctl; the OSD is the caller's. */
static BOOL StepMaster(int delta)
{
    /* Initialize target from current state if needed */
    if (g_masterTarget < 0)
        g_masterTarget = MasterTargetFromMonitors();

    g_masterTarget += delta;
    if (g_masterTarget < 0) g_masterTarget = 0;
    if (g_masterTarget > 100) g_masterTarget = 100;

    /* No DDC read-back: Monitor_SetBrightness already stores the written
       level, and the read cost about half of every step, which made the tray
       wheel lag. */
    BOOL ok = TRUE;
    TIMED("step: SetAllBrightness",
          ok = Monitor_SetAllBrightness(&g_monitors, g_masterTarget));

    /* Update popup if visible */
    UI_RefreshPopup(g_hwndPopup, &g_monitors);

    ManualChange();
    return ok;
}

/* A brightness step from the hotkeys or the tray wheel: the step itself, then
   the OSD on the monitor under the cursor. */
static void StepWithOsd(int delta)
{
    StepMaster(delta);

    /* Show OSD on primary monitor (where cursor is) */
    POINT curPos;
    GetCursorPos(&curPos);
    HMONITOR hCurMon = MonitorFromPoint(curPos, MONITOR_DEFAULTTOPRIMARY);
    /* Find matching monitor for percentage display, fallback to first */
    int pct = 50;
    for (int i = 0; i < g_monitors.count; i++) {
        if (g_monitors.monitors[i].hMonitor == hCurMon && g_monitors.monitors[i].controllable) {
            pct = Monitor_GetPercent(&g_monitors.monitors[i]);
            break;
        }
    }
    UI_ShowOSD(g_hInst, hCurMon, pct, !UI_IsPopupVisible(g_hwndPopup));
}

static void HandleHotkey(int id)
{
    int step = g_settings.step;
    int delta = 0;

    switch (id) {
    case WM_HOTKEY_BRIGHTEN: delta = step;  break;
    case WM_HOTKEY_DIM:      delta = -step; break;
    case WM_HOTKEY_POPUP:
        /* No tray icon to anchor to, so the popup opens in the middle of the
           monitor the cursor is on. WM_HOTKEY grants the foreground right that
           UI_ShowPopup needs to take the keyboard focus. */
        if (UI_IsPopupVisible(g_hwndPopup)) {
            UI_HidePopup(g_hwndPopup);
        } else {
            POINT pt;
            GetCursorPos(&pt);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            POINT center = { (mi.rcWork.left + mi.rcWork.right) / 2,
                             (mi.rcWork.top + mi.rcWork.bottom) / 2 };
            UI_ShowPopup(g_hwndPopup, &g_monitors, &center, TRUE);
        }
        return;
    default: return;
    }

    StepWithOsd(delta);
}

/* ---- Apply Preset ---- */

static BOOL ApplyPreset(int index)
{
    if (index < 0 || index >= g_settings.presetCount) return FALSE;
    g_masterTarget = (int)g_settings.presets[index].brightness;
    BOOL ok = Monitor_SetAllBrightness(&g_monitors, g_masterTarget);
    Monitor_RefreshBrightness(&g_monitors);
    UI_RefreshPopup(g_hwndPopup, &g_monitors);

    ManualChange();
    return ok;
}

/* Forward declarations for the switches below (defined further down). */
static void SetScheduleEnabled(BOOL on);
static void SetIdleDimEnabled(BOOL on);

/* ---- lumosctl (remote.c runs the protocol, these are its actions) ---- */

/* A lumosctl command is the user acting, even though no key was pressed: it
   ends an idle dim the way input would, and the idle countdown starts again
   from it. Without this, the next idle tick dimmed a level the user had just
   set (a scheduled "lumosctl --preset Day" was undone two seconds later). */
static void RemoteActivity(void)
{
    g_lastRemoteTick = GetTickCount();
    g_remoteSeen = TRUE;
    if (g_idleDimmed)
        Idle_Restore();
}

static MonitorList *AppMonitors(void) { return &g_monitors; }
static Settings    *AppSettings(void) { return &g_settings; }

/* What the monitors show now, as an All Monitors level. Read from the
   monitors rather than g_masterTarget, which holds the level to come back to
   while the idle dim is on. */
static int AppMasterLevel(void)
{
    int v = MasterTargetFromMonitors();
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

static BOOL AppSetMaster(int percent)
{
    RemoteActivity();
    g_masterTarget = percent;
    BOOL ok = Monitor_SetAllBrightness(&g_monitors, percent);
    Monitor_RefreshBrightness(&g_monitors);
    UI_RefreshPopup(g_hwndPopup, &g_monitors);
    ManualChange();
    return ok;
}

static BOOL AppStepMaster(int delta)
{
    RemoteActivity();
    return StepMaster(delta);
}

static BOOL AppSetMonitor(int index, int percent)
{
    if (index < 0 || index >= g_monitors.count) return FALSE;
    RemoteActivity();
    BOOL ok = Monitor_SetBrightness(&g_monitors.monitors[index], (DWORD)percent);
    UI_RefreshPopup(g_hwndPopup, &g_monitors);
    ManualChange();
    return ok;
}

static BOOL AppApplyPreset(int index)
{
    RemoteActivity();
    return ApplyPreset(index);
}

static void AppSetSchedule(BOOL on)  { RemoteActivity(); SetScheduleEnabled(on); }
static void AppSetIdleDim(BOOL on)   { RemoteActivity(); SetIdleDimEnabled(on); }
static void AppRescan(void)          { RemoteActivity(); ScheduleRescanFromTrigger(g_hwndHidden); }

static const AppControl kAppControl = {
    AppMonitors, AppSettings, AppMasterLevel, AppSetMaster, AppStepMaster,
    AppSetMonitor, AppApplyPreset, AppSetSchedule, AppSetIdleDim, AppRescan
};

/* ---- Monitor rescan (async) ---- */

/* Worker thread: runs the slow DDC/CI enumeration OFF the UI thread, then hands
   the fresh list back via WM_APP_RESCAN_DONE. Kept off-thread because these
   calls can block for seconds while displays settle after unlock/power-on, and
   blocking the UI thread would also stall the WH_MOUSE_LL hook that lives on it,
   causing system-wide mouse jank. Only handle acquisition happens here; all
   window work stays on the main thread. */
static DWORD WINAPI RescanThreadProc(LPVOID param)
{
    RescanArgs *args = (RescanArgs *)param;
    HWND hwnd = args->hwnd;
    DWORD gen = args->gen;
    free(args);

    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(MonitorList));
    if (fresh)
        Monitor_Enumerate(fresh);
    /* Post even on alloc failure (fresh == NULL) so the busy flag is cleared.
       The generation lets the main thread recognise a result from a worker it
       already wrote off, which may arrive minutes late or never. */
    PostMessageW(hwnd, WM_APP_RESCAN_DONE, (WPARAM)gen, (LPARAM)fresh);
    return 0;
}

/* Launch a single-flight rescan. Called only on the main thread. If one is
   already running, remember to run once more when it finishes (coalesces the
   burst of triggers that a single unlock produces). */
static void StartRescan(HWND hwnd)
{
    if (InterlockedCompareExchange(&g_rescanBusy, 1, 0) != 0) {
        g_rescanPending = TRUE;
        return;
    }
    RescanArgs *args = (RescanArgs *)malloc(sizeof(RescanArgs));
    if (!args) {
        g_rescanBusy = 0;
        return;
    }
    args->hwnd = hwnd;
    args->gen = ++g_rescanGeneration;
    g_rescanAwaitedGen = args->gen;
    g_rescanStartTick = GetTickCount();

    HANDLE h = CreateThread(NULL, 0, RescanThreadProc, args, 0, NULL);
    if (h) {
        CloseHandle(h);
        DbgLog("rescan: worker %lu launched", args->gen);
        g_lastRescanTick = g_rescanStartTick;
        SetTimer(hwnd, RESCAN_WATCHDOG_TIMER_ID, RESCAN_WATCHDOG_MS, NULL);
    } else {
        free(args);
        g_rescanAwaitedGen = 0;
        g_rescanBusy = 0;   /* launch failed; keep the current list */
    }
}

/* Debounced trigger: SetTimer with the same id restarts the interval, so a
   burst of notifications collapses into one rescan once things settle. */
static void ScheduleRescan(HWND hwnd)
{
    SetTimer(hwnd, RESCAN_TIMER_ID, RESCAN_DEBOUNCE_MS, NULL);
}

/* Throttled entry point for display changes: scan no sooner than
   RESCAN_MIN_INTERVAL_MS after the last one, but always scan eventually, so a
   real plug or unplug is delayed rather than dropped. */
static void ScheduleRescanThrottled(HWND hwnd)
{
    g_rescanWriteOffs = 0;
    g_rescanRetry = 0;
    g_awaitRetry = 0;
    DWORD since = GetTickCount() - g_lastRescanTick;
    DWORD delay = (since >= RESCAN_MIN_INTERVAL_MS)
                  ? RESCAN_DEBOUNCE_MS
                  : RESCAN_MIN_INTERVAL_MS - since;
    SetTimer(hwnd, RESCAN_TIMER_ID, delay, NULL);
}

/* A display change, an unlock or a manual re-scan is a fresh start: clear the
   counters that stopped us retrying on our own. */
static void ScheduleRescanFromTrigger(HWND hwnd)
{
    g_rescanWriteOffs = 0;
    g_rescanRetry = 0;
    g_awaitRetry = 0;
    KillTimer(hwnd, RESCAN_RETRY_TIMER_ID);   /* a pending backoff is now moot */
    ScheduleRescan(hwnd);
}

/* ---- Schedule runtime ---- */

static int CurrentMinuteOfDay(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wHour * 60 + st.wMinute;
}

/* Push the schedule's brightness for the current time, unless suspended,
   disabled, or empty. Applies only when the value changed (less DDC traffic). */
static void Schedule_ApplyNow(void)
{
    if (!g_settings.scheduleEnabled || g_settings.scheduleCount == 0)
        return;

    /* The idle level owns the monitors right now. Leave it alone and force a
       re-push once the user is back, since by then the schedule value for the
       current time may equal the last one we applied. */
    if (g_idleDimmed) {
        g_scheduleLastApplied = -1;
        return;
    }

    int now = CurrentMinuteOfDay();

    if (g_scheduleSuspended) {
        if (Schedule_ShouldResume(g_scheduleSuspendMinute, g_scheduleResumeMinute, now))
            g_scheduleSuspended = FALSE;
        else
            return;
    }

    int value = Schedule_BrightnessAt(g_settings.schedule, g_settings.scheduleCount, now);
    if (value == g_scheduleLastApplied)
        return;

    g_scheduleLastApplied = value;
    g_masterTarget = value;
    TIMED("schedule: SetAllBrightness", Monitor_SetAllBrightness(&g_monitors, value));
    UI_RefreshPopup(g_hwndPopup, &g_monitors);  /* no-op if popup hidden */
}

/* Re-push the intended brightness onto the (freshly re-enumerated) monitors.
   Called after a wake/unlock/display-on rescan, because many displays reset
   their brightness to a default (often 100%) across sleep or DPMS off, and a
   plain re-enumeration only reads that reset value back, it does not restore
   ours. When a schedule is active we force its current value (bypassing the
   "unchanged" guard); otherwise we re-apply the last master target. */
static void ReapplyBrightness(void)
{
    /* Woke up with nobody at the keyboard (display power-on, unlock by another
       session): hold the idle level instead of restoring the full one. */
    if (g_idleDimmed) {
        TIMED("reapply(idle level): SetAllBrightness",
              Monitor_SetAllBrightness(&g_monitors, g_settings.idleDimPercent));
        return;
    }
    if (g_settings.scheduleEnabled && g_settings.scheduleCount > 0 && !g_scheduleSuspended) {
        g_scheduleLastApplied = -1;   /* force a re-push even if the value is unchanged */
        Schedule_ApplyNow();
        return;
    }
    if (g_masterTarget >= 0) {         /* skip if the user never set a level yet */
        TIMED("reapply(master): SetAllBrightness",
              Monitor_SetAllBrightness(&g_monitors, g_masterTarget));
        UI_RefreshPopup(g_hwndPopup, &g_monitors);
    }
}

/* A manual brightness change: hand control back to the user until the next anchor. */
static void Schedule_Suspend(void)
{
    if (!g_settings.scheduleEnabled || g_settings.scheduleCount == 0)
        return;
    int now = CurrentMinuteOfDay();
    g_scheduleSuspended = TRUE;
    g_scheduleSuspendMinute = now;
    g_scheduleResumeMinute =
        Schedule_NextAnchorMinute(g_settings.schedule, g_settings.scheduleCount, now);
    g_scheduleLastApplied = -1;  /* force re-apply after resume */
}

/* Every manual brightness change (hotkey, wheel, slider, preset) goes through
   here: it supersedes the idle level and suspends the schedule. */
static void ManualChange(void)
{
    g_idleDimmed = FALSE;   /* the user just set a level; do not restore over it */
    Schedule_Suspend();
}

/* A popup slider moved. The next hotkey or wheel step starts from
   g_masterTarget, so it must follow the slider: the All Monitors slider sets it
   directly, and a single monitor's slider sets it to what the monitors now
   show. Left alone, the next step jumped back to the level the wheel had last
   set. It is not cleared to -1 instead, because ReapplyBrightness reads -1 as
   "never set" and would skip the restore after a wake. */
static void PopupChange(int masterLevel)
{
    g_masterTarget = (masterLevel >= 0) ? masterLevel : MasterTargetFromMonitors();
    ManualChange();
}

/* ---- Idle auto-dim ---- */

/* Milliseconds since the last keyboard or mouse input in this session. */
static DWORD IdleMilliseconds(void)
{
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    lii.dwTime = 0;
    if (!GetLastInputInfo(&lii))
        return 0;
    DWORD now = GetTickCount();
    DWORD idle = now - lii.dwTime;        /* unsigned math, so the 49-day wrap is fine */
    /* A lumosctl command counts as activity too (see RemoteActivity). */
    if (g_remoteSeen && now - g_lastRemoteTick < idle)
        idle = now - g_lastRemoteTick;
    return idle;
}

/* Reasons to leave the brightness alone even though no input has arrived. */
enum { DIMBLOCK_NONE = 0, DIMBLOCK_FULLSCREEN, DIMBLOCK_CAPTURE };

/* Fullscreen video, presentation mode and a live call all mean somebody is
   watching without touching anything. Checked only when we would dim. */
static int Idle_DimBlocked(void)
{
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
         state == QUNS_PRESENTATION_MODE ||
         state == QUNS_BUSY))
        return DIMBLOCK_FULLSCREEN;

    if (Capture_InUse())
        return DIMBLOCK_CAPTURE;

    return DIMBLOCK_NONE;
}

/* Drop to the configured idle level. g_masterTarget is left untouched so it
   still holds the level to come back to; if the user has never set one we
   recover it from the monitors first, otherwise there is nothing to restore. */
static void Idle_Dim(void)
{
    /* This path is retried every tick for as long as the call or the video
       lasts, so the log records a reason only when the reason changes. */
    static int lastBlock = DIMBLOCK_NONE;
    int block = Idle_DimBlocked();
    if (block != lastBlock) {
        lastBlock = block;
        if (block != DIMBLOCK_NONE)
            DbgLog("Idle dim skipped: %s", block == DIMBLOCK_FULLSCREEN
                   ? "fullscreen or presentation" : "microphone or camera in use");
    }
    if (block != DIMBLOCK_NONE)
        return;
    if (g_masterTarget < 0)
        g_masterTarget = MasterTargetFromMonitors();
    g_idleDimmed = TRUE;
    DbgLog("Idle dim -> %d%% (restore target %d)", g_settings.idleDimPercent, g_masterTarget);
    TIMED("idle dim: SetAllBrightness",
          Monitor_SetAllBrightness(&g_monitors, g_settings.idleDimPercent));
    UI_RefreshPopup(g_hwndPopup, &g_monitors);   /* no-op if the popup is hidden */
}

/* Back to the pre-dim level. Reuses ReapplyBrightness so an active schedule
   recomputes its value for the current time: an idle stretch can span hours. */
static void Idle_Restore(void)
{
    if (!g_idleDimmed)
        return;
    g_idleDimmed = FALSE;   /* cleared first: ReapplyBrightness holds the idle level while set */
    DbgLog("Idle restore -> target %d", g_masterTarget);
    TIMED("idle restore: ReapplyBrightness", ReapplyBrightness());
}

static void Idle_Tick(void)
{
    if (!g_settings.idleDimEnabled) {
        Idle_Restore();   /* setting turned off mid-dim */
        return;
    }
    BOOL idle = IdleMilliseconds() >= (DWORD)g_settings.idleDimMinutes * 60000u;
    if (idle && !g_idleDimmed)       Idle_Dim();
    else if (!idle && g_idleDimmed)  Idle_Restore();
}

/* ---- Switches shared by the tray menu and lumosctl ---- */

static void SetScheduleEnabled(BOOL on)
{
    g_settings.scheduleEnabled = on;
    Settings_Save(&g_settings);
    g_scheduleSuspended = FALSE;      /* re-enable takes effect immediately */
    g_scheduleLastApplied = -1;
    Schedule_ApplyNow();
}

static void SetIdleDimEnabled(BOOL on)
{
    g_settings.idleDimEnabled = on;
    Settings_Save(&g_settings);
    if (!on)
        Idle_Restore();   /* undo an active dim immediately */
}

/* ---- Main Window Proc ---- */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* A newer instance is launching and wants our spot: exit cleanly so it can
       take over. Registered message, so it cannot be a compile-time switch case. */
    if (msg == g_wmTakeover && g_wmTakeover != 0) {
        DestroyWindow(hwnd);   /* -> WM_DESTROY -> PostQuitMessage */
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON: {
        /* NOTIFYICON_VERSION_4: event in LOWORD(lParam), anchor in wParam. */
        POINT anchor = { GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
        switch (LOWORD(lParam)) {
        case NIN_SELECT:          /* left click */
            /* Ignored right after a keyboard select, in case the shell sends
               both for one Enter: the toggle would close the popup again. */
            if (GetTickCount() - g_trayKeySelectTick > TRAY_CLICK_WINDOW_MS)
                UI_TogglePopup(g_hwndPopup, &g_monitors);
            break;
        case NIN_KEYSELECT:
            /* Show, never toggle: Enter can deliver NIN_KEYSELECT twice, and a
               toggle would close the popup again at once. */
            g_trayKeySelectTick = GetTickCount();
            if (!UI_IsPopupVisible(g_hwndPopup))
                UI_ShowPopup(g_hwndPopup, &g_monitors, &anchor, TRUE);
            break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            g_trayRightClickTick = GetTickCount();
            break;
        case WM_CONTEXTMENU:      /* right click, Shift+F10 or the Menu key */
            /* A right click sends button messages just before; without one,
               the menu was opened from the keyboard. The button-up counts too,
               so a long press still reads as a click. */
            ShowContextMenu(hwnd, &anchor,
                            GetTickCount() - g_trayRightClickTick > TRAY_CLICK_WINDOW_MS);
            break;
        }
        return 0;
    }

    case WM_HOTKEY:
        HandleHotkey((int)wParam);
        return 0;

    case WM_APP_WHEEL: {
        int notches = g_wheelPending;
        g_wheelPending = 0;
        if (notches != 0)
            StepWithOsd(notches * g_settings.step);
        return 0;
    }

    case WM_COPYDATA: {
        LRESULT r;
        if (Remote_HandleCopyData(hwnd, wParam, lParam, &r))
            return r;
        break;
    }

    case WM_COMMAND: {
        int cmd = LOWORD(wParam);
        if (cmd >= IDM_PRESET_BASE && cmd < IDM_PRESET_BASE + MAX_PRESETS) {
            ApplyPreset(cmd - IDM_PRESET_BASE);
        } else switch (cmd) {
        case IDM_RESCAN:
            ScheduleRescanFromTrigger(hwnd);
            break;
        case IDM_AUTOSTART: {
            BOOL current = Settings_GetAutostart();
            Settings_SetAutostart(!current);
            g_settings.autostart = !current;
            Settings_Save(&g_settings);
            break;
        }
        case IDM_SCHEDULE_TOGGLE:
            SetScheduleEnabled(!g_settings.scheduleEnabled);
            break;
        case IDM_IDLEDIM_TOGGLE:
            SetIdleDimEnabled(!g_settings.idleDimEnabled);
            break;
        case IDM_SETTINGS:
            UI_ShowSettings(hwnd, &g_settings);
            break;
        case IDM_SETTINGS_SAVED: {
            /* The window already wrote the edited values into g_settings.
               Persist them, then apply the ones with a runtime effect. */
            if (Settings_GetAutostart() != g_settings.autostart)
                Settings_SetAutostart(g_settings.autostart);
            Settings_Save(&g_settings);
            if (!g_settings.idleDimEnabled)
                Idle_Restore();           /* undo an active dim right away */
            g_scheduleSuspended = FALSE;  /* a schedule toggle takes effect now */
            g_scheduleLastApplied = -1;
            Schedule_ApplyNow();
            break;
        }
        case IDM_SCHEDULE_EDIT:
            UI_ShowScheduleEditor(hwnd, &g_settings);
            break;
        case IDM_SCHEDULE_SAVED:
            Settings_Save(&g_settings);
            g_scheduleSuspended = FALSE;
            g_scheduleLastApplied = -1;
            Schedule_ApplyNow();
            break;
        case IDM_ABOUT:
            UI_ShowAbout(hwnd);
            break;
        case IDM_EXIT:
            PostQuitMessage(0);
            break;
        }
        return 0;
    }

    case WM_DISPLAYCHANGE:
        /* Monitor plugged/unplugged (resolution/topology change) */
        DbgLog("display change: %ux%u bpp=%u",
               (unsigned)LOWORD(lParam), (unsigned)HIWORD(lParam), (unsigned)wParam);
        ScheduleRescanThrottled(hwnd);
        return 0;

    case WM_WTSSESSION_CHANGE:
        /* Session unlocked or reconnected: DDC handles may be stale */
        DbgLog("session change: %u", (unsigned)wParam);
        if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT) {
            g_reapplyOnRescan = TRUE;   /* restore brightness after the recovery rescan */
            ScheduleRescanFromTrigger(hwnd);
        }
        return 0;

    case WM_POWERBROADCAST:
        /* Display powered back on (GUID_CONSOLE_DISPLAY_STATE) or system resume */
        if (wParam == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING *pbs = (POWERBROADCAST_SETTING *)lParam;
            if (pbs &&
                IsEqualGUID(&pbs->PowerSetting, &kGuidConsoleDisplayState) &&
                pbs->DataLength >= 1) {
                DbgLog("display power state = %u", (unsigned)pbs->Data[0]);
                if (pbs->Data[0] != 0) {   /* 0 = off, non-zero = on/dimmed */
                    g_reapplyOnRescan = TRUE;
                    ScheduleRescanFromTrigger(hwnd);
                }
            }
        } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            DbgLog("power: APM resume");
            g_reapplyOnRescan = TRUE;   /* wake from sleep: displays often reset brightness */
            ScheduleRescanFromTrigger(hwnd);
        }
        return TRUE;

    case WM_TIMER:
        if (wParam == RESCAN_TIMER_ID) {
            KillTimer(hwnd, RESCAN_TIMER_ID);
            StartRescan(hwnd);
        } else if (wParam == SCHEDULE_TIMER_ID) {
            Schedule_ApplyNow();
        } else if (wParam == IDLE_TIMER_ID) {
            Idle_Tick();
        } else if (wParam == RESCAN_WATCHDOG_TIMER_ID) {
            KillTimer(hwnd, RESCAN_WATCHDOG_TIMER_ID);
            if (g_rescanBusy) {
                /* The worker is stuck inside a display driver call. It cannot be
                   killed safely, so it is left parked and its result will be
                   discarded; what matters is releasing the single-flight guard
                   so the app can rescan again. */
                DbgLog("rescan: worker %lu written off after %lu ms",
                       g_rescanAwaitedGen, GetTickCount() - g_rescanStartTick);
                g_rescanAwaitedGen = 0;
                g_rescanBusy = 0;
                g_rescanPending = FALSE;
                if (++g_rescanWriteOffs <= RESCAN_MAX_WRITEOFFS)
                    ScheduleRescan(hwnd);   /* try once more with a fresh worker */
                else
                    DbgLog("rescan: %d workers hung in a row, waiting for a new trigger",
                           g_rescanWriteOffs);
            }
        } else if (wParam == RESCAN_RETRY_TIMER_ID) {
            KillTimer(hwnd, RESCAN_RETRY_TIMER_ID);
            if (g_hwndPopup && IsWindowVisible(g_hwndPopup))
                SetTimer(hwnd, RESCAN_RETRY_TIMER_ID, RESCAN_AWAIT_DEFER_MS, NULL);
            else
                StartRescan(hwnd);
        }
        return 0;

    case WM_APP_RESCAN_DONE: {
        /* Worker finished enumerating. Swap in the fresh list and rebuild the
           popup here on the UI thread (window ops must not run on the worker). */
        DWORD gen = (DWORD)wParam;
        MonitorList *fresh = (MonitorList *)lParam;

        if (gen != g_rescanAwaitedGen) {
            /* A worker the watchdog wrote off has finally returned. Its handles
               describe a display topology we have already replaced. */
            DbgLog("rescan: discarding late result from worker %lu", gen);
            if (fresh) {
                TIMED("rescan late: cleanup",
                      Monitor_CleanupExcept(fresh, &g_monitors));
                free(fresh);
            }
            return 0;   /* the busy flag belongs to the current worker now */
        }

        KillTimer(hwnd, RESCAN_WATCHDOG_TIMER_ID);
        g_rescanAwaitedGen = 0;
        g_rescanWriteOffs = 0;   /* this worker came back, the driver is answering */
        DbgLog("rescan: worker %lu finished after %lu ms",
               gen, GetTickCount() - g_rescanStartTick);

        /* Reject a placeholder enumeration instead of adopting it: for a few
           seconds after a display returns, Windows reports no monitor at all or
           a generic panel, and adopting that silently kills brightness control
           until the next display event. Retry on a backoff and keep the list we
           have, which is either still valid or about to be replaced anyway. */
        if (fresh && !Monitor_HasControllable(fresh) &&
            (Monitor_HasControllable(&g_monitors) || g_reapplyOnRescan) &&
            g_rescanRetry < RESCAN_MAX_RETRIES) {
            DWORD delay = kRescanBackoffMs[g_rescanRetry++];
            DbgLog("rescan: nothing controllable, retry %d in %lu ms",
                   g_rescanRetry, delay);
            TIMED("rescan rejected: cleanup",
                  Monitor_CleanupExcept(fresh, &g_monitors));
            free(fresh);
            SetTimer(hwnd, RESCAN_RETRY_TIMER_ID, delay, NULL);
            g_rescanBusy = 0;
            return 0;
        }
        g_rescanRetry = 0;

        if (fresh) {
            BOOL recovered;
            int waiting = Monitor_TrackUnanswered(fresh, &g_monitors, &recovered);

            /* Release the handles we are replacing, except any the fresh list
               has acquired again: destroying those would invalidate the list we
               are about to adopt. */
            /* The popup goes first. Destroying it while active finishes any
               key or mouse drag, which writes to the monitor of that row; that
               must happen while the row still means the same monitor and its
               handle is still open. */
            if (g_hwndPopup) DestroyWindow(g_hwndPopup);
            g_hwndPopup = NULL;
            TIMED("rescan done: cleanup",
                  Monitor_CleanupExcept(&g_monitors, fresh));
            g_monitors = *fresh;            /* adopt fresh list (plain struct copy) */
            free(fresh);
            Settings_LoadDeltas(&g_settings, &g_monitors);
            g_hwndPopup = UI_CreatePopup(g_hInst, &g_monitors);
            /* g_idleDimmed is included so a monitor plugged in during an idle
               stretch gets the idle level too, instead of staying bright. */
            /* A monitor that answers again after a wake missed the restore
               that ran without it, so it gets the level now. */
            if (g_reapplyOnRescan || g_idleDimmed || recovered) {
                g_reapplyOnRescan = FALSE;
                /* restore our level after wake/unlock/display-on */
                TIMED("rescan done: ReapplyBrightness", ReapplyBrightness());
            }

            if (waiting > 0) {
                DWORD delay = (g_awaitRetry < RESCAN_MAX_RETRIES)
                              ? kRescanBackoffMs[g_awaitRetry++]
                              : RESCAN_AWAIT_STEADY_MS;
                DbgLog("rescan: %d monitor(s) not answering, retry in %lu ms",
                       waiting, delay);
                SetTimer(hwnd, RESCAN_RETRY_TIMER_ID, delay, NULL);
            } else {
                g_awaitRetry = 0;
            }
        }
        g_rescanBusy = 0;
        if (g_rescanPending) {   /* triggers arrived mid-run: coalesce one more */
            g_rescanPending = FALSE;
            ScheduleRescan(hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
