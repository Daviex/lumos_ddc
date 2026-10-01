#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <shlobj.h>
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
static BOOL         g_scheduleSuspended = FALSE;
static int          g_scheduleSuspendMinute = 0;   /* minute-of-day at suspend */
static int          g_scheduleResumeMinute = 0;    /* next anchor to resume at */
static ULONGLONG    g_scheduleResumeLocalMinute;   /* dated local-time deadline */
static int          g_scheduleLastApplied = -1;    /* last brightness pushed by the schedule */
static int          g_masterTarget;          /* intended base percent, including negative delta compensation */
static BOOL         g_masterTargetValid;
static BOOL         g_idleDimmed = FALSE;     /* TRUE while the idle level is on the monitors */

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

static const WCHAR APPCLASS[] = L"LumosMain";

/* Forward declarations */
static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
static void CreateTrayIcon(HWND hwnd);
static void RemoveTrayIcon(void);
static void RegisterHotkeys(HWND hwnd);
static void UnregisterHotkeys(HWND hwnd);
static void ShowContextMenu(HWND hwnd);
static void HandleHotkey(int id);
static void ApplyPreset(int index);
static void ApplyPresetBrightness(DWORD brightness);
static void ApplyStartupBrightness(BOOL loginLaunch);
static void InstallMouseHook(void);
static void RemoveMouseHook(void);
static void ScheduleRescan(HWND hwnd);
static void ScheduleRescanFromTrigger(HWND hwnd);
static void StartRescan(HWND hwnd);
static DWORD WINAPI RescanThreadProc(LPVOID param);

/* Heap-passed to the rescan worker: which window to post back to, and which
   generation the result belongs to. */
typedef struct { HWND hwnd; DWORD gen; } RescanArgs;
static void Schedule_ApplyNow(void);
static void Schedule_Suspend(void);
static void ManualChange(void);
static void SliderManualChange(int row, int target);
static void Idle_Tick(void);
static void Idle_Restore(void);

static void SaveDeltasCallback(void)
{
    Settings_SaveDeltas(&g_settings, &g_monitors);
    Settings_Save(&g_settings);
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
    Settings_LoadDeltas(&g_settings, &g_monitors);
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);

    if (!UI_Init(hInst)) {
        MessageBoxW(NULL, L"Failed to initialize UI", APP_NAME, MB_ICONERROR);
        Monitor_Cleanup(&g_monitors);
        Monitor_FlushRetiredHandles();
        CoUninitialize();
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
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
        MessageBoxW(NULL, L"Failed to start monitor worker", APP_NAME, MB_ICONERROR);
        Monitor_Cleanup(&g_monitors);
        Monitor_FlushRetiredHandles();
        UI_Shutdown();
        if (g_hwndHidden) DestroyWindow(g_hwndHidden);
        CoUninitialize();
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    /* Re-enumerate on session unlock and on display power-on. Windows does not
       reliably send WM_DISPLAYCHANGE across a lock screen, so cached DDC handles
       go stale; these notifications trigger a rescan to re-acquire them. */
    WTSRegisterSessionNotification(g_hwndHidden, NOTIFY_FOR_THIS_SESSION);
    g_hPowerNotify = RegisterPowerSettingNotification(
        g_hwndHidden, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);

    /* Create popup (hidden) */
    g_hwndPopup = UI_CreatePopup(hInst, &g_monitors);
    UI_SetDeltaSaveCallback(SaveDeltasCallback);

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

    /* Message loop */
    MSG msg = { 0 };
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (UI_HandleDialogMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    int exitCode = (int)msg.wParam;

    /* Cleanup */
    RemoveMouseHook();
    UnregisterHotkeys(g_hwndHidden);
    if (g_hPowerNotify) UnregisterPowerSettingNotification(g_hPowerNotify);
    WTSUnRegisterSessionNotification(g_hwndHidden);
    RemoveTrayIcon();
    if (g_hwndPopup) DestroyWindow(g_hwndPopup);
    Monitor_Cleanup(&g_monitors);
    MonitorWorker_Stop();
    while (PeekMessageW(&msg, g_hwndHidden, WM_MONITOR_RESULT, WM_MONITOR_RESULT, PM_REMOVE))
        free((void *)msg.lParam);
    UI_Shutdown();
    CoUninitialize();
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
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_LUMOS));
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, APP_NAME L" - Monitor Brightness");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
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
            if (FAILED(StringCchCatW(path, MAX_PATH, L"\\Lumos"))) return;
            CreateDirectoryW(path, NULL);
            if (FAILED(StringCchCatW(path, MAX_PATH, L"\\lumos-app.log"))) return;
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
            short delta = (short)HIWORD(mhs->mouseData);
            PostMessageW(g_hwndHidden, WM_HOTKEY,
                         (WPARAM)(delta > 0 ? WM_HOTKEY_BRIGHTEN : WM_HOTKEY_DIM), 0);
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

static void RegisterHotkeys(HWND hwnd)
{
    RegisterHotKey(hwnd, WM_HOTKEY_BRIGHTEN, MOD_CONTROL | MOD_ALT, VK_UP);
    RegisterHotKey(hwnd, WM_HOTKEY_DIM,      MOD_CONTROL | MOD_ALT, VK_DOWN);
}

static void UnregisterHotkeys(HWND hwnd)
{
    UnregisterHotKey(hwnd, WM_HOTKEY_BRIGHTEN);
    UnregisterHotKey(hwnd, WM_HOTKEY_DIM);
}

/* ---- Context Menu ---- */

static void ShowContextMenu(HWND hwnd)
{
    UI_ShowContextMenu(hwnd, &g_settings);
}

/* ---- Hotkey Handler ---- */

static void HandleHotkey(int id)
{
    if (!Monitor_HasSelected(&g_monitors)) return;
    int step = g_settings.step;
    int delta = 0;

    switch (id) {
    case WM_HOTKEY_BRIGHTEN: delta = step;  break;
    case WM_HOTKEY_DIM:      delta = -step; break;
    default: return;
    }

    /* Initialize target from current state if needed */
    if (!g_masterTargetValid) {
        g_masterTarget = Brightness_MasterTarget(&g_monitors);
        g_masterTargetValid = TRUE;
    }

    g_masterTarget += delta;

    int lo, hi;
    Brightness_TargetRange(&g_monitors, &lo, &hi);
    if (g_masterTarget < lo) g_masterTarget = lo;
    if (g_masterTarget > hi) g_masterTarget = hi;
    UI_SetMasterTarget(g_masterTarget);

    TIMED("hotkey: SetAllBrightness",
          Monitor_SetAllBrightness(&g_monitors, g_masterTarget));

    /* Show feedback on an affected display, preferring the cursor's monitor. */
    TIMED("hotkey: RefreshBrightness", Monitor_RefreshBrightness(&g_monitors));
    {
        POINT curPos;
        GetCursorPos(&curPos);
        HMONITOR hCurMon = MonitorFromPoint(curPos, MONITOR_DEFAULTTOPRIMARY);
        int selected = -1;
        for (int i = 0; i < g_monitors.count; i++) {
            BrightMonitor *mon = &g_monitors.monitors[i];
            if (!Monitor_CanControl(mon)) continue;
            if (selected < 0) selected = i;
            if (mon->hMonitor == hCurMon) {
                selected = i;
                break;
            }
        }
        if (selected >= 0) {
            BrightMonitor *mon = &g_monitors.monitors[selected];
            UI_ShowOSD(g_hInst, mon->hMonitor, Brightness_GetPercent(mon));
        }
    }

    /* Update popup if visible */
    UI_RefreshPopup(g_hwndPopup, &g_monitors);

    ManualChange();
}

/* ---- Apply Preset ---- */

static void ApplyPreset(int index)
{
    if (index < 0 || index >= g_settings.presetCount) return;
    ApplyPresetBrightness(g_settings.presets[index].brightness);
}

static void ApplyPresetBrightness(DWORD brightness)
{
    g_masterTarget = (int)brightness;
    g_masterTargetValid = TRUE;
    UI_SetMasterTarget(g_masterTarget);
    Monitor_SetAllBrightness(&g_monitors, g_masterTarget);
    Monitor_RefreshBrightness(&g_monitors);
    UI_RefreshPopup(g_hwndPopup, &g_monitors);

    ManualChange();
}

static void ApplyStartupBrightness(BOOL loginLaunch)
{
    if (!loginLaunch) {
        Schedule_ApplyNow();
        return;
    }

    /* Treat Day as a preset selection, including per-monitor offsets and
       suspension of the schedule until its next anchor. */
    ApplyPresetBrightness((DWORD)Settings_DayBrightness(&g_settings));

    /* Displays can still be reconnecting at login. The existing bounded
       rescan/retry path re-applies the latest intent, so a later manual,
       schedule or idle change always takes precedence over this startup value. */
    g_rescan.reapplyBrightness = TRUE;
    ScheduleRescanFromTrigger(g_hwndHidden);
}

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
#ifdef DEBUG
    DWORD generation = args->gen; /* worker frees args as soon as it starts */
#endif
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
    g_masterTarget = value;
    g_masterTargetValid = TRUE;
    UI_SetMasterTarget(value);
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
        UI_SetMasterTarget(g_settings.idleDimPercent);
        TIMED("reapply(idle level): SetAllBrightness",
              Monitor_SetAllBrightness(&g_monitors, g_settings.idleDimPercent));
        return;
    }
    if (g_settings.scheduleEnabled && g_settings.scheduleCount > 0 && !g_scheduleSuspended) {
        g_scheduleLastApplied = -1;   /* force a re-push even if the value is unchanged */
        Schedule_ApplyNow();
        return;
    }
    if (g_masterTargetValid) {        /* skip if the user never set a level yet */
        UI_SetMasterTarget(g_masterTarget);
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
}

/* Every manual brightness change (hotkey, wheel, slider, preset) goes through
   here: it supersedes the idle level and suspends the schedule. */
static void ManualChange(void)
{
    g_idleDimmed = FALSE;   /* the user just set a level; do not restore over it */
    Schedule_Suspend();
}

static void SliderManualChange(int row, int target)
{
    g_masterTarget = row < 0 ? target : Brightness_MasterTarget(&g_monitors);
    g_masterTargetValid = TRUE;
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
    if (!GetLastInputInfo(&lii))
        return 0;
    return GetTickCount() - lii.dwTime;   /* unsigned math, so the 49-day wrap is fine */
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
    if (!g_masterTargetValid) {
        g_masterTarget = Brightness_MasterTarget(&g_monitors);
        g_masterTargetValid = TRUE;
    }
    g_idleDimmed = TRUE;
    UI_SetMasterTarget(g_settings.idleDimPercent);
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

/* ---- Main-thread message handlers ---- */

static void RestartSchedule(void)
{
    g_scheduleSuspended = FALSE;
    g_scheduleLastApplied = -1;
    Schedule_ApplyNow();
}

/* A scope change cancels queued writes and any drag against the previous
   selection. Rebase manual controls on the newly selected displays so the
   first hotkey does not jump to the previous group's brightness. */
static void UpdateMonitorSelection(void)
{
    MonitorList scoped = g_monitors;
    Settings_ApplyMonitorSelection(&g_settings, &scoped);
    BOOL changed = scoped.selectedOnly != g_monitors.selectedOnly;
    DWORD retained = 0;
    for (int i = 0; i < scoped.count; i++) {
        if (scoped.monitors[i].excludedFromControl != g_monitors.monitors[i].excludedFromControl)
            changed = TRUE;
        if (Monitor_CanControl(&scoped.monitors[i]) && Monitor_CanControl(&g_monitors.monitors[i]))
            retained |= 1u << i;
    }
    if (!changed) return;

    if (g_hwndPopup) DestroyWindow(g_hwndPopup);
    g_hwndPopup = NULL;
    MonitorWorker_Reset();
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    /* Saving a new scope is user activity. Restore an idle level only on
       displays retained from the old scope; newly selected displays keep their
       own current brightness instead of inheriting another display's target. */
    if (g_idleDimmed && g_masterTargetValid) {
        for (int i = 0; i < g_monitors.count; i++) {
            if (!(retained & (1u << i))) continue;
            int target = g_masterTarget + g_monitors.monitors[i].delta;
            if (target < 0) target = 0;
            if (target > 100) target = 100;
            Monitor_SetBrightness(&g_monitors.monitors[i], (DWORD)target);
        }
    }
    g_idleDimmed = FALSE;
    g_masterTarget = Brightness_MasterTarget(&g_monitors);
    g_masterTargetValid = Monitor_HasSelected(&g_monitors);
    UI_SetMasterTarget(g_masterTarget);
    g_hwndPopup = UI_CreatePopup(g_hInst, &g_monitors);
    Monitor_RefreshBrightness(&g_monitors);
}

static void HandleCommand(HWND hwnd, int cmd)
{
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
        UpdateMonitorSelection();
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
        StartRescan(hwnd);
    }
}

static void HandleMonitorResult(HWND hwnd, MonitorResult *result)
{
    if (result && MonitorWorker_Accept(result)) {
        if (result->success) {
            BrightMonitor *monitor = &g_monitors.monitors[result->index];
            monitor->brightnessMin = result->minimum;
            monitor->brightnessCur = result->current;
            monitor->brightnessMax = result->maximum;
            UI_RefreshPopup(g_hwndPopup, &g_monitors);
        } else {
            /* Failed results are writes. A display can answer reads at login
               before accepting writes; retry the current intent after recovery. */
            g_rescan.reapplyBrightness = TRUE;
            Monitor_RefreshBrightness(&g_monitors);
            ScheduleRescanThrottled(hwnd);
        }
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
    for (int i = 0; i < MAX_MONITORS; i++) {
        if (!(mask & (1u << i))) continue;
        int match = Monitor_FindUniqueDisplay(&g_monitors, &pending[i].monitor);
        if (match >= 0 && Monitor_CanControl(&g_monitors.monitors[match]))
            Monitor_SetBrightness(&g_monitors.monitors[match], pending[i].percent);
    }
}

/* Takes ownership of fresh and rebuilds the popup on the main thread. */
static void AdoptMonitorList(MonitorList *fresh)
{
    /* Finish any active drag against the old topology before replacing it.
       In-flight hardware calls retain their own handle leases. */
    if (g_hwndPopup) DestroyWindow(g_hwndPopup);
    g_hwndPopup = NULL;
    MonitorTarget pending[MAX_MONITORS];
    DWORD pendingMask = CapturePendingTargets(pending);
    MonitorWorker_Reset();
    TIMED("rescan done: cleanup", Monitor_Cleanup(&g_monitors));
    g_monitors = *fresh; /* Transfer ownership of the fresh list's leases. */
    free(fresh);
    Settings_LoadDeltas(&g_settings, &g_monitors);
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    g_hwndPopup = UI_CreatePopup(g_hInst, &g_monitors);

    /* Keep a pending restore through placeholder results, even after retries
       expire. A monitor connected while idle must inherit the idle level too. */
    if ((g_rescan.reapplyBrightness || g_idleDimmed) &&
        Monitor_HasSelected(&g_monitors)) {
        g_rescan.reapplyBrightness = FALSE;
        TIMED("rescan done: ReapplyBrightness", ReapplyBrightness());
    }
    ApplyPendingTargets(pending, pendingMask);
    /* Enumeration may have read before a write completed. */
    Monitor_RefreshBrightness(&g_monitors);
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
    if (fresh && !Monitor_HasSelected(fresh) &&
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

    if (fresh) AdoptMonitorList(fresh);
    g_rescan.busy = 0;
    if (g_rescan.pending) {   /* triggers arrived mid-run: coalesce one more */
        g_rescan.pending = FALSE;
        ScheduleRescan(hwnd);
    }
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
    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
            UI_TogglePopup(g_hwndPopup, &g_monitors);
            break;
        case WM_RBUTTONUP:
            ShowContextMenu(hwnd);
            break;
        }
        return 0;

    case WM_HOTKEY:
        HandleHotkey((int)wParam);
        return 0;

    case WM_COMMAND:
        HandleCommand(hwnd, LOWORD(wParam));
        return 0;

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
            g_rescan.reapplyBrightness = TRUE;   /* restore brightness after the recovery rescan */
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
                    g_rescan.reapplyBrightness = TRUE;
                    ScheduleRescanFromTrigger(hwnd);
                }
            }
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
