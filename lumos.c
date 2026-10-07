#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <shlobj.h>
#include <powrprof.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strsafe.h>

#include "resource.h"
#include "monitor.h"
#include "monitor_worker.h"
#include "brightness.h"
#include "ui.h"
#include "presets.h"
#include "capture.h"
#include "ui_monitor_selection.h"
#include "idle_black.h"
#include "idle_activity.h"
#include "brightmap.h"
#include "remote.h"
#include "diagnostics.h"

/* Interactive applications observe their own session's display and presence.
   Defined locally for MinGW versions which omit the GUID constants. */
static const GUID kGuidSessionDisplayStatus =
    { 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };
static const GUID kGuidSessionUserPresence =
    { 0x3c0f4548, 0xc03f, 0x4c4d, { 0xb9, 0xf2, 0x23, 0x7e, 0xde, 0x68, 0x63, 0x76 } };

/* Coalesce the near-simultaneous unlock + display-power triggers into one
   re-enumeration so we don't hammer the DDC/I2C bus. */
#define RESCAN_TIMER_ID     0xB100
#define RESCAN_DEBOUNCE_MS  600

/* Posted by the rescan worker thread when the fresh MonitorList is ready.
   lParam = MonitorList* (heap, adopted and freed by the main thread). */
#define WM_APP_RESCAN_DONE  (WM_APP + 1)

/* Posted by the mouse hook when wheel notches over the tray icon start to
   pile up; the notches themselves are counted in g_wheelPending. */
#define WM_APP_WHEEL        (WM_APP + 4)

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
#define SOURCE_TIMER_ID     0xB105

static HINSTANCE    g_hInst;
static HWND         g_hwndHidden;    /* Hidden top-level window (receives broadcasts + notifications) */
static HWND         g_hwndPopup;
static MonitorList  g_monitors;
static Settings     g_settings;
static NOTIFYICONDATAW g_nid;
static HHOOK        g_mouseHook;
static HPOWERNOTIFY g_hPowerNotify;
static HPOWERNOTIFY g_hPresenceNotify;
static BOOL         g_sessionDisplayKnown;
static DWORD        g_sessionDisplayState;
static BOOL         g_sessionPresenceKnown;
static DWORD        g_sessionPresenceState;
static UINT         g_wmTakeover;     /* cross-process "quit, I'm replacing you" message */
static BOOL         g_scheduleSuspended = FALSE;
static int          g_scheduleSuspendMinute = 0;   /* minute-of-day at suspend */
static int          g_scheduleResumeMinute = 0;    /* next anchor to resume at */
static ULONGLONG    g_scheduleResumeLocalMinute;   /* dated local-time deadline */
static int          g_scheduleLastApplied = -1;    /* last brightness pushed by the schedule */
static int          g_masterTarget;          /* intended base percent, mapped through per-monitor ranges */
static BOOL         g_masterTargetValid;
static BOOL         g_masterTargetExplicit; /* A requested group level, rather than an inferred UI average. */
static BOOL         g_idleDimmed = FALSE;     /* TRUE while the idle level is on the monitors */
static DWORD        g_idleEpoch;
static IdleActivity g_idleActivity;
static int          g_idleLastBlock;

typedef struct {
    volatile LONG busy;          /* one awaited worker; timed-out workers may still finish */
    BOOL pending;               /* a trigger arrived while busy */
    BOOL reapplyBrightness;     /* restore the intended level after wake/unlock */
    DWORD startTick;
    DWORD generation;
    DWORD awaitedGeneration;
    DWORD lastStartTick;
    int retry;
    int writeOffs;
} RescanState;

static RescanState g_rescan;
static int          g_awaitRetry = 0;         /* index into kRescanBackoffMs for unanswered monitors */
static Hotkey       g_hotkeysActive[HOTKEY_COUNT]; /* what RegisterHotKey currently holds */
static int          g_hotkeyStartupFailure = -1;   /* first action we could not register, or -1 */
static BOOL         g_hotkeysSuspended = FALSE;    /* released while Settings captures keys */
static DWORD        g_trayRightClickTick = 0;      /* last right button down or up on the icon */
static DWORD        g_trayKeySelectTick = 0;       /* last NIN_KEYSELECT */
static int          g_wheelPending = 0;            /* tray wheel notches not applied yet (hook and handler share the UI thread) */
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
static BOOL ApplyPresetBrightness(DWORD brightness);
static void ApplyStartupBrightness(BOOL loginLaunch);
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
static void SliderManualChange(int row, int target);
static int MasterTargetFromMonitors(void);
static void Idle_Tick(void);
static void Idle_Restore(void);
static void Idle_Activity(void);
static void UpdateIdleBlack(BOOL enabled, BOOL idle);
static void RestoreIdleMonitor(BrightMonitor *monitor);
static void ResumeSourceMonitor(BrightMonitor *monitor);
static void RestartSourcePolling(void);
static void TryIdleHandoff(BrightMonitor *monitor);
static void ApplyIdleBrightness(void);

static void LogMonitorList(const char *reason)
{
    Diagnostics_Log("INFO", "monitors", "%s count=%d selectedOnly=%d", reason,
                    g_monitors.count, g_monitors.selectedOnly);
    for (int i = 0; i < g_monitors.count; i++) {
        char detail[128];
        StringCchPrintfA(detail, ARRAYSIZE(detail), "%s row=%d", reason, i + 1);
        Diagnostics_MonitorState(&g_monitors.monitors[i], detail);
    }
}

static void LogSettings(const char *reason)
{
    Diagnostics_Log("INFO", "settings",
        "%s ini=\"%ls\" idleEnabled=%d idleMinutes=%d idlePercent=%d sourcePollSeconds=%d "
        "scheduleEnabled=%d schedulePoints=%d autostart=%d step=%d selectedOnly=%d "
        "selectedCount=%d sourceRules=%d blackIdleCount=%d",
        reason, g_settings.iniPath, g_settings.idleDimEnabled, g_settings.idleDimMinutes,
        g_settings.idleDimPercent, g_settings.sourcePollSeconds, g_settings.scheduleEnabled,
        g_settings.scheduleCount, g_settings.autostart, g_settings.step,
        g_settings.monitorSelection.selectedOnly, g_settings.monitorSelection.count,
        g_settings.monitorSelection.inputRuleCount, g_settings.monitorSelection.idleBlackCount);
    for (int i = 0; i < g_settings.scheduleCount; i++)
        Diagnostics_Log("INFO", "schedule", "anchor=%d time=%02d:%02d percent=%d", i + 1,
            g_settings.schedule[i].minutes / 60, g_settings.schedule[i].minutes % 60,
            g_settings.schedule[i].brightness);
    for (int i = 0; i < g_settings.monitorSelection.count; i++)
        Diagnostics_Log("INFO", "settings", "selected monitor=\"%ls\" id=\"%ls\"",
            g_settings.monitorSelection.names[i], g_settings.monitorSelection.keys[i]);
    LogMonitorList(reason);
}

/* A monitor's range changed in the popup. Save it, then put every monitor
   back on the current master level so the change shows at once: matching two
   monitors means adjusting one while looking at both. masterLevel is the
   popup's All Monitors level, used when no level has been set yet. */
static void RangeChanged(int masterLevel)
{
    Settings_StoreRanges(&g_settings, &g_monitors);
    Settings_Save(&g_settings);
    if (g_idleDimmed)
        return;   /* the idle level owns the monitors; the restore uses the new range */
    if (!g_masterTargetValid)
        g_masterTarget = masterLevel;
    g_masterTargetValid = TRUE;
    g_masterTargetExplicit = TRUE;
    UI_SetMasterTarget(g_masterTarget);
    Monitor_SetAllBrightness(&g_monitors, g_masterTarget);
}

/* ---- Entry Point ---- */

/* The Windows Run entry supplies this switch; opening Lumos manually keeps
   the current brightness (or the configured schedule) as before. */
static BOOL IsWindowsLoginLaunch(const WCHAR *commandLine)
{
    int argc = 0;
    BOOL loginLaunch = FALSE;
    if (!commandLine || !commandLine[0]) return FALSE;
    WCHAR **argv = CommandLineToArgvW(commandLine, &argc);
    if (!argv) return FALSE;
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], LUMOS_STARTUP_ARGUMENT) == 0) {
            loginLaunch = TRUE;
            break;
        }
    }
    LocalFree(argv);
    return loginLaunch;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLineA, int showCmd)
{
    (void)hPrev; (void)cmdLineA; (void)showCmd;
    g_hInst = hInst;
    BOOL loginLaunch = IsWindowsLoginLaunch(GetCommandLineW());

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

    Diagnostics_Init();
    Diagnostics_Log("INFO", "app", "launch login=%d", loginLaunch);
    /* Initialize COM for Shell */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* Initialize common controls */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    /* Init subsystems */
    memset(&g_monitors, 0, sizeof(g_monitors));
    Monitor_Enumerate(&g_monitors);
    Settings_Init(&g_settings);
    Settings_UpgradeAutostart(); /* Add the login switch to this exe's legacy Run entry. */
    Settings_ApplyRanges(&g_settings, &g_monitors);
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    LogSettings("loaded");

    if (!UI_Init(hInst)) {
        Diagnostics_Log("ERROR", "app", "UI initialization failed error=0x%08lX", GetLastError());
        MessageBoxW(NULL, L"Failed to initialize UI", APP_NAME, MB_ICONERROR);
        Monitor_Cleanup(&g_monitors);
        Monitor_FlushRetiredHandles();
        CoUninitialize();
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        Diagnostics_Close();
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

    if (!g_hwndHidden || !MonitorWorker_Start(g_hwndHidden, &g_monitors)) {
        Diagnostics_Log("ERROR", "app", "hidden window or monitor worker startup failed error=0x%08lX", GetLastError());
        MessageBoxW(NULL, L"Failed to start monitor worker", APP_NAME, MB_ICONERROR);
        Monitor_Cleanup(&g_monitors);
        Monitor_FlushRetiredHandles();
        UI_Shutdown();
        if (g_hwndHidden) DestroyWindow(g_hwndHidden);
        CoUninitialize();
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        Diagnostics_Close();
        return 1;
    }

    /* Re-enumerate on session unlock and on display power-on. Windows does not
       reliably send WM_DISPLAYCHANGE across a lock screen, so cached DDC handles
       go stale; these notifications trigger a rescan to re-acquire them. */
    IdleBlack_Init(hInst, g_hwndHidden);
    UpdateIdleBlack(g_settings.idleDimEnabled, FALSE);
    WTSRegisterSessionNotification(g_hwndHidden, NOTIFY_FOR_THIS_SESSION);
    g_hPowerNotify = RegisterPowerSettingNotification(
        g_hwndHidden, &kGuidSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_hPresenceNotify = RegisterPowerSettingNotification(
        g_hwndHidden, &kGuidSessionUserPresence, DEVICE_NOTIFY_WINDOW_HANDLE);

    /* Create popup (hidden) */
    g_hwndPopup = UI_CreatePopup(hInst, &g_monitors);
    UI_SetRangeChangeCallback(RangeChanged);
    static const HotkeyHost hotkeyHost = { ApplyHotkeys, SuspendHotkeys, FirstFailedHotkey };
    UI_SetHotkeyHost(&hotkeyHost);
    Remote_Init(&kAppControl);

    /* Tray icon, hotkeys, mouse hook */
    CreateTrayIcon(g_hwndHidden);
    RegisterHotkeys(g_hwndHidden);
    InstallMouseHook();

    /* Login starts at Day until the next schedule anchor. Manual launches
       retain the existing schedule behavior. */
    UI_SetManualChangeCallback(SliderManualChange);
    SetTimer(g_hwndHidden, SCHEDULE_TIMER_ID, SCHEDULE_TICK_MS, NULL);
    ApplyStartupBrightness(loginLaunch);

    /* Idle auto-dim tick. Always armed: the handler returns at once when the
       feature is off, which keeps enable/disable free of timer bookkeeping. */
    SetTimer(g_hwndHidden, IDLE_TIMER_ID, IDLE_TICK_MS, NULL);
    RestartSourcePolling();

    /* Message loop */
    MSG msg = { 0 };
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (UI_HandleDialogMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    int exitCode = (int)msg.wParam;

    Diagnostics_Log("INFO", "app", "shutdown exitCode=%d", exitCode);
    /* Cleanup */
    IdleBlack_Shutdown();
    RemoveMouseHook();
    UnregisterHotkeys(g_hwndHidden);
    if (g_hPowerNotify) UnregisterPowerSettingNotification(g_hPowerNotify);
    if (g_hPresenceNotify) UnregisterPowerSettingNotification(g_hPresenceNotify);
    WTSUnRegisterSessionNotification(g_hwndHidden);
    RemoveTrayIcon();
    if (g_hwndPopup) DestroyWindow(g_hwndPopup);
    Monitor_Cleanup(&g_monitors);
    MonitorWorker_Stop();
    while (PeekMessageW(&msg, g_hwndHidden, WM_MONITOR_RESULT, WM_MONITOR_RESULT, PM_REMOVE))
        free((void *)msg.lParam);
    UI_Shutdown();
    CoUninitialize();
    Diagnostics_Close();
    /* Release before closing so a replacing instance sees WAIT_OBJECT_0 promptly
       instead of waiting for abandonment. */
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return exitCode;
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

#define DbgLog(...) Diagnostics_Log("INFO", "app", __VA_ARGS__)

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
        return PtInRect(&rcIcon, ptPhysical);
    }
    DbgLog("GetRect FAILED hr=0x%08X hwnd=%p id=%u",
           (unsigned)hr, (void*)nii.hWnd, nii.uID);
    return FALSE;
}

static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT *mhs = (MSLLHOOKSTRUCT *)lParam;
        if (IsCursorOverTrayIcon(mhs->pt)) {
            DbgLog("Tray mouse wheel: mouseData=0x%08X", (unsigned)mhs->mouseData);
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
   map each selected reading back through its range and average the results. */
static int MasterTargetFromMonitors(void)
{
    return Brightness_MasterTarget(&g_monitors);
}

/* Move the master level by delta, the way the All Monitors slider would.
   Shared by the hotkeys, the tray wheel and lumosctl; the OSD is the caller's. */
static BOOL StepMaster(int delta)
{
    /* Initialize target from current state if needed */
    if (!g_masterTargetValid && !g_idleDimmed)
        g_masterTarget = MasterTargetFromMonitors();
    g_masterTargetValid = TRUE;
    g_masterTargetExplicit = TRUE;

    g_masterTarget += delta;
    if (g_masterTarget < 0) g_masterTarget = 0;
    if (g_masterTarget > 100) g_masterTarget = 100;
    UI_SetMasterTarget(g_masterTarget);

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
    if (!Monitor_HasSelected(&g_monitors)) return;
    StepMaster(delta);

    /* The OSD shows the All Monitors level, the value the step just moved.
       The level of the monitor under the cursor stops at the ends of that
       monitor's range (50% for a range of 50-100) and looks stuck there. */
    POINT curPos;
    GetCursorPos(&curPos);
    HMONITOR hCurMon = MonitorFromPoint(curPos, MONITOR_DEFAULTTOPRIMARY);
    int selected = -1;
    for (int i = 0; i < g_monitors.count; i++) {
        BrightMonitor *monitor = &g_monitors.monitors[i];
        if (!Monitor_CanControl(monitor) || !Monitor_SourceAllowsControl(monitor)) continue;
        if (selected < 0) selected = i;
        if (monitor->hMonitor == hCurMon) {
            selected = i;
            break;
        }
    }
    if (selected >= 0)
        UI_ShowOSD(g_hInst, g_monitors.monitors[selected].hMonitor, g_masterTarget,
                   !UI_IsPopupVisible(g_hwndPopup));
}

static void HandleHotkey(int id)
{
    Diagnostics_Log("INFO", "hotkey", "id=%d step=%d", id, g_settings.step);
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
            MONITORINFO mi = { 0 };
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            POINT center = { (mi.rcWork.left + mi.rcWork.right) / 2,
                             (mi.rcWork.top + mi.rcWork.bottom) / 2 };
            UI_ShowPopup(g_hwndPopup, &g_monitors, &center, TRUE);
        }
        return;
    default: return;
    }

    if (Monitor_HasSelected(&g_monitors))
        StepWithOsd(delta);
    else
        Diagnostics_Log("INFO", "hotkey", "SKIP reason=no-controllable-selected-monitor");
}

/* ---- Apply Preset ---- */

static BOOL ApplyPreset(int index)
{
    if (index < 0 || index >= g_settings.presetCount) return FALSE;
    Diagnostics_Log("INFO", "preset", "index=%d name=\"%ls\" percent=%lu", index,
                    g_settings.presets[index].name, g_settings.presets[index].brightness);
    return ApplyPresetBrightness(g_settings.presets[index].brightness);
}

static BOOL ApplyPresetBrightness(DWORD brightness)
{
    g_masterTarget = (int)brightness;
    g_masterTargetValid = TRUE;
    g_masterTargetExplicit = TRUE;
    UI_SetMasterTarget(g_masterTarget);
    BOOL ok = Monitor_SetAllBrightness(&g_monitors, g_masterTarget);
    Monitor_RefreshBrightness(&g_monitors);
    UI_RefreshPopup(g_hwndPopup, &g_monitors);

    ManualChange();
    return ok;
}

static void ApplyStartupBrightness(BOOL loginLaunch)
{
    if (!loginLaunch) {
        Schedule_ApplyNow();
        return;
    }

    /* Treat Day as a preset selection, including per-monitor ranges and
       suspension of the schedule until its next anchor. */
    ApplyPresetBrightness((DWORD)Settings_DayBrightness(&g_settings));

    /* Displays can still be reconnecting at login. The existing bounded
       rescan/retry path re-applies the latest intent, so a later manual,
       schedule or idle change always takes precedence over this startup value. */
    g_rescan.reapplyBrightness = TRUE;
    ScheduleRescanFromTrigger(g_hwndHidden);
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
    Idle_Activity();
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
    g_masterTargetValid = TRUE;
    g_masterTargetExplicit = TRUE;
    UI_SetMasterTarget(g_masterTarget);
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
    if (ok) SliderManualChange(index, percent);
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

/* Release an enumeration that was rejected or could not be delivered. */
static void FreeMonitorList(MonitorList *list)
{
    if (!list) return;
    Monitor_Cleanup(list);
    free(list);
}

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
    if (!PostMessageW(hwnd, WM_APP_RESCAN_DONE, (WPARAM)gen, (LPARAM)fresh))
        FreeMonitorList(fresh);
    return 0;
}

/* Launch a single-flight rescan. Called only on the main thread. If one is
   already running, remember to run once more when it finishes (coalesces the
   burst of triggers that a single unlock produces). */
static void StartRescan(HWND hwnd)
{
    if (InterlockedCompareExchange(&g_rescan.busy, 1, 0) != 0) {
        g_rescan.pending = TRUE;
        return;
    }
    RescanArgs *args = (RescanArgs *)malloc(sizeof(RescanArgs));
    if (!args) {
        g_rescan.busy = 0;
        return;
    }
    args->hwnd = hwnd;
    args->gen = ++g_rescan.generation;
    g_rescan.awaitedGeneration = args->gen;
    DWORD generation = args->gen; /* worker frees args as soon as it starts */
    g_rescan.startTick = GetTickCount();

    HANDLE h = CreateThread(NULL, 0, RescanThreadProc, args, 0, NULL);
    if (h) {
        CloseHandle(h);
        DbgLog("rescan: worker %lu launched", generation);
        g_rescan.lastStartTick = g_rescan.startTick;
        SetTimer(hwnd, RESCAN_WATCHDOG_TIMER_ID, RESCAN_WATCHDOG_MS, NULL);
    } else {
        free(args);
        g_rescan.awaitedGeneration = 0;
        g_rescan.busy = 0;   /* launch failed; keep the current list */
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
    g_rescan.writeOffs = 0;
    g_rescan.retry = 0;
    g_awaitRetry = 0;
    DWORD since = GetTickCount() - g_rescan.lastStartTick;
    DWORD delay = (since >= RESCAN_MIN_INTERVAL_MS)
                  ? RESCAN_DEBOUNCE_MS
                  : RESCAN_MIN_INTERVAL_MS - since;
    SetTimer(hwnd, RESCAN_TIMER_ID, delay, NULL);
}

/* A display change, an unlock or a manual re-scan is a fresh start: clear the
   counters that stopped us retrying on our own. */
static void ScheduleRescanFromTrigger(HWND hwnd)
{
    g_rescan.writeOffs = 0;
    g_rescan.retry = 0;
    g_awaitRetry = 0;
    KillTimer(hwnd, RESCAN_RETRY_TIMER_ID);   /* a pending backoff is now moot */
    ScheduleRescan(hwnd);
}

/* ---- Schedule runtime ---- */

/* Use FILETIME arithmetic on local calendar fields, without timezone conversion:
   schedule anchors are wall-clock times. Keeping the date also handles a single
   anchor at the current minute (next occurrence is tomorrow) and long sleeps. */
static ULONGLONG CurrentLocalMinute(int *minuteOfDay)
{
    SYSTEMTIME st;
    FILETIME ft;
    GetLocalTime(&st);
    *minuteOfDay = st.wHour * 60 + st.wMinute;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 600000000ULL;
}

/* Push the schedule's brightness for the current time, unless suspended,
   disabled, or empty. Applies only when the value changed (less DDC traffic). */
static void Schedule_ApplyNow(void)
{
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) {
        g_rescan.reapplyBrightness = TRUE;
        return;
    }
    Diagnostics_Log("INFO", "schedule", "check enabled=%d points=%d idle=%d suspended=%d lastApplied=%d",
        g_settings.scheduleEnabled, g_settings.scheduleCount, g_idleDimmed,
        g_scheduleSuspended, g_scheduleLastApplied);
    if (!g_settings.scheduleEnabled || g_settings.scheduleCount == 0)
        return;

    /* The idle level owns the monitors right now. Leave it alone and force a
       re-push once the user is back, since by then the schedule value for the
       current time may equal the last one we applied. */
    if (g_idleDimmed) {
        g_scheduleLastApplied = -1;
        return;
    }

    int now;
    ULONGLONG localMinute = CurrentLocalMinute(&now);

    if (g_scheduleSuspended) {
        BOOL resume = g_scheduleResumeLocalMinute && localMinute
            ? localMinute >= g_scheduleResumeLocalMinute
            : Schedule_ShouldResume(g_scheduleSuspendMinute, g_scheduleResumeMinute, now);
        if (resume)
            g_scheduleSuspended = FALSE;
        else
            return;
    }

    int value = Schedule_BrightnessAt(g_settings.schedule, g_settings.scheduleCount, now);
    if (value == g_scheduleLastApplied)
        return;

    g_scheduleLastApplied = value;
    Diagnostics_Log("INFO", "schedule", "APPLY minute=%d basePercent=%d", now, value);
    g_masterTarget = value;
    g_masterTargetValid = TRUE;
    g_masterTargetExplicit = TRUE;
    UI_SetMasterTarget(value);
    TIMED("schedule: SetAllBrightness", Monitor_SetAllBrightness(&g_monitors, value));
    UI_RefreshPopup(g_hwndPopup, &g_monitors);  /* no-op if popup hidden */
}

/* Re-push the intended brightness onto the (freshly re-enumerated) monitors.
   Called after a wake/unlock/display-on rescan, because many displays reset
   their brightness to a default (often 100%) across sleep or DPMS off, and a
   plain re-enumeration only reads that reset value back, it does not restore
   ours. When a schedule is active we force its current value (bypassing the
   "unchanged" guard); otherwise each retained monitor intent wins over the
   master fallback. An acknowledged idle baseline remains native raw state. */
static void ReapplyBrightness(void)
{
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) {
        g_rescan.reapplyBrightness = TRUE;
        return;
    }
    Diagnostics_Log("INFO", "policy", "reapply idle=%d masterValid=%d masterTarget=%d scheduleSuspended=%d",
                    g_idleDimmed, g_masterTargetValid, g_masterTarget, g_scheduleSuspended);
    /* Woke up with nobody at the keyboard (display power-on, unlock by another
       session): hold the idle level instead of restoring the full one. */
    if (g_idleDimmed) {
        UI_SetMasterTarget(g_settings.idleDimPercent);
        TIMED("reapply(idle level)", ApplyIdleBrightness());
        return;
    }
    if (g_settings.scheduleEnabled && g_settings.scheduleCount > 0 && !g_scheduleSuspended) {
        g_scheduleLastApplied = -1;   /* force a re-push even if the value is unchanged */
        Schedule_ApplyNow();
        return;
    }
    if (g_masterTargetValid) UI_SetMasterTarget(g_masterTarget);
    for (int i = 0; i < g_monitors.count; i++) {
        BrightMonitor *monitor = &g_monitors.monitors[i];
        if (!Monitor_CanControl(monitor)) continue;
        if (monitor->desiredBrightnessValid) {
            Monitor_SetBrightness(monitor, monitor->desiredBrightness);
        } else if (monitor->idleApplied && monitor->preIdleBrightnessValid) {
            if (!monitor->idleReleasePending) RestoreIdleMonitor(monitor);
        } else if (g_masterTargetValid && g_masterTargetExplicit) {
            int percent = BrightMap_Level(g_masterTarget, monitor->rangeLo, monitor->rangeHi);
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            Monitor_SetBrightness(monitor, (DWORD)percent);
        }
    }
    UI_RefreshPopup(g_hwndPopup, &g_monitors);
}

/* Put the monitors in mask (bit i = monitor i) on the current All Monitors
   level, leaving the others alone. Skipped when no level has been set yet. */
static void ApplyMasterTo(unsigned mask)
{
    for (int i = 0; i < g_monitors.count; i++) {
        BrightMonitor *mon = &g_monitors.monitors[i];
        if (mask & (1u << i))
            ResumeSourceMonitor(mon);
    }
    UI_RefreshPopup(g_hwndPopup, &g_monitors);
}

/* A manual brightness change: hand control back to the user until the next anchor. */
static void Schedule_Suspend(void)
{
    if (!g_settings.scheduleEnabled || g_settings.scheduleCount == 0)
        return;
    int now;
    ULONGLONG localMinute = CurrentLocalMinute(&now);
    g_scheduleSuspended = TRUE;
    g_scheduleSuspendMinute = now;
    g_scheduleResumeMinute =
        Schedule_NextAnchorMinute(g_settings.schedule, g_settings.scheduleCount, now);
    int untilResume = g_scheduleResumeMinute - now;
    if (untilResume <= 0) untilResume += 1440;
    g_scheduleResumeLocalMinute = localMinute ? localMinute + (ULONGLONG)untilResume : 0;
    g_scheduleLastApplied = -1;  /* force re-apply after resume */
    Diagnostics_Log("INFO", "schedule", "SUSPEND currentMinute=%d resumeMinute=%d", now, g_scheduleResumeMinute);
}

/* Every manual brightness change (hotkey, wheel, slider, preset) goes through
   here: it supersedes the idle level and suspends the schedule. */
static void ManualChange(void)
{
    Diagnostics_Log("INFO", "manual", "user brightness change master=%d previousIdle=%d", g_masterTarget, g_idleDimmed);
    g_idleDimmed = FALSE;   /* the user just set a level; do not restore over it */
    IdleBlack_Clear();
    Schedule_Suspend();
    for (int i = 0; i < g_monitors.count; i++)
        TryIdleHandoff(&g_monitors.monitors[i]);
}

static void SliderManualChange(int row, int target)
{
    if (row >= 0 && row < g_monitors.count)
        Diagnostics_Monitor(&g_monitors.monitors[row], "INFO", "slider", "manual row=%d percent=%d", row + 1, target);
    else
        Diagnostics_Log("INFO", "slider", "manual masterPercent=%d", target);
    /* Filtered requests do not optimistically overwrite confirmed brightness.
       Derive master intent from a temporary view of the requested row instead. */
    MonitorList intended = g_monitors;
    for (int i = 0; i < intended.count; i++) {
        BrightMonitor *monitor = &intended.monitors[i];
        if (monitor->sourceFilter && monitor->desiredBrightnessValid)
            monitor->brightnessCur = Brightness_ToRaw(monitor, monitor->desiredBrightness);
    }
    if (row >= 0 && row < intended.count)
        intended.monitors[row].brightnessCur = Brightness_ToRaw(&intended.monitors[row], (DWORD)target);
    g_masterTarget = row < 0 ? target : Brightness_MasterTarget(&intended);
    g_masterTargetValid = TRUE;
    g_masterTargetExplicit = row < 0;
    UI_SetMasterTarget(g_masterTarget);
    ManualChange();
}

/* ---- Idle auto-dim ---- */

/* Milliseconds since the last keyboard or mouse input in this session. */
static DWORD IdleMilliseconds(void)
{
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    lii.dwTime = 0;
    if (!GetLastInputInfo(&lii)) {
        Diagnostics_Log("ERROR", "idle", "GetLastInputInfo FAILED error=0x%08lX", GetLastError());
        return IdleActivity_Sample(&g_idleActivity, GetTickCount64(), FALSE, 0);
    }
    return IdleActivity_Sample(&g_idleActivity, GetTickCount64(), TRUE, lii.dwTime);
}

/* Input, CLI actions and overlay wake all restart the same monotonic clock. */
static void Idle_Activity(void)
{
    IdleActivity_Record(&g_idleActivity, GetTickCount64());
    if (!g_sessionDisplayKnown || g_sessionDisplayState != 0)
        Idle_Restore();
}

static void UpdateIdleBlack(BOOL enabled, BOOL idle)
{
    enabled = enabled && (!g_sessionDisplayKnown || g_sessionDisplayState != 0);
    if (idle && g_idleActivity.inputValid)
        IdleBlack_UpdateForInput(&g_monitors, enabled, idle, g_idleActivity.inputTick);
    else
        IdleBlack_Update(&g_monitors, enabled, idle);
}

/* Reasons to leave the brightness alone even though no input has arrived. */
enum { DIMBLOCK_NONE = 0, DIMBLOCK_FULLSCREEN, DIMBLOCK_CAPTURE, DIMBLOCK_DISPLAY_REQUEST };

/* Fullscreen video, presentation mode and a live call all mean somebody is
   watching without touching anything. Also checked while already dimmed. */
static int Idle_DimBlocked(void)
{
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        BOOL blackActive = IdleBlack_Active();
        /* The full-monitor black cover itself makes the shell report BUSY.
           Keep explicit D3D/presentation exclusions, and verify other visible
           fullscreen content before letting BUSY undo our own idle cover. */
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE ||
            (state == QUNS_BUSY && (!blackActive || IdleBlack_HasExternalFullscreen()))) {
            if (g_idleLastBlock != DIMBLOCK_FULLSCREEN)
                Diagnostics_Log("INFO", "idle", "fullscreen exclusion shellState=%d ownBlackActive=%d", state, blackActive);
            return DIMBLOCK_FULLSCREEN;
        }
    }

    if (Capture_InUse())
        return DIMBLOCK_CAPTURE;

    /* This public query is aggregate: it cannot identify request owners.
       An OLED request from this process would otherwise inhibit our own idle
       forever. Keep the existing fullscreen/capture policy in that mode. */
    if (!IdleBlack_HoldsDisplayRequest()) {
        ULONG executionState = 0;
        if (CallNtPowerInformation(SystemExecutionState, NULL, 0,
                                  &executionState, sizeof(executionState)) == 0 &&
            (executionState & ES_DISPLAY_REQUIRED))
            return DIMBLOCK_DISPLAY_REQUEST;
    }

    return DIMBLOCK_NONE;
}

/* Drop to the configured idle level. g_masterTarget is left untouched so it
   still holds the level to come back to; if the user has never set one we
   recover it from the monitors first, otherwise there is nothing to restore. */
static void DimIdleMonitor(BrightMonitor *monitor)
{
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) return;
    if (monitor->idleBlack) {
        /* Changing to black mode cannot leave an earlier acknowledged LCD
           dim on the panel, including an acknowledgement which arrives late. */
        if (!monitor->idleReleasePending) RestoreIdleMonitor(monitor);
        return;
    }
    if (!Monitor_CanControl(monitor) || monitor->idleDimPending) {
        Diagnostics_Monitor(monitor, "INFO", "idle-dim", "SKIP reason=%s sourcePolicy=%s",
            monitor->excludedFromControl ? "monitor-excluded" : monitor->idleBlack ? "true-black-overlay" :
            !monitor->controllable ? "brightness-unavailable" : "dim-already-pending",
            Diagnostics_SourceReason(monitor));
        return;
    }
    if (!monitor->idleEpoch) {
        if (++g_idleEpoch == 0) ++g_idleEpoch;
        monitor->idleEpoch = g_idleEpoch;
    }
    int percent = BrightMap_Level(g_settings.idleDimPercent, monitor->rangeLo, monitor->rangeHi);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* A new dim replaces any queued release for this monitor. */
    BOOL releasePending = monitor->idleReleasePending;
    monitor->idleReleasePending = FALSE;
    monitor->idleDimPending = TRUE;
    BOOL asynchronous = MonitorWorker_Running();
    Diagnostics_Monitor(monitor, "INFO", "idle-dim", "QUEUE percent=%d epoch=%lu asynchronous=%d sourcePolicy=%s",
                        percent, monitor->idleEpoch, asynchronous, Diagnostics_SourceReason(monitor));
    if (!Monitor_SetIdleBrightness(monitor, (DWORD)percent)) {
        Diagnostics_Monitor(monitor, "ERROR", "idle-dim", "REQUEST FAILED");
        monitor->idleDimPending = FALSE;
        monitor->idleReleasePending = releasePending;
    } else if (!asynchronous) {
        monitor->idleDimPending = FALSE;
        monitor->idleApplied = TRUE;
    }
}

static void ApplyIdleBrightness(void)
{
    for (int i = 0; i < g_monitors.count; i++)
        DimIdleMonitor(&g_monitors.monitors[i]);
    UpdateIdleBlack(g_settings.idleDimEnabled, g_idleDimmed);
}

/* This is the sole exception to the normal PC-input filter: undo our own
   successful idle dim on the exact alternate input just observed. */
static void TryIdleHandoff(BrightMonitor *monitor)
{
    if (!Monitor_CanControl(monitor) || !monitor->sourceFilter ||
        !monitor->expectedInput || !monitor->sourceKnown ||
        monitor->currentInput == monitor->expectedInput ||
        !monitor->idleApplied || !monitor->preIdleBrightnessValid ||
        monitor->idleReleasePending) {
        if (monitor->sourceFilter && (!monitor->sourceKnown || monitor->currentInput != monitor->expectedInput)) {
            const char *reason = !Monitor_CanControl(monitor) ? "monitor-excluded-or-brightness-unavailable" :
                !monitor->expectedInput ? "PC-input-unassigned" : !monitor->sourceKnown ? "input-unavailable" :
                !monitor->idleApplied ? "no-acknowledged-idle-write" :
                !monitor->preIdleBrightnessValid ? "no-original-brightness" : "restore-already-pending";
            Diagnostics_Monitor(monitor, "INFO", "idle-handoff", "SKIP reason=%s idleApplied=%d baselineValid=%d releasePending=%d",
                reason, monitor->idleApplied, monitor->preIdleBrightnessValid, monitor->idleReleasePending);
        }
        return;
    }
    Diagnostics_Monitor(monitor, "INFO", "idle-handoff", "RESTORE originalRaw=%lu otherInput=0x%02lX epoch=%lu pcStillIdle=%d",
        monitor->preIdleBrightness, monitor->currentInput, monitor->idleEpoch, g_idleDimmed);
    monitor->idleReleasePending = TRUE;
    monitor->idleDimPending = FALSE; /* This request replaces any queued dim. */
    BOOL asynchronous = MonitorWorker_Running();
    if (!Monitor_ReleaseIdleBrightness(monitor, monitor->preIdleBrightness, monitor->currentInput)) {
        Diagnostics_Monitor(monitor, "ERROR", "idle-handoff", "restore request FAILED; retry on next source poll");
        monitor->idleReleasePending = FALSE;
    } else if (!asynchronous) {
        monitor->idleReleasePending = FALSE;
        monitor->idleApplied = FALSE;
        if (!g_idleDimmed) monitor->preIdleBrightnessValid = FALSE;
    }
}

static void Idle_Dim(void)
{
    int block = Idle_DimBlocked();
    if (block != DIMBLOCK_NONE)
        return;
    for (int i = 0; i < g_monitors.count; i++) {
        BrightMonitor *monitor = &g_monitors.monitors[i];
        /* An outstanding old dim/release still owns its original baseline. */
        /* Black-only cycles issue no panel dim. Retain the prior LCD epoch so
           a canceled in-flight dim can still acknowledge its exact baseline. */
        if (monitor->idleBlack) continue;
        if (!monitor->idleApplied && !monitor->idleDimPending && !monitor->idleReleasePending) {
            monitor->preIdleBrightnessValid = FALSE;
            monitor->idleEpoch = 0;
        }
    }
    if (!g_masterTargetValid)
        g_masterTarget = Brightness_MasterTarget(&g_monitors);
    g_idleDimmed = TRUE;
    UI_SetMasterTarget(g_settings.idleDimPercent);
    DbgLog("Idle dim -> %d%% (restore target %d)", g_settings.idleDimPercent, g_masterTarget);
    TIMED("idle dim", ApplyIdleBrightness());
    UI_RefreshPopup(g_hwndPopup, &g_monitors);   /* no-op if the popup is hidden */
}

/* Manual wake restores individual intent, or the exact native baseline when
   no brightness has been requested. The inferred master is only a UI value. */
static void RestoreIdleMonitor(BrightMonitor *monitor)
{
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) return;
    if (!Monitor_CanControl(monitor) ||
        (!monitor->idleApplied && !monitor->idleDimPending && !monitor->idleReleasePending))
        return;
    if (monitor->desiredBrightnessValid) {
        monitor->idleReleasePending = FALSE;
        Monitor_SetBrightness(monitor, monitor->desiredBrightness);
    } else if (Monitor_SourceAllowsControl(monitor)) {
        if (monitor->idleApplied && monitor->preIdleBrightnessValid) {
            monitor->idleReleasePending = TRUE;
            monitor->idleDimPending = FALSE;
            if (!Monitor_RestoreIdleBrightness(monitor)) monitor->idleReleasePending = FALSE;
        } else if (monitor->idleDimPending) {
            /* No baseline is known until the pending dim acknowledges. Cancel
               it; any write already completed still delivers its owned raw
               baseline and is restored by RetryIdleRestores on the next tick. */
            MonitorWorker_Cancel(monitor);
            monitor->idleDimPending = FALSE;
        }
    }
}

/* An active schedule recomputes its current value: idle can span hours.
   Otherwise, wake must not flatten individual monitor targets to the master. */
static void Idle_Restore(void)
{
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) return;
    if (!g_idleDimmed)
        return;
    g_idleDimmed = FALSE;   /* cleared first: ReapplyBrightness holds the idle level while set */
    IdleBlack_Clear();
    DbgLog("Idle restore -> target %d", g_masterTarget);
    if (g_settings.scheduleEnabled && g_settings.scheduleCount > 0 && !g_scheduleSuspended) {
        for (int i = 0; i < g_monitors.count; i++)
            g_monitors.monitors[i].idleReleasePending = FALSE;
        TIMED("idle restore: ReapplyBrightness", ReapplyBrightness());
    } else {
        /* Black idle never changed panel brightness; removing its window is
           sufficient. Only restore brightness on displays which used dimming. */
        UI_SetMasterTarget(g_masterTarget);
        for (int i = 0; i < g_monitors.count; i++)
            RestoreIdleMonitor(&g_monitors.monitors[i]);
        UI_RefreshPopup(g_hwndPopup, &g_monitors);
    }
    for (int i = 0; i < g_monitors.count; i++) {
        /* Requeue an alternate-input undo after waking; a normal raw restore
           must keep its pending flag until its own acknowledgement arrives. */
        if (!Monitor_SourceAllowsControl(&g_monitors.monitors[i]))
            g_monitors.monitors[i].idleReleasePending = FALSE;
        TryIdleHandoff(&g_monitors.monitors[i]);
    }
    UpdateIdleBlack(g_settings.idleDimEnabled, FALSE);
}

/* Removing the black cover is immediate, but a dimmed panel may reject its
   restore. Keep its acknowledged idle ownership until a write succeeds, and
   retry the latest policy without touching displays which already recovered. */
static void RetryIdleRestores(void)
{
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) return;
    MonitorTarget pending[MAX_MONITORS];
    DWORD pendingMask = MonitorWorker_PendingTargets(pending);
    for (int i = 0; i < g_monitors.count; i++) {
        BrightMonitor *monitor = &g_monitors.monitors[i];
        if (!monitor->idleApplied || (g_idleDimmed && !monitor->idleBlack) ||
            monitor->idleReleasePending ||
            !Monitor_CanControl(monitor) || !Monitor_SourceAllowsControl(monitor) ||
            (pendingMask & (1u << i))) continue;
        Diagnostics_Monitor(monitor, "WARN", "idle-restore",
            "RETRY acknowledged dim still applied; resume latest brightness policy");
        if (monitor->idleBlack) RestoreIdleMonitor(monitor);
        else ResumeSourceMonitor(monitor);
    }
}

static void Idle_Tick(void)
{
    DWORD idleMs = IdleMilliseconds();
    Diagnostics_Log("INFO", "idle", "tick enabled=%d noInputMs=%lu thresholdMs=%lu dimmed=%d masterValid=%d master=%d",
        g_settings.idleDimEnabled, idleMs, (DWORD)g_settings.idleDimMinutes * 60000u,
        g_idleDimmed, g_masterTargetValid, g_masterTarget);
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) {
        /* Do not send idle brightness writes to a display Windows has turned
           off. Retain any acknowledged baseline for recovery after power-on. */
        UpdateIdleBlack(FALSE, FALSE);
        return;
    }
    if (!g_settings.idleDimEnabled) {
        g_idleLastBlock = DIMBLOCK_NONE;
        Idle_Restore();   /* setting turned off mid-dim */
        RetryIdleRestores();
        UpdateIdleBlack(FALSE, FALSE);
        return;
    }
    DWORD threshold = (DWORD)g_settings.idleDimMinutes * 60000u;
    /* The expensive capture lookup is needed at a dim decision, throughout
       dimming, and while waiting for an existing exclusion to finish. */
    int block = (idleMs >= threshold || g_idleDimmed || g_idleLastBlock != DIMBLOCK_NONE)
                ? Idle_DimBlocked() : DIMBLOCK_NONE;
    if (block != g_idleLastBlock) {
        static const char *const reasons[] = { "none", "fullscreen", "capture", "display-request" };
        Diagnostics_Log("INFO", "idle", "exclusion changed previous=%d current=%d reason=%s",
                        g_idleLastBlock, block, reasons[block]);
        if (block == DIMBLOCK_NONE && g_idleLastBlock != DIMBLOCK_NONE) {
            Idle_Activity(); /* Give the user a full timeout after video/call ends. */
            idleMs = 0;
        }
        g_idleLastBlock = block;
    }
    BOOL idle = block == DIMBLOCK_NONE && idleMs >= threshold;
    if (idle && !g_idleDimmed)       Idle_Dim();
    else if (!idle && g_idleDimmed)  Idle_Restore();
    RetryIdleRestores();
    UpdateIdleBlack(TRUE, g_idleDimmed);
}

/* ---- Main-thread message handlers ---- */

static void RestartSchedule(void)
{
    g_scheduleSuspended = FALSE;
    g_scheduleLastApplied = -1;
    Schedule_ApplyNow();
}

static void RestartSourcePolling(void)
{
    g_settings.sourcePollSeconds = Settings_ClampSourcePollSeconds(g_settings.sourcePollSeconds);
    UINT interval = (UINT)g_settings.sourcePollSeconds * 1000u;
    UI_MonitorSelectionSetSourcePollInterval(interval);
    UINT_PTR armed = SetTimer(g_hwndHidden, SOURCE_TIMER_ID, interval, NULL);
    Diagnostics_Log(armed ? "INFO" : "ERROR", "source-timer", "configured intervalMs=%u armed=%d error=0x%08lX",
                    interval, armed != 0, armed ? ERROR_SUCCESS : GetLastError());
}

/* A scope change cancels queued writes and any drag against the previous
   selection. Rebase manual controls on the newly selected displays so the
   first hotkey does not jump to the previous group's brightness. */
static void UpdateMonitorSelection(void)
{
    MonitorList scoped = g_monitors;
    Settings_ApplyMonitorSelection(&g_settings, &scoped);
    BOOL scopeChanged = scoped.selectedOnly != g_monitors.selectedOnly;
    BOOL ruleChanged = FALSE;
    DWORD rulesChanged = 0;
    DWORD retained = 0;
    for (int i = 0; i < scoped.count; i++) {
        if (scoped.monitors[i].excludedFromControl != g_monitors.monitors[i].excludedFromControl)
            scopeChanged = TRUE;
        if (scoped.monitors[i].idleBlack != g_monitors.monitors[i].idleBlack)
            scopeChanged = TRUE; /* Changing idle mode is user activity. */
        if (scoped.monitors[i].sourceFilter != g_monitors.monitors[i].sourceFilter ||
            scoped.monitors[i].expectedInput != g_monitors.monitors[i].expectedInput) {
            ruleChanged = TRUE;
            rulesChanged |= 1u << i;
        }
        if (Monitor_CanControl(&scoped.monitors[i]) && Monitor_CanControl(&g_monitors.monitors[i]))
            retained |= 1u << i;
    }
    if (!scopeChanged && !ruleChanged) return;
    IdleBlack_Clear();

    if (g_hwndPopup) DestroyWindow(g_hwndPopup);
    g_hwndPopup = NULL;
    MonitorTarget pending[MAX_MONITORS];
    DWORD pendingMask = MonitorWorker_PendingTargets(pending);
    DWORD idlePendingMask = 0;
    for (int i = 0; i < g_monitors.count; i++)
        if (g_monitors.monitors[i].idleDimPending) idlePendingMask |= 1u << i;
    MonitorWorker_Reset();
    for (int i = 0; i < g_monitors.count; i++) {
        g_monitors.monitors[i].idleDimPending = FALSE;
        g_monitors.monitors[i].idleReleasePending = FALSE;
    }
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    LogMonitorList("selection updated");
    if (!scopeChanged) {
        /* A changed association is checked afresh. It must neither rebase the
           master from another PC's brightness nor cancel the current policy. */
        for (int i = 0; i < g_monitors.count; i++) {
            if ((rulesChanged & (1u << i)) && g_monitors.monitors[i].sourceFilter) {
                g_monitors.monitors[i].sourceKnown = FALSE;
                g_monitors.monitors[i].sourceCheckedTick = 0;
            }
        }
        g_hwndPopup = UI_CreatePopup(g_hInst, &g_monitors);
        for (int i = 0; i < g_monitors.count; i++) {
            BrightMonitor *monitor = &g_monitors.monitors[i];
            if (!Monitor_CanControl(monitor)) continue;
            if (g_idleDimmed && (idlePendingMask & (1u << i))) {
                DimIdleMonitor(monitor); /* Recompute canceled idle work, never replay its old percent. */
            } else if (pendingMask & (1u << i)) {
                if (g_idleDimmed) DimIdleMonitor(monitor);
                else Monitor_SetBrightness(monitor, pending[i].percent);
            }
            else if ((rulesChanged & (1u << i)) && !monitor->sourceFilter)
                ResumeSourceMonitor(monitor);
        }
        MonitorWorker_RefreshSources(&g_monitors, UI_MonitorSelectionIsOpen());
        UpdateIdleBlack(g_settings.idleDimEnabled, g_idleDimmed);
        return;
    }
    /* Saving a new scope is user activity. Restore an idle level only on
       displays retained from the old scope; newly selected displays keep their
       own current brightness instead of inheriting another display's target. */
    MonitorList intended = g_monitors;
    for (int i = 0; i < intended.count; i++) {
        BrightMonitor *monitor = &intended.monitors[i];
        if ((retained & (1u << i)) && monitor->sourceFilter && monitor->desiredBrightnessValid)
            monitor->brightnessCur = Brightness_ToRaw(monitor, monitor->desiredBrightness);
    }
    if (g_idleDimmed) {
        for (int i = 0; i < g_monitors.count; i++) {
            if (!(retained & (1u << i))) continue;
            BrightMonitor *monitor = &g_monitors.monitors[i];
            BOOL owned = monitor->idleApplied && monitor->preIdleBrightnessValid;
            DWORD original = monitor->preIdleBrightness;
            RestoreIdleMonitor(monitor);
            if (monitor->desiredBrightnessValid)
                intended.monitors[i].brightnessCur = Brightness_ToRaw(monitor, monitor->desiredBrightness);
            else if (owned)
                intended.monitors[i].brightnessCur = original;
        }
    }
    g_idleDimmed = FALSE;
    for (int i = 0; i < g_monitors.count; i++) {
        if (!(retained & (1u << i)))
            g_monitors.monitors[i].desiredBrightnessValid = FALSE;
    }
    g_masterTarget = Brightness_MasterTarget(&intended);
    g_masterTargetValid = Monitor_HasSelected(&g_monitors);
    g_masterTargetExplicit = FALSE;
    UI_SetMasterTarget(g_masterTarget);
    g_hwndPopup = UI_CreatePopup(g_hInst, &g_monitors);
    Monitor_RefreshBrightness(&g_monitors);
    MonitorWorker_RefreshSources(&g_monitors, UI_MonitorSelectionIsOpen());
    UpdateIdleBlack(g_settings.idleDimEnabled, g_idleDimmed);
}

static void HandleCommand(HWND hwnd, int cmd)
{
    Diagnostics_Log("INFO", "command", "id=%d", cmd);
    if (cmd >= IDM_PRESET_BASE && cmd < IDM_PRESET_BASE + MAX_PRESETS) {
        ApplyPreset(cmd - IDM_PRESET_BASE);
    } else switch (cmd) {
    case IDM_RESCAN:
        ScheduleRescanFromTrigger(hwnd);
        break;
    case IDM_AUTOSTART: {
        BOOL current = Settings_GetAutostart();
        g_settings.autostart = Settings_SetAutostart(!current) ? !current : current;
        Settings_Save(&g_settings);
        break;
    }
    case IDM_SCHEDULE_TOGGLE:
        g_settings.scheduleEnabled = !g_settings.scheduleEnabled;
        Settings_Save(&g_settings);
        RestartSchedule();
        break;
    case IDM_IDLEDIM_TOGGLE:
        g_settings.idleDimEnabled = !g_settings.idleDimEnabled;
        Settings_Save(&g_settings);
        if (!g_settings.idleDimEnabled)
            Idle_Restore();   /* undo an active dim immediately */
        break;
    case IDM_SETTINGS:
        UI_ShowSettings(hwnd, &g_settings, &g_monitors);
        break;
    case IDM_SETTINGS_SAVED: {
        /* The window already wrote the edited values into g_settings.
           Persist them, then apply the ones with a runtime effect. */
        if (Settings_GetAutostart() != g_settings.autostart &&
            !Settings_SetAutostart(g_settings.autostart))
            g_settings.autostart = Settings_GetAutostart();
        Settings_Save(&g_settings);
        RestartSourcePolling();
        UpdateMonitorSelection();
        int oldLo[MAX_MONITORS];
        for (int i = 0; i < g_monitors.count; i++)
            oldLo[i] = g_monitors.monitors[i].rangeLo;
        int level = g_masterTargetValid ? g_masterTarget : MasterTargetFromMonitors();
        Settings_ApplyRanges(&g_settings, &g_monitors);
        BOOL minChanged = FALSE;
        for (int i = 0; i < g_monitors.count; i++)
            if (g_monitors.monitors[i].rangeLo != oldLo[i])
                minChanged = TRUE;
        if (minChanged && !g_idleDimmed) {
            g_masterTarget = level;
            g_masterTargetValid = TRUE;
            g_masterTargetExplicit = TRUE;
            UI_SetMasterTarget(g_masterTarget);
            Monitor_SetAllBrightness(&g_monitors, g_masterTarget);
            UI_RefreshPopup(g_hwndPopup, &g_monitors);
        }
        LogSettings("saved and applied");
        if (!g_settings.idleDimEnabled)
            Idle_Restore();           /* undo an active dim right away */
        RestartSchedule();
        break;
    }
    case IDM_SCHEDULE_EDIT:
        UI_ShowScheduleEditor(hwnd, &g_settings);
        break;
    case IDM_SCHEDULE_SAVED:
        Settings_Save(&g_settings);
        RestartSchedule();
        break;
    case IDM_ABOUT:
        UI_ShowAbout(hwnd);
        break;
    case IDM_EXIT:
        PostQuitMessage(0);
        break;
    }
}

static void HandleTimer(HWND hwnd, WPARAM wParam)
{
    if (wParam == RESCAN_TIMER_ID) {
        KillTimer(hwnd, RESCAN_TIMER_ID);
        StartRescan(hwnd);
    } else if (wParam == SCHEDULE_TIMER_ID) {
        Schedule_ApplyNow();
    } else if (wParam == IDLE_TIMER_ID) {
        Idle_Tick();
    } else if (wParam == SOURCE_TIMER_ID) {
        BOOL needed = UI_MonitorSelectionIsOpen();
        for (int i = 0; !needed && i < g_monitors.count; i++) {
            const BrightMonitor *monitor = &g_monitors.monitors[i];
            needed = monitor->backend == BACKEND_DDC && monitor->hasHandle &&
                     !monitor->excludedFromControl &&
                     (monitor->sourceFilter || !monitor->controllable);
        }
        Diagnostics_Log("INFO", "source-timer", "tick needed=%d pickerOpen=%d intervalSeconds=%d",
                        needed, UI_MonitorSelectionIsOpen(), g_settings.sourcePollSeconds);
        if (needed) MonitorWorker_RefreshSources(&g_monitors, UI_MonitorSelectionIsOpen());
    } else if (wParam == RESCAN_WATCHDOG_TIMER_ID) {
        KillTimer(hwnd, RESCAN_WATCHDOG_TIMER_ID);
        if (g_rescan.busy) {
            /* The worker is stuck inside a display driver call. It cannot be
               killed safely, so it is left parked and its result will be
               discarded; what matters is releasing the single-flight guard
               so the app can rescan again. */
            DbgLog("rescan: worker %lu written off after %lu ms",
                   g_rescan.awaitedGeneration, GetTickCount() - g_rescan.startTick);
            g_rescan.awaitedGeneration = 0;
            g_rescan.busy = 0;
            g_rescan.pending = FALSE;
            if (++g_rescan.writeOffs <= RESCAN_MAX_WRITEOFFS)
                ScheduleRescan(hwnd);   /* try once more with a fresh worker */
            else
                DbgLog("rescan: %d workers hung in a row, waiting for a new trigger",
                       g_rescan.writeOffs);
        }
    } else if (wParam == RESCAN_RETRY_TIMER_ID) {
        KillTimer(hwnd, RESCAN_RETRY_TIMER_ID);
        if (g_hwndPopup && UI_IsPopupVisible(g_hwndPopup))
            SetTimer(hwnd, RESCAN_RETRY_TIMER_ID, RESCAN_AWAIT_DEFER_MS, NULL);
        else
            StartRescan(hwnd);
    }
}

/* Re-enter on the latest policy/intent, never replay old queued commands.
   Only the monitor which came back is written when the policy is unchanged. */
static void ResumeSourceMonitor(BrightMonitor *monitor)
{
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) {
        g_rescan.reapplyBrightness = TRUE;
        return;
    }
    Diagnostics_MonitorState(monitor, "source-resume requested");
    if (!Monitor_CanControl(monitor) || !Monitor_SourceAllowsControl(monitor)) {
        Diagnostics_Monitor(monitor, "INFO", "source-resume", "SKIP sourcePolicy=%s", Diagnostics_SourceReason(monitor));
        return;
    }
    int base;
    if (g_idleDimmed) {
        Diagnostics_Monitor(monitor, "INFO", "source-resume", "policy=idle mode=%s",
                            monitor->idleBlack ? "true-black" : "brightness");
        DimIdleMonitor(monitor);
        return;
    } else {
        int previousSchedule = g_scheduleLastApplied;
        Schedule_ApplyNow();
        if (g_settings.scheduleEnabled && g_settings.scheduleCount > 0 && !g_scheduleSuspended) {
            /* A changed schedule already queued the current target for all. */
            if (previousSchedule != g_scheduleLastApplied) return;
            base = g_scheduleLastApplied;
        } else if (monitor->desiredBrightnessValid) {
            Monitor_SetBrightness(monitor, monitor->desiredBrightness);
            return;
        } else if (monitor->idleApplied && monitor->preIdleBrightnessValid) {
            RestoreIdleMonitor(monitor);
            return;
        } else if (g_masterTargetValid && g_masterTargetExplicit) {
            base = g_masterTarget;
        } else {
            return; /* Manual launch without a requested brightness. */
        }
    }
    int percent = BrightMap_Level(base, monitor->rangeLo, monitor->rangeHi);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    Monitor_SetBrightness(monitor, (DWORD)percent);
}

/* A native write can finish after a topology reset. Reconcile only its idle
   ownership, using stable identity and the same idle cycle/association. The
   old index, brightness payload and source telemetry remain invalid. */
static int MonitorResultStateIndex(const MonitorResult *result)
{
    if (!result) return -1;
    if (MonitorWorker_AcceptState(result)) return result->index;
    if (!result->brightnessWritten || !result->idleEpoch ||
        (result->purpose != MONITOR_WRITE_IDLE && result->purpose != MONITOR_WRITE_IDLE_RELEASE &&
         result->purpose != MONITOR_WRITE_IDLE_RESTORE) ||
        result->backend != BACKEND_DDC || !result->deviceInstance[0]) return -1;
    BrightMonitor identity = {0};
    identity.backend = result->backend;
    CopyMemory(identity.deviceInstance, result->deviceInstance, sizeof(identity.deviceInstance));
    identity.deviceInstance[255] = L'\0';
    int index = Monitor_FindUniqueDisplay(&g_monitors, &identity);
    if (index < 0) return -1;
    const BrightMonitor *monitor = &g_monitors.monitors[index];
    if (result->idleEpoch != monitor->idleEpoch ||
        result->sourceFilter != monitor->sourceFilter ||
        result->expectedInput != monitor->expectedInput) return -1;
    return index;
}

static void HandleMonitorResult(HWND hwnd, MonitorResult *result)
{
    BOOL current = result && MonitorWorker_Accept(result);
    int stateIndex = MonitorResultStateIndex(result);
    if (result) {
        BrightMonitor identity = {0};
        identity.backend = result->backend;
        CopyMemory(identity.deviceInstance, result->deviceInstance, sizeof(identity.deviceInstance));
        const BrightMonitor *logged = current ? &g_monitors.monitors[result->index] :
                                      stateIndex >= 0 ? &g_monitors.monitors[stateIndex] : &identity;
        Diagnostics_Monitor(logged, result->success ? "INFO" : "WARN", "worker-result",
            "row=%d accepted=%d stateIndex=%d kind=%d success=%d purpose=%s generation=%lu sequence=%lu "
            "brightnessWritten=%d brightnessUpdated=%d sourceUpdated=%d sourceKnown=%d input=0x%02lX epoch=%lu",
            result->index + 1, current, stateIndex, result->kind, result->success,
            Diagnostics_WritePurpose(result->purpose), result->generation, result->sequence,
            result->brightnessWritten, result->brightnessUpdated, result->sourceUpdated,
            result->sourceKnown, result->currentInput, result->idleEpoch);
    }
    if (stateIndex >= 0) {
        BrightMonitor *monitor = &g_monitors.monitors[stateIndex];
        if (result->idleEpoch == monitor->idleEpoch && result->brightnessWritten) {
            if (result->purpose == MONITOR_WRITE_IDLE) {
                if (!monitor->preIdleBrightnessValid && result->preIdleBrightnessValid) {
                    monitor->preIdleBrightness = result->preIdleBrightness;
                    monitor->preIdleBrightnessValid = TRUE;
                }
                monitor->idleApplied = TRUE;
            } else {
                monitor->idleApplied = FALSE;
                if (!g_idleDimmed) monitor->preIdleBrightnessValid = FALSE;
            }
        }
    }
    if (current) {
        BrightMonitor *monitor = &g_monitors.monitors[result->index];
        BOOL wasAllowed = Monitor_SourceAllowsControl(monitor);
        BOOL wasControllable = monitor->controllable;
        if (result->kind != MONITOR_RESULT_SOURCE) {
            if (result->purpose == MONITOR_WRITE_IDLE) monitor->idleDimPending = FALSE;
            if (result->purpose == MONITOR_WRITE_IDLE_RELEASE || result->purpose == MONITOR_WRITE_IDLE_RESTORE) {
                monitor->idleReleasePending = FALSE;
                monitor->idleDimPending = FALSE;
            }
            if (result->purpose == MONITOR_WRITE_NORMAL &&
                (result->brightnessWritten || result->kind == MONITOR_RESULT_SKIPPED ||
                 result->kind == MONITOR_RESULT_CANCELLED || !result->success)) {
                monitor->idleDimPending = FALSE;
                monitor->idleReleasePending = FALSE;
            }
        }
        if (result->kind == MONITOR_RESULT_CANCELLED) {
            free(result);
            return;
        }
        if (result->sourceUpdated) {
            monitor->currentInput = result->currentInput;
            monitor->sourceKnown = result->sourceKnown;
            monitor->sourceCheckedTick = result->sourceCheckedTick;
        }
        if (result->brightnessUpdated) {
            monitor->brightnessMin = result->minimum;
            monitor->brightnessCur = result->current;
            monitor->brightnessMax = result->maximum;
            monitor->controllable = TRUE;
            monitor->awaitingAnswer = FALSE;
        }
        if (!result->brightnessWritten) {
            if ((!wasControllable && monitor->controllable) ||
                (result->sourceUpdated && monitor->sourceFilter && !wasAllowed &&
                 Monitor_SourceAllowsControl(monitor)))
                ResumeSourceMonitor(monitor);
        }
        if (result->kind == MONITOR_RESULT_SOURCE) {
            /* Source polling can also carry a validated capability recovery. */
        } else if (result->kind == MONITOR_RESULT_SKIPPED) {
            /* An input mismatch or unavailable source is not a driver failure. */
        } else if (result->success) {
            if (!result->brightnessUpdated) {
                monitor->brightnessMin = result->minimum;
                monitor->brightnessCur = result->current;
                monitor->brightnessMax = result->maximum;
            }
        } else if (result->purpose != MONITOR_WRITE_IDLE_RELEASE && result->purpose != MONITOR_WRITE_IDLE_RESTORE) {
            /* Failed results are writes. A display can answer reads at login
               before accepting writes; retry the current intent after recovery. */
            g_rescan.reapplyBrightness = TRUE;
            Monitor_RefreshBrightness(&g_monitors);
            ScheduleRescanThrottled(hwnd);
        }
        /* Failed raw restores wait for a later tick/poll; never spin or rescan. */
        if (result->kind == MONITOR_RESULT_SOURCE || result->purpose != MONITOR_WRITE_IDLE_RELEASE)
            TryIdleHandoff(monitor);
        UI_RefreshPopup(g_hwndPopup, &g_monitors);
        UI_MonitorSelectionRefresh(&g_monitors);
        Diagnostics_MonitorState(monitor, "result applied");
        UpdateIdleBlack(g_settings.idleDimEnabled, g_idleDimmed);
    } else if (stateIndex >= 0 && result->brightnessWritten && result->purpose == MONITOR_WRITE_IDLE) {
        /* Its brightness result was superseded, but an actual dim still needs
           undoing if the newer input telemetry belongs to another computer. */
        TryIdleHandoff(&g_monitors.monitors[stateIndex]);
    }
    free(result);
}

/* Snapshot intent while the old topology is still alive. Current automatic
   policy wins after wake; ambiguous identities never carry a pending write. */
static DWORD CapturePendingTargets(MonitorTarget pending[MAX_MONITORS])
{
    DWORD mask = MonitorWorker_PendingTargets(pending);
    for (int i = 0; i < MAX_MONITORS; i++) {
        if ((mask & (1u << i)) &&
            Monitor_FindUniqueDisplay(&g_monitors, &pending[i].monitor) < 0)
            mask &= ~(1u << i);
    }
    if ((g_rescan.reapplyBrightness || g_idleDimmed) &&
        (g_idleDimmed || (g_settings.scheduleEnabled &&
         g_settings.scheduleCount > 0 && !g_scheduleSuspended)))
        return 0;
    return mask;
}

static void ApplyPendingTargets(const MonitorTarget pending[MAX_MONITORS], DWORD mask)
{
    if (g_sessionDisplayKnown && g_sessionDisplayState == 0) {
        if (mask) g_rescan.reapplyBrightness = TRUE;
        return;
    }
    for (int i = 0; i < MAX_MONITORS; i++) {
        if (!(mask & (1u << i))) continue;
        int match = Monitor_FindUniqueDisplay(&g_monitors, &pending[i].monitor);
        if (match >= 0 && Monitor_CanControl(&g_monitors.monitors[match]))
            Monitor_SetBrightness(&g_monitors.monitors[match], pending[i].percent);
    }
}

/* Takes ownership of fresh and rebuilds the popup on the main thread. */
static int AdoptMonitorList(MonitorList *fresh)
{
    IdleBlack_Clear();
    unsigned recovered = 0;
    int waiting = Monitor_TrackUnanswered(fresh, &g_monitors, &recovered);
    /* Finish any active drag against the old topology before replacing it.
       In-flight hardware calls retain their own handle leases. */
    if (g_hwndPopup) DestroyWindow(g_hwndPopup);
    g_hwndPopup = NULL;
    MonitorTarget pending[MAX_MONITORS];
    DWORD pendingMask = CapturePendingTargets(pending);
    for (int i = 0; i < fresh->count; i++) {
        int previous = Monitor_FindUniqueDisplay(&g_monitors, &fresh->monitors[i]);
        if (previous >= 0 && Monitor_FindUniqueDisplay(fresh, &g_monitors.monitors[previous]) == i) {
            fresh->monitors[i].desiredBrightnessValid = g_monitors.monitors[previous].desiredBrightnessValid;
            fresh->monitors[i].desiredBrightness = g_monitors.monitors[previous].desiredBrightness;
            fresh->monitors[i].preIdleBrightnessValid = g_monitors.monitors[previous].preIdleBrightnessValid;
            fresh->monitors[i].preIdleBrightness = g_monitors.monitors[previous].preIdleBrightness;
            fresh->monitors[i].idleApplied = g_monitors.monitors[previous].idleApplied;
            fresh->monitors[i].idleEpoch = g_monitors.monitors[previous].idleEpoch;
        }
    }
    MonitorWorker_Reset();
    TIMED("rescan done: cleanup", Monitor_Cleanup(&g_monitors));
    g_monitors = *fresh; /* Transfer ownership of the fresh list's leases. */
    free(fresh);
    Settings_ApplyRanges(&g_settings, &g_monitors);
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    LogMonitorList("rescan adopted");
    /* Enumeration can already see the PC input on return. Revalidate here so
       source polling also resumes devices which disappeared from the topology. */
    for (int i = 0; i < g_monitors.count; i++) {
        if (g_monitors.monitors[i].sourceFilter) {
            g_monitors.monitors[i].sourceKnown = FALSE;
            g_monitors.monitors[i].sourceCheckedTick = 0;
        }
    }
    g_hwndPopup = UI_CreatePopup(g_hInst, &g_monitors);
    UI_MonitorSelectionRefresh(&g_monitors);

    /* Keep a pending restore through placeholder results, even after retries
       expire. A monitor connected while idle must inherit the idle level too. */
    BOOL reapplied = FALSE;
    if ((g_rescan.reapplyBrightness || g_idleDimmed) &&
        Monitor_HasSelected(&g_monitors)) {
        g_rescan.reapplyBrightness = FALSE;
        TIMED("rescan done: ReapplyBrightness", ReapplyBrightness());
        reapplied = TRUE;
    }
    if (!reapplied && recovered) ApplyMasterTo(recovered);
    ApplyPendingTargets(pending, pendingMask);
    /* Enumeration may have read before a write completed. */
    Monitor_RefreshBrightness(&g_monitors);
    MonitorWorker_RefreshSources(&g_monitors, UI_MonitorSelectionIsOpen());
    UpdateIdleBlack(g_settings.idleDimEnabled, g_idleDimmed);
    return waiting;
}

static void HandleRescanResult(HWND hwnd, DWORD gen, MonitorList *fresh)
{
    if (gen != g_rescan.awaitedGeneration) {
        /* A worker the watchdog wrote off has finally returned. Its handles
           describe a display topology we have already replaced. */
        DbgLog("rescan: discarding late result from worker %lu", gen);
        TIMED("rescan late: cleanup", FreeMonitorList(fresh));
        return;   /* the busy flag belongs to the current worker now */
    }

    KillTimer(hwnd, RESCAN_WATCHDOG_TIMER_ID);
    g_rescan.awaitedGeneration = 0;
    g_rescan.writeOffs = 0;   /* this worker came back, the driver is answering */
    DbgLog("rescan: worker %lu finished after %lu ms",
           gen, GetTickCount() - g_rescan.startTick);

    /* Reject a placeholder enumeration instead of adopting it: for a few
       seconds after a display returns, Windows reports no monitor at all or
       a generic panel, and adopting that silently kills brightness control
       until the next display event. Retry on a backoff and keep the list we
       have, which is either still valid or about to be replaced anyway. */
    if (fresh) Settings_ApplyMonitorSelection(&g_settings, fresh);
    if (fresh && !Monitor_HasControllable(fresh) &&
        (Monitor_HasSelected(&g_monitors) || g_rescan.reapplyBrightness) &&
        g_rescan.retry < RESCAN_MAX_RETRIES) {
        DWORD delay = kRescanBackoffMs[g_rescan.retry++];
        DbgLog("rescan: nothing controllable, retry %d in %lu ms",
               g_rescan.retry, delay);
        TIMED("rescan rejected: cleanup", FreeMonitorList(fresh));
        SetTimer(hwnd, RESCAN_RETRY_TIMER_ID, delay, NULL);
        g_rescan.busy = 0;
        return;
    }
    g_rescan.retry = 0;

    if (fresh) {
        int waiting = AdoptMonitorList(fresh);
        if (waiting > 0) {
            DWORD delay = g_awaitRetry < RESCAN_MAX_RETRIES
                          ? kRescanBackoffMs[g_awaitRetry++] : RESCAN_AWAIT_STEADY_MS;
            SetTimer(hwnd, RESCAN_RETRY_TIMER_ID, delay, NULL);
        } else {
            g_awaitRetry = 0;
        }
    }
    g_rescan.busy = 0;
    if (g_rescan.pending) {   /* triggers arrived mid-run: coalesce one more */
        g_rescan.pending = FALSE;
        ScheduleRescan(hwnd);
    }
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

/* Power notifications carry DWORD payloads, which need not be aligned. Initial
   presence is just a snapshot; only a later return to present is activity. */
static void HandlePowerSettingChange(HWND hwnd, const POWERBROADCAST_SETTING *setting)
{
    if (!setting) return;
    BOOL display = IsEqualGUID(&setting->PowerSetting, &kGuidSessionDisplayStatus);
    BOOL presence = IsEqualGUID(&setting->PowerSetting, &kGuidSessionUserPresence);
    if ((!display && !presence) || setting->DataLength < sizeof(DWORD)) return;
    DWORD value;
    CopyMemory(&value, setting->Data, sizeof(value));
    if (value > 2) return;
    if (display) {
        BOOL returning = !g_sessionDisplayKnown || g_sessionDisplayState == 0;
        g_sessionDisplayKnown = TRUE;
        g_sessionDisplayState = value;
        Diagnostics_Log("INFO", "power", "session display state=%lu", value);
        if (value == 0) {
            for (int i = 0; i < g_monitors.count; i++) {
                BrightMonitor *monitor = &g_monitors.monitors[i];
                /* Cancel queued PC idle work; a native call already in progress
                   can still acknowledge its baseline later. An alternate-input
                   handoff may undo our dim on another PC and remains allowed. */
                if (monitor->idleDimPending ||
                    (monitor->idleReleasePending &&
                     (Monitor_SourceAllowsControl(monitor) || !monitor->sourceKnown))) {
                    MonitorWorker_Cancel(monitor);
                    monitor->idleDimPending = monitor->idleReleasePending = FALSE;
                    g_rescan.reapplyBrightness = TRUE;
                }
            }
            UpdateIdleBlack(FALSE, FALSE);
        } else {
            /* A normal on->dim notification does not require new DDC handles
               and must not cause us to undo Windows's own dim transition. */
            if (returning) {
                g_rescan.reapplyBrightness = TRUE;
                ScheduleRescanFromTrigger(hwnd);
            }
            if (value == 1) Idle_Tick();
        }
    } else {
        BOOL returned = g_sessionPresenceKnown && g_sessionPresenceState != 0 && value == 0;
        g_sessionPresenceKnown = TRUE;
        g_sessionPresenceState = value;
        Diagnostics_Log("INFO", "power", "session presence state=%lu returned=%d", value, returned);
        if (returned) Idle_Activity();
    }
}

/* ---- Main Window Proc ---- */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* A newer instance is launching and wants our spot: exit cleanly so it can
       take over. Registered message, so it cannot be a compile-time switch case. */
    if (msg == g_wmTakeover && g_wmTakeover != 0) {
        Diagnostics_Log("INFO", "app", "replacement requested by another Lumos instance");
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

    case WM_COMMAND:
        HandleCommand(hwnd, LOWORD(wParam));
        return 0;

    case WM_DISPLAYCHANGE:
        IdleBlack_Clear();
        /* Monitor plugged/unplugged (resolution/topology change) */
        DbgLog("display change: %ux%u bpp=%u",
               (unsigned)LOWORD(lParam), (unsigned)HIWORD(lParam), (unsigned)wParam);
        ScheduleRescanThrottled(hwnd);
        return 0;

    case WM_WTSSESSION_CHANGE:
        /* Session unlocked or reconnected: DDC handles may be stale */
        DbgLog("session change: %u", (unsigned)wParam);
        if (wParam == WTS_SESSION_LOCK || wParam == WTS_CONSOLE_DISCONNECT) {
            IdleBlack_SetSessionLocked(TRUE);
        } else if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT) {
            Idle_Activity();
            IdleBlack_SetSessionLocked(FALSE);
            Idle_Tick(); /* Re-evaluate the input which unlocked the session before covering it. */
            g_rescan.reapplyBrightness = TRUE;   /* restore brightness after the recovery rescan */
            ScheduleRescanFromTrigger(hwnd);
        }
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_POWERSETTINGCHANGE) {
            HandlePowerSettingChange(hwnd, (const POWERBROADCAST_SETTING *)lParam);
        } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            DbgLog("power: APM resume");
            g_rescan.reapplyBrightness = TRUE;   /* wake from sleep: displays often reset brightness */
            ScheduleRescanFromTrigger(hwnd);
        }
        return TRUE;

    case WM_TIMER:
        HandleTimer(hwnd, wParam);
        return 0;

    case WM_MONITOR_RESULT:
        HandleMonitorResult(hwnd, (MonitorResult *)lParam);
        return 0;

    case WM_IDLE_BLACK_WAKE:
        Idle_Activity();
        return 0;

    case WM_APP_RESCAN_DONE:
        HandleRescanResult(hwnd, (DWORD)wParam, (MonitorList *)lParam);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
