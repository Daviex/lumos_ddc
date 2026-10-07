/* Exercise the application's real startup/schedule/rescan orchestration.
   Clock, timers, windows, settings and monitor I/O are in-memory mocks. The
   application's entry point is never called and no hardware is accessed. */
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include "../monitor.h"
#include "../monitor_worker.h"
#include "../brightness.h"
#include "../ui.h"
#include "../presets.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static const HWND testWindow = (HWND)(UINT_PTR)1;
static const HWND testPopup = (HWND)(UINT_PTR)2;
static int nowMinute, nowDay, dayValue, dayCalls;
static int mockRangeLo, mockRangeHi;
static DWORD nowTick, lastInputTick;
static int setAllCalls, setOneCalls, actualWrites, lastBase, lastUiTarget;
static int refreshCalls, popupCalls, destroyCalls, workerResets, cleanupCalls;
static int timerCalls, killCalls;
static int sourceRefreshCalls, pickerRefreshCalls;
static UINT sourceTelemetryInterval;
static BOOL pickerOpen;
static DWORD mockPendingMask;
static DWORD mockPendingTarget;
static BOOL acceptResult;
static BOOL acceptStateResult;
static int acceptCalls;
static int idleWrites, releaseWrites, restoreRequests, cancelCalls;
static BOOL mockWorkerRunning;
static BOOL mockReleaseSucceeds;
static BOOL mockNormalSucceeds;
static int releaseRequests;
static int blackUpdates, blackClears;
static BOOL blackIdle, blackEnabled;
static int osdCalls, osdPercent;
static HMONITOR osdMonitor;
static BOOL popupVisible;
static UINT_PTR lastTimer;
static UINT lastInterval;
static UINT_PTR lastKilledTimer;

static void WINAPI MockGetLocalTime(LPSYSTEMTIME time);
static DWORD WINAPI MockGetTickCount(void);
static BOOL WINAPI MockGetLastInputInfo(PLASTINPUTINFO input);
static UINT_PTR WINAPI MockSetTimer(HWND, UINT_PTR, UINT, TIMERPROC);
static BOOL WINAPI MockKillTimer(HWND, UINT_PTR);
static BOOL WINAPI MockDestroyWindow(HWND);
static HRESULT WINAPI MockNotificationState(QUERY_USER_NOTIFICATION_STATE *state);
static BOOL WINAPI MockGetCursorPos(LPPOINT point);
static HMONITOR WINAPI MockMonitorFromPoint(POINT point, DWORD flags);
static void ConfigureTwoReadyMonitors(void);

#define GetLocalTime MockGetLocalTime
#define GetTickCount MockGetTickCount
#define GetLastInputInfo MockGetLastInputInfo
#define SetTimer MockSetTimer
#define KillTimer MockKillTimer
#define DestroyWindow MockDestroyWindow
#define SHQueryUserNotificationState MockNotificationState
#define GetCursorPos MockGetCursorPos
#define MonitorFromPoint MockMonitorFromPoint
#define WinMain static UnusedApplicationEntryPoint
#include "../lumos.c"
#undef GetLocalTime
#undef GetTickCount
#undef GetLastInputInfo
#undef SetTimer
#undef KillTimer
#undef DestroyWindow
#undef SHQueryUserNotificationState
#undef GetCursorPos
#undef MonitorFromPoint
#undef WinMain

static void WINAPI MockGetLocalTime(LPSYSTEMTIME time)
{
    memset(time, 0, sizeof(*time));
    time->wYear = 2026;
    time->wMonth = 10;
    time->wDay = (WORD)nowDay;
    time->wHour = (WORD)(nowMinute / 60);
    time->wMinute = (WORD)(nowMinute % 60);
}

static DWORD WINAPI MockGetTickCount(void) { return nowTick; }
static BOOL WINAPI MockGetLastInputInfo(PLASTINPUTINFO input)
{
    CHECK(input->cbSize == sizeof(*input));
    input->dwTime = lastInputTick;
    return TRUE;
}

static HRESULT WINAPI MockNotificationState(QUERY_USER_NOTIFICATION_STATE *state)
{
    *state = QUNS_ACCEPTS_NOTIFICATIONS;
    return S_OK;
}

BOOL Capture_InUse(void) { return FALSE; }

static BOOL WINAPI MockGetCursorPos(LPPOINT point)
{
    point->x = point->y = 50;
    return TRUE;
}

static HMONITOR WINAPI MockMonitorFromPoint(POINT point, DWORD flags)
{
    (void)point; (void)flags;
    return (HMONITOR)(UINT_PTR)2; /* Cursor is on the excluded screen. */
}

void UI_ShowOSD(HINSTANCE instance, HMONITOR monitor, int percent, BOOL allMonitors)
{
    (void)instance; (void)allMonitors;
    osdCalls++;
    osdMonitor = monitor;
    osdPercent = percent;
}

static UINT_PTR WINAPI MockSetTimer(HWND hwnd, UINT_PTR id, UINT interval,
                                  TIMERPROC callback)
{
    CHECK(hwnd == testWindow && callback == NULL);
    timerCalls++;
    lastTimer = id;
    lastInterval = interval;
    return id;
}

static BOOL WINAPI MockKillTimer(HWND hwnd, UINT_PTR id)
{
    CHECK(hwnd == testWindow);
    killCalls++;
    lastKilledTimer = id;
    return TRUE;
}

static BOOL WINAPI MockDestroyWindow(HWND hwnd)
{
    CHECK(hwnd == testPopup);
    destroyCalls++;
    return TRUE;
}

int Settings_DayBrightness(const Settings *settings)
{
    CHECK(settings == &g_settings);
    dayCalls++;
    return dayValue;
}

void Settings_ApplyRanges(Settings *settings, MonitorList *view)
{
    CHECK(settings == &g_settings && view == &g_monitors);
    for (int i = 0; i < view->count; i++) {
        view->monitors[i].rangeLo = mockRangeLo;
        view->monitors[i].rangeHi = mockRangeHi;
    }
}

void Settings_StoreRanges(Settings *settings, const MonitorList *view)
{ CHECK(settings == &g_settings && view == &g_monitors); }
void Settings_Save(Settings *settings) { CHECK(settings == &g_settings); }
BOOL UI_IsPopupVisible(HWND window)
{ CHECK(window == NULL || window == testPopup); return window && popupVisible; }

/* Model stable matches and anonymous stand-ins without invoking hardware.
   The backend suite exercises the real tracker in detail. */
int Monitor_TrackUnanswered(MonitorList *fresh, const MonitorList *previous, unsigned *recovered)
{
    BOOL matched[MAX_MONITORS] = {0};
    int oldIndex[MAX_MONITORS];
    for (int i = 0; i < fresh->count; i++) {
        oldIndex[i] = Monitor_FindUniqueDisplay(previous, &fresh->monitors[i]);
        if (oldIndex[i] >= 0) matched[oldIndex[i]] = TRUE;
    }
    int lostWorking = 0, lostWaiting = 0;
    for (int i = 0; i < previous->count; i++) {
        if (matched[i]) continue;
        if (previous->monitors[i].awaitingAnswer) lostWaiting++;
        else if (previous->monitors[i].controllable) lostWorking++;
    }
    int waiting = 0;
    *recovered = 0;
    for (int i = 0; i < fresh->count; i++) {
        BrightMonitor *monitor = &fresh->monitors[i];
        BOOL returned = FALSE, awaited = FALSE;
        if (oldIndex[i] >= 0) {
            const BrightMonitor *old = &previous->monitors[oldIndex[i]];
            returned = monitor->controllable && old->awaitingAnswer;
            awaited = !monitor->controllable && (old->controllable || old->awaitingAnswer);
        } else if (!monitor->deviceInstance[0]) {
            if (monitor->controllable && lostWaiting) { lostWaiting--; returned = TRUE; }
            else if (!monitor->controllable && lostWorking) { lostWorking--; awaited = TRUE; }
            else if (!monitor->controllable && lostWaiting) { lostWaiting--; awaited = TRUE; }
        }
        monitor->awaitingAnswer = awaited;
        if (awaited) waiting++;
        if (returned) *recovered |= 1u << i;
    }
    return waiting;
}

BOOL Monitor_HasControllable(const MonitorList *view)
{
    for (int i = 0; i < view->count; i++)
        if (view->monitors[i].controllable) return TRUE;
    return FALSE;
}

BOOL Monitor_SetBrightness(BrightMonitor *monitor, DWORD percent)
{
    setOneCalls++;
    CHECK(Monitor_CanControl(monitor));
    if (!Monitor_CanControl(monitor)) return FALSE;
    monitor->desiredBrightness = percent;
    monitor->desiredBrightnessValid = TRUE;
    if (!Monitor_SourceAllowsControl(monitor)) return FALSE;
    if (!mockNormalSucceeds) return FALSE;
    monitor->brightnessCur = Brightness_ToRaw(monitor, percent);
    if (!mockWorkerRunning) {
        monitor->idleApplied = FALSE;
        monitor->idleDimPending = FALSE;
        monitor->idleReleasePending = FALSE;
    }
    actualWrites++;
    return TRUE;
}

BOOL Monitor_SetIdleBrightness(BrightMonitor *monitor, DWORD percent)
{
    CHECK(Monitor_CanControl(monitor));
    if (!Monitor_CanControl(monitor) || !Monitor_SourceAllowsControl(monitor))
        return FALSE;
    if (mockWorkerRunning) return TRUE; /* Delivery is driven explicitly by the test. */
    if (!monitor->preIdleBrightnessValid) {
        monitor->preIdleBrightness = monitor->brightnessCur;
        monitor->preIdleBrightnessValid = TRUE;
    }
    monitor->brightnessCur = Brightness_ToRaw(monitor, percent);
    idleWrites++;
    actualWrites++;
    return TRUE;
}

BOOL Monitor_RestoreIdleBrightness(BrightMonitor *monitor)
{
    restoreRequests++;
    CHECK(Monitor_CanControl(monitor) && monitor->idleApplied && monitor->preIdleBrightnessValid);
    if (!Monitor_SourceAllowsControl(monitor) || !mockNormalSucceeds) return FALSE;
    if (mockWorkerRunning) return TRUE;
    monitor->brightnessCur = monitor->preIdleBrightness;
    monitor->idleApplied = FALSE;
    monitor->idleDimPending = FALSE;
    monitor->idleReleasePending = FALSE;
    monitor->preIdleBrightnessValid = FALSE;
    actualWrites++;
    return TRUE;
}

BOOL Monitor_ReleaseIdleBrightness(BrightMonitor *monitor, DWORD raw, DWORD otherInput)
{
    releaseRequests++;
    CHECK(Monitor_CanControl(monitor));
    CHECK(monitor->preIdleBrightnessValid && monitor->idleApplied);
    if (!Monitor_CanControl(monitor) || !monitor->sourceFilter ||
        !monitor->sourceKnown || !monitor->expectedInput ||
        otherInput == monitor->expectedInput || monitor->currentInput != otherInput)
        return FALSE;
    if (!mockReleaseSucceeds) return FALSE;
    if (mockWorkerRunning) return TRUE;
    monitor->brightnessCur = raw;
    releaseWrites++;
    actualWrites++;
    return TRUE;
}

BOOL Monitor_SetAllBrightness(MonitorList *view, int base)
{
    CHECK(view == &g_monitors);
    setAllCalls++;
    lastBase = base;
    BOOL allOk = TRUE;
    for (int i = 0; i < view->count; i++) {
        BrightMonitor *monitor = &view->monitors[i];
        if (monitor->excludedFromControl) continue;
        int percent = BrightMap_Level(base, monitor->rangeLo, monitor->rangeHi);
        if (!monitor->controllable) {
            if (monitor->awaitingAnswer || (monitor->backend == BACKEND_DDC && monitor->hasHandle)) {
                monitor->desiredBrightnessValid = TRUE;
                monitor->desiredBrightness = (DWORD)percent;
            }
            continue;
        }
        if (!Monitor_SetBrightness(monitor, (DWORD)percent)) allOk = FALSE;
    }
    return allOk;
}

void Monitor_RefreshBrightness(MonitorList *view)
{
    CHECK(view == &g_monitors);
    refreshCalls++;
}

void Monitor_Cleanup(MonitorList *view)
{
    cleanupCalls++;
    memset(view, 0, sizeof(*view));
}
void Monitor_Enumerate(MonitorList *view)
{ (void)view; CHECK(FALSE); } /* Tests deliver completed scans explicitly. */

void MonitorWorker_Reset(void) { workerResets++; }
void IdleBlack_Init(HINSTANCE instance, HWND owner) { (void)instance; (void)owner; }
void IdleBlack_Update(const MonitorList *view, BOOL enabled, BOOL idle)
{
    CHECK(view == &g_monitors);
    blackUpdates++;
    blackIdle = idle;
    blackEnabled = enabled;
}
void IdleBlack_Clear(void) { blackClears++; blackIdle = FALSE; }
void IdleBlack_SetSessionLocked(BOOL locked) { (void)locked; }
void IdleBlack_Shutdown(void) { CHECK(FALSE); }
BOOL MonitorWorker_Running(void) { return mockWorkerRunning; }
void MonitorWorker_Cancel(BrightMonitor *monitor)
{
    CHECK(monitor >= g_monitors.monitors && monitor < g_monitors.monitors + g_monitors.count);
    cancelCalls++;
}
void MonitorWorker_RefreshSources(const MonitorList *view, BOOL allMonitors)
{
    CHECK(view == &g_monitors);
    CHECK(allMonitors == pickerOpen);
    sourceRefreshCalls++;
}
BOOL UI_MonitorSelectionIsOpen(void) { return pickerOpen; }
void UI_MonitorSelectionRefresh(const MonitorList *view)
{
    CHECK(view == &g_monitors);
    pickerRefreshCalls++;
}
BOOL MonitorWorker_Accept(const MonitorResult *result)
{
    CHECK(result != NULL);
    acceptCalls++;
    return acceptResult;
}
BOOL MonitorWorker_AcceptState(const MonitorResult *result)
{
    CHECK(result != NULL);
    return acceptStateResult;
}
DWORD MonitorWorker_PendingTargets(MonitorTarget targets[MAX_MONITORS])
{
    memset(targets, 0, sizeof(MonitorTarget) * MAX_MONITORS);
    for (int i = 0; i < g_monitors.count; i++) {
        targets[i].monitor = g_monitors.monitors[i];
        targets[i].percent = mockPendingTarget;
    }
    return mockPendingMask;
}

void UI_SetMasterTarget(int target) { lastUiTarget = target; }
void UI_MonitorSelectionSetSourcePollInterval(UINT milliseconds) { sourceTelemetryInterval = milliseconds; }
void UI_RefreshPopup(HWND hwnd, MonitorList *view)
{
    CHECK((hwnd == NULL || hwnd == testPopup) && view == &g_monitors);
    popupCalls++;
}
HWND UI_CreatePopup(HINSTANCE instance, MonitorList *view)
{
    (void)instance;
    CHECK(view == &g_monitors);
    return testPopup;
}

static BrightMonitor MakeMonitor(BOOL controllable, int percent)
{
    BrightMonitor monitor = {0};
    monitor.controllable = controllable;
    monitor.backend = controllable ? BACKEND_DDC : BACKEND_NONE;
    monitor.hasHandle = controllable;
    monitor.brightnessMax = 100;
    monitor.brightnessCur = (DWORD)percent;
    monitor.rangeLo = mockRangeLo;
    monitor.rangeHi = mockRangeHi;
    wcscpy_s(monitor.deviceInstance, 256, L"TEST\\MONITOR");
    return monitor;
}

static void ResetState(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    memset(&g_monitors, 0, sizeof(g_monitors));
    memset(&g_rescan, 0, sizeof(g_rescan));
    g_hInst = NULL;
    g_hwndHidden = testWindow;
    g_hwndPopup = NULL;
    g_scheduleSuspended = FALSE;
    g_scheduleSuspendMinute = g_scheduleResumeMinute = 0;
    g_scheduleResumeLocalMinute = 0;
    g_scheduleLastApplied = -1;
    g_masterTarget = 0;
    g_masterTargetValid = FALSE;
    g_masterTargetExplicit = FALSE;
    g_idleDimmed = FALSE;
    g_idleEpoch = 0;
    g_awaitRetry = 0;
    g_remoteSeen = FALSE;
    g_lastRemoteTick = 0;
    g_settings.sourcePollSeconds = DEFAULT_SOURCE_POLL_SECONDS;
    nowMinute = 570; /* 09:30 */
    nowDay = 1;
    dayValue = 82;
    mockRangeLo = 0;
    mockRangeHi = 100;
    nowTick = lastInputTick = 60000;
    dayCalls = setAllCalls = setOneCalls = actualWrites = 0;
    lastBase = lastUiTarget = -999;
    refreshCalls = popupCalls = destroyCalls = workerResets = cleanupCalls = 0;
    timerCalls = killCalls = 0;
    sourceRefreshCalls = pickerRefreshCalls = 0;
    sourceTelemetryInterval = 0;
    pickerOpen = FALSE;
    mockPendingMask = mockPendingTarget = 0;
    acceptResult = TRUE;
    acceptStateResult = TRUE;
    acceptCalls = 0;
    idleWrites = releaseWrites = restoreRequests = cancelCalls = 0;
    mockWorkerRunning = FALSE;
    mockReleaseSucceeds = TRUE;
    mockNormalSucceeds = TRUE;
    releaseRequests = 0;
    blackUpdates = blackClears = 0;
    blackIdle = blackEnabled = FALSE;
    osdCalls = osdPercent = 0;
    osdMonitor = NULL;
    popupVisible = FALSE;
    lastTimer = lastKilledTimer = 0;
    lastInterval = 0;
}

static void ConfigureSchedule(void)
{
    g_settings.scheduleEnabled = TRUE;
    g_settings.scheduleCount = 2;
    g_settings.schedule[0] = (SchedulePoint){480, 40};
    g_settings.schedule[1] = (SchedulePoint){720, 20};
}

static void AddReadyMonitor(void)
{
    g_monitors.count = 1;
    g_monitors.monitors[0] = MakeMonitor(TRUE, 50);
}

static void DeliverRescan(int count, BOOL controllable, int percent)
{
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return;
    fresh->count = count;
    if (count) fresh->monitors[0] = MakeMonitor(controllable, percent);
    DWORD generation = ++g_rescan.generation;
    g_rescan.awaitedGeneration = generation;
    g_rescan.busy = 1;
    HandleRescanResult(testWindow, generation, fresh);
    CHECK(g_rescan.busy == 0 && g_rescan.awaitedGeneration == 0);
}

static void TestCommandLine(void)
{
    CHECK(!IsWindowsLoginLaunch(NULL));
    CHECK(!IsWindowsLoginLaunch(L""));
    CHECK(!IsWindowsLoginLaunch(L"lumos.exe"));
    CHECK(!IsWindowsLoginLaunch(L"\"C:\\--startup\\lumos.exe\""));
    CHECK(IsWindowsLoginLaunch(L"lumos.exe " LUMOS_STARTUP_ARGUMENT));
    CHECK(IsWindowsLoginLaunch(L"\"C:\\Program Files\\Lumos\\lumos.exe\" \""
                               LUMOS_STARTUP_ARGUMENT L"\""));
    CHECK(IsWindowsLoginLaunch(L"lumos.exe --STARTUP"));
    CHECK(IsWindowsLoginLaunch(L"lumos.exe other " LUMOS_STARTUP_ARGUMENT));
    CHECK(!IsWindowsLoginLaunch(LUMOS_STARTUP_ARGUMENT));
    CHECK(!IsWindowsLoginLaunch(L"lumos.exe --startup=1"));
    CHECK(!IsWindowsLoginLaunch(L"lumos.exe --startup-extra"));
    CHECK(!IsWindowsLoginLaunch(L"lumos.exe prefix--startup"));
    CHECK(!IsWindowsLoginLaunch(L"lumos.exe \"text --startup\""));
}

static void TestManualStartup(void)
{
    ResetState();
    AddReadyMonitor();
    ApplyStartupBrightness(FALSE);
    CHECK(dayCalls == 0 && setAllCalls == 0 && actualWrites == 0);
    CHECK(!g_masterTargetValid && !g_scheduleSuspended);
    CHECK(!g_rescan.reapplyBrightness && timerCalls == 0 && killCalls == 0);

    ConfigureSchedule();
    ApplyStartupBrightness(FALSE);
    CHECK(dayCalls == 0 && setAllCalls == 1 && actualWrites == 1);
    CHECK(lastBase == Schedule_BrightnessAt(g_settings.schedule, 2, nowMinute));
    CHECK(lastBase != dayValue && lastUiTarget == lastBase);
    CHECK(g_masterTargetValid && !g_scheduleSuspended);
    CHECK(!g_rescan.reapplyBrightness && timerCalls == 0);
}

static void TestLoginUntilNextAnchor(void)
{
    ResetState();
    ConfigureSchedule();
    AddReadyMonitor();
    g_idleDimmed = TRUE;
    g_rescan.retry = RESCAN_MAX_RETRIES;
    g_rescan.writeOffs = RESCAN_MAX_WRITEOFFS;
    ApplyStartupBrightness(TRUE);
    CHECK(dayCalls == 1 && lastBase == 82 && lastUiTarget == 82);
    CHECK(actualWrites == 1 && g_monitors.monitors[0].brightnessCur == 82);
    CHECK(refreshCalls == 1 && popupCalls == 1);
    CHECK(g_masterTargetValid && g_masterTarget == 82 && !g_idleDimmed);
    CHECK(g_scheduleSuspended && g_scheduleSuspendMinute == 570);
    CHECK(g_scheduleResumeMinute == 720 && g_scheduleLastApplied == -1);
    CHECK(g_rescan.reapplyBrightness && g_rescan.retry == 0 && g_rescan.writeOffs == 0);
    CHECK(killCalls == 1 && lastKilledTimer == RESCAN_RETRY_TIMER_ID);
    CHECK(timerCalls == 1 && lastTimer == RESCAN_TIMER_ID && lastInterval == 600);

    nowMinute = 719;
    Schedule_ApplyNow();
    CHECK(g_scheduleSuspended && setAllCalls == 1 && lastBase == 82);
    nowMinute = 720;
    Schedule_ApplyNow();
    CHECK(!g_scheduleSuspended && setAllCalls == 2 && lastBase == 20);
    CHECK(g_masterTarget == 20 && g_scheduleLastApplied == 20);
    DeliverRescan(1, TRUE, 100);
    CHECK(!g_rescan.reapplyBrightness && lastBase == 20 && setAllCalls == 3);
    CHECK(g_monitors.monitors[0].brightnessCur == 20);
    CHECK(dayCalls == 1); /* Recovery reads current intent rather than Day again. */
}

static void TestManualOverrideBeforeRescan(void)
{
    ResetState();
    ConfigureSchedule();
    AddReadyMonitor();
    ApplyStartupBrightness(TRUE);
    /* The popup previews/queues the write before notifying the application. */
    Monitor_SetAllBrightness(&g_monitors, 25);
    SliderManualChange(-1, 25);
    CHECK(g_masterTarget == 25 && g_scheduleSuspended);
    DeliverRescan(1, TRUE, 100);
    CHECK(lastBase == 25 && lastUiTarget == 25 && !g_rescan.reapplyBrightness);
    CHECK(g_monitors.monitors[0].brightnessCur == 25 && dayCalls == 1);
}

static void TestLoginExactlyAtAnchor(void)
{
    ResetState();
    ConfigureSchedule();
    AddReadyMonitor();
    nowMinute = 480;
    ApplyStartupBrightness(TRUE);
    CHECK(g_scheduleSuspendMinute == 480 && g_scheduleResumeMinute == 720);
    Schedule_ApplyNow();
    CHECK(g_scheduleSuspended && lastBase == 82 && setAllCalls == 1);
    nowMinute = 481;
    Schedule_ApplyNow();
    CHECK(g_scheduleSuspended && setAllCalls == 1);
    nowMinute = 720;
    Schedule_ApplyNow();
    CHECK(!g_scheduleSuspended && lastBase == 20 && setAllCalls == 2);
}

static void TestIdleTakesPrecedenceOnRescan(void)
{
    ResetState();
    ConfigureSchedule();
    AddReadyMonitor();
    ApplyStartupBrightness(TRUE);
    g_settings.idleDimPercent = 12;
    g_idleDimmed = TRUE; /* The idle subsystem entered its holding state. */
    DeliverRescan(1, TRUE, 100);
    CHECK(!g_rescan.reapplyBrightness && g_idleDimmed);
    CHECK(idleWrites == 1 && lastUiTarget == 12 && g_masterTarget == 82);
    CHECK(g_monitors.monitors[0].brightnessCur == 12 && dayCalls == 1);
    Schedule_ApplyNow();
    CHECK(idleWrites == 1 && g_scheduleLastApplied == -1);
    Idle_Restore();
    CHECK(!g_idleDimmed && lastBase == 82 && g_scheduleSuspended);
}

static void TestSingleAnchorWaitsUntilTomorrow(void)
{
    ResetState();
    ConfigureSchedule();
    g_settings.scheduleCount = 1;
    AddReadyMonitor();
    nowMinute = 480;
    ApplyStartupBrightness(TRUE);
    CHECK(g_scheduleResumeMinute == 480 && g_scheduleResumeLocalMinute > 0);
    Schedule_ApplyNow();
    CHECK(g_scheduleSuspended && setAllCalls == 1);
    nowMinute = 481;
    Schedule_ApplyNow();
    CHECK(g_scheduleSuspended && setAllCalls == 1);
    nowMinute = 1439;
    Schedule_ApplyNow();
    CHECK(g_scheduleSuspended && setAllCalls == 1);
    nowDay = 2;
    nowMinute = 479;
    Schedule_ApplyNow();
    CHECK(g_scheduleSuspended && setAllCalls == 1);
    nowMinute = 480;
    Schedule_ApplyNow();
    CHECK(!g_scheduleSuspended && lastBase == 40 && setAllCalls == 2);
}

static void TestResumeAfterTwoDaySleep(void)
{
    ResetState();
    ConfigureSchedule();
    g_settings.scheduleCount = 1;
    AddReadyMonitor();
    nowMinute = 480;
    ApplyStartupBrightness(TRUE);
    nowDay = 3;
    nowMinute = 479; /* Yesterday's anchor passed, even though today's has not. */
    Schedule_ApplyNow();
    CHECK(!g_scheduleSuspended && lastBase == 40 && setAllCalls == 2);
}

static void TestUnavailableThenLateReady(void)
{
    ResetState();
    ApplyStartupBrightness(TRUE);
    CHECK(dayCalls == 1 && g_masterTarget == 82 && g_masterTargetValid);
    CHECK(g_rescan.reapplyBrightness && !g_scheduleSuspended && actualWrites == 0);
    for (int retry = 0; retry < RESCAN_MAX_RETRIES; retry++) {
        int oldTimers = timerCalls;
        DeliverRescan(retry % 2, FALSE, 100); /* Empty and placeholder enumerations. */
        CHECK(g_rescan.reapplyBrightness && g_rescan.retry == retry + 1);
        CHECK(timerCalls == oldTimers + 1 && lastTimer == RESCAN_RETRY_TIMER_ID);
        CHECK(lastInterval == kRescanBackoffMs[retry]);
        CHECK(g_monitors.count == 0 && workerResets == 0 && actualWrites == 0);
    }
    DeliverRescan(1, FALSE, 100); /* Retry exhaustion may adopt a placeholder. */
    CHECK(g_monitors.count == 1 && !Monitor_HasControllable(&g_monitors));
    CHECK(g_rescan.retry == 0 && g_rescan.reapplyBrightness && workerResets == 1);
    CHECK(setAllCalls == 1 && actualWrites == 0);
    CHECK(cleanupCalls == RESCAN_MAX_RETRIES + 1 && refreshCalls == 2);

    ApplyPresetBrightness(35); /* New user intent while no display is available. */
    CHECK(g_masterTarget == 35 && g_rescan.reapplyBrightness && actualWrites == 0);
    DeliverRescan(1, TRUE, 100);
    CHECK(!g_rescan.reapplyBrightness && lastBase == 35 && actualWrites == 1);
    CHECK(g_monitors.monitors[0].brightnessCur == 35 && dayCalls == 1);
    CHECK(destroyCalls == 1 && workerResets == 2);

    int oldWrites = actualWrites, oldSetAll = setAllCalls;
    DeliverRescan(1, TRUE, 60); /* Ordinary later rescan only refreshes brightness. */
    CHECK(!g_rescan.reapplyBrightness && setAllCalls == oldSetAll);
    CHECK(actualWrites == oldWrites && g_monitors.monitors[0].brightnessCur == 60);
    CHECK(destroyCalls == 2 && workerResets == 3 && dayCalls == 1);
    CHECK(refreshCalls == 5 && popupCalls == 3);
}

static void DeliverFailedWrite(BOOL accepted)
{
    MonitorResult *result = (MonitorResult *)calloc(1, sizeof(*result));
    CHECK(result != NULL);
    if (!result) return;
    result->index = 0;
    result->success = FALSE;
    acceptResult = accepted;
    HandleMonitorResult(testWindow, result); /* Takes ownership of result. */
}

static void TestWriteFailureRetriesLatestIntent(void)
{
    ResetState();
    AddReadyMonitor();
    ApplyStartupBrightness(TRUE);
    DeliverRescan(1, TRUE, 100);
    CHECK(!g_rescan.reapplyBrightness && g_monitors.monitors[0].brightnessCur == 82);
    int oldTimers = timerCalls, oldRefreshes = refreshCalls;
    DeliverFailedWrite(TRUE);
    CHECK(acceptCalls == 1 && g_rescan.reapplyBrightness);
    CHECK(refreshCalls == oldRefreshes + 1 && timerCalls == oldTimers + 1);
    CHECK(lastTimer == RESCAN_TIMER_ID && lastInterval == RESCAN_DEBOUNCE_MS);

    Monitor_SetAllBrightness(&g_monitors, 25);
    SliderManualChange(-1, 25);
    DeliverRescan(1, TRUE, 100);
    CHECK(!g_rescan.reapplyBrightness && lastBase == 25 && g_masterTarget == 25);
    CHECK(g_monitors.monitors[0].brightnessCur == 25 && dayCalls == 1);
    oldTimers = timerCalls;
    oldRefreshes = refreshCalls;
    DeliverFailedWrite(FALSE); /* Stale failure cannot schedule another recovery. */
    CHECK(acceptCalls == 2 && !g_rescan.reapplyBrightness);
    CHECK(timerCalls == oldTimers && refreshCalls == oldRefreshes && lastBase == 25);
}

static void ConfigureOledSelection(void)
{
    AddReadyMonitor();
    wcscpy(g_monitors.monitors[0].deviceInstance, L"TEST\\OLED");
    g_monitors.count = 2;
    g_monitors.monitors[1] = MakeMonitor(TRUE, 87);
    wcscpy(g_monitors.monitors[1].deviceInstance, L"TEST\\LCD");
    g_settings.monitorSelection.selectedOnly = TRUE;
    g_settings.monitorSelection.count = 1;
    CHECK(Settings_MonitorKey(&g_monitors.monitors[0], g_settings.monitorSelection.keys[0]));
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    CHECK(Monitor_CanControl(&g_monitors.monitors[0]));
    CHECK(!Monitor_CanControl(&g_monitors.monitors[1]));
}

static void TestAllPoliciesRespectGlobalSelection(void)
{
    ResetState();
    ConfigureOledSelection();
    ApplyStartupBrightness(TRUE);
    CHECK(g_monitors.monitors[0].brightnessCur == 82);
    CHECK(g_monitors.monitors[1].brightnessCur == 87 && actualWrites == 1);

    g_settings.presetCount = 1;
    g_settings.presets[0].brightness = 30;
    ApplyPreset(0);
    CHECK(g_monitors.monitors[0].brightnessCur == 30);
    CHECK(g_monitors.monitors[1].brightnessCur == 87 && actualWrites == 2);

    g_settings.idleDimPercent = 5;
    Idle_Dim();
    CHECK(g_idleDimmed && g_monitors.monitors[0].brightnessCur == 5);
    CHECK(g_monitors.monitors[1].brightnessCur == 87 && actualWrites == 3);
    Idle_Restore();
    CHECK(!g_idleDimmed && g_monitors.monitors[0].brightnessCur == 30);
    CHECK(g_monitors.monitors[1].brightnessCur == 87 && actualWrites == 4);

    ConfigureSchedule();
    Schedule_ApplyNow();
    CHECK(g_monitors.monitors[0].brightnessCur == (DWORD)BrightMap_Level(lastBase, mockRangeLo, mockRangeHi));
    CHECK(g_monitors.monitors[1].brightnessCur == 87 && actualWrites == 5);

    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return;
    fresh->count = 2;
    fresh->monitors[0] = g_monitors.monitors[1];
    fresh->monitors[1] = g_monitors.monitors[0];
    fresh->monitors[0].excludedFromControl = FALSE; /* New enumeration has no policy yet. */
    fresh->monitors[1].brightnessCur = 100;
    wcscpy(fresh->monitors[1].name, L"Renamed OLED");
    g_rescan.awaitedGeneration = ++g_rescan.generation;
    HandleRescanResult(testWindow, g_rescan.generation, fresh);
    CHECK(!Monitor_CanControl(&g_monitors.monitors[0]));
    CHECK(Monitor_CanControl(&g_monitors.monitors[1]));
    CHECK(g_monitors.monitors[0].brightnessCur == 87 && actualWrites == 6);
}

static void TestScopeChangeAndDisconnectedSelection(void)
{
    ResetState();
    ConfigureOledSelection();
    ApplyStartupBrightness(TRUE);
    CHECK(Settings_MonitorKey(&g_monitors.monitors[1], g_settings.monitorSelection.keys[0]));
    UpdateMonitorSelection();
    CHECK(workerResets == 1 && g_masterTarget == 87 && g_masterTargetValid);
    CHECK(!Monitor_CanControl(&g_monitors.monitors[0]));
    ApplyPresetBrightness(20);
    CHECK(g_monitors.monitors[0].brightnessCur == 82 && g_monitors.monitors[1].brightnessCur == 20);

    wcscpy(g_settings.monitorSelection.keys[0], L"DDC:DISCONNECTED");
    UpdateMonitorSelection();
    int oldWrites = actualWrites;
    CHECK(!g_masterTargetValid && !Monitor_HasSelected(&g_monitors));
    ApplyStartupBrightness(TRUE);
    Idle_Dim();
    Idle_Restore();
    CHECK(actualWrites == oldWrites);
    g_rescan.retry = RESCAN_MAX_RETRIES;
    DeliverRescan(1, TRUE, 75); /* An unrelated monitor must not become a fallback. */
    CHECK(g_rescan.reapplyBrightness && actualWrites == oldWrites);
    CHECK(!Monitor_HasSelected(&g_monitors) && g_monitors.monitors[0].brightnessCur == 75);

    g_settings.monitorSelection.selectedOnly = FALSE;
    UpdateMonitorSelection();
    ApplyPresetBrightness(45);
    CHECK(Monitor_HasSelected(&g_monitors) && g_monitors.monitors[0].brightnessCur == 45);
}

static void TestHotkeysAndFeedbackUseSelectedMonitor(void)
{
    ResetState();
    ConfigureOledSelection();
    g_monitors.monitors[0].hMonitor = (HMONITOR)(UINT_PTR)1;
    g_monitors.monitors[1].hMonitor = (HMONITOR)(UINT_PTR)2;
    g_settings.step = 5;
    ApplyStartupBrightness(TRUE);
    HandleHotkey(WM_HOTKEY_BRIGHTEN);
    CHECK(g_masterTarget == 87 && g_monitors.monitors[0].brightnessCur == 87);
    CHECK(g_monitors.monitors[1].brightnessCur == 87);
    CHECK(osdCalls == 1 && osdMonitor == (HMONITOR)(UINT_PTR)1 && osdPercent == 87);
    g_settings.monitorSelection.count = 0;
    UpdateMonitorSelection();
    int writes = actualWrites;
    HandleHotkey(WM_HOTKEY_DIM);
    CHECK(actualWrites == writes && osdCalls == 1 && !g_masterTargetValid);
}

static void TestScopeChangeWhileDimmedKeepsNewDisplayLevel(void)
{
    ResetState();
    ConfigureOledSelection();
    ApplyStartupBrightness(TRUE);
    g_settings.idleDimPercent = 5;
    Idle_Dim();
    CHECK(g_monitors.monitors[0].brightnessCur == 5);
    CHECK(Settings_MonitorKey(&g_monitors.monitors[1], g_settings.monitorSelection.keys[0]));
    int writes = actualWrites;
    UpdateMonitorSelection();
    CHECK(!g_idleDimmed && g_masterTarget == 87 && actualWrites == writes);
    CHECK(g_monitors.monitors[0].brightnessCur == 5 && g_monitors.monitors[1].brightnessCur == 87);
    Idle_Restore();
    CHECK(actualWrites == writes && g_monitors.monitors[1].brightnessCur == 87);

    ResetState();
    ConfigureOledSelection();
    ApplyStartupBrightness(TRUE);
    Idle_Dim();
    writes = actualWrites;
    g_settings.monitorSelection.selectedOnly = FALSE;
    UpdateMonitorSelection();
    CHECK(!g_idleDimmed && actualWrites == writes + 1); /* Only the retained OLED restores. */
    CHECK(g_monitors.monitors[0].brightnessCur == 82 && g_monitors.monitors[1].brightnessCur == 87);
    CHECK(g_masterTarget == 84); /* Average of the restored/current bases, 82 and 87. */

    g_settings.monitorSelection.selectedOnly = TRUE;
    g_settings.monitorSelection.count = 0;
    UpdateMonitorSelection();
    Idle_Dim(); /* No display was dimmed; the internal fallback must not transfer. */
    g_settings.monitorSelection.count = 1;
    CHECK(Settings_MonitorKey(&g_monitors.monitors[1], g_settings.monitorSelection.keys[0]));
    writes = actualWrites;
    UpdateMonitorSelection();
    CHECK(!g_idleDimmed && actualWrites == writes && g_masterTarget == 87);
    CHECK(g_monitors.monitors[1].brightnessCur == 87);
}

static void DeliverSourceResultAt(int index, int kind, BOOL known, DWORD input)
{
    MonitorResult *result = (MonitorResult *)calloc(1, sizeof(*result));
    CHECK(result != NULL);
    if (!result) return;
    result->kind = kind;
    result->index = index;
    result->sourceKnown = known;
    result->sourceUpdated = TRUE;
    result->currentInput = input;
    result->sourceCheckedTick = 100;
    HandleMonitorResult(testWindow, result);
}

static void DeliverSourceResult(int kind, BOOL known, DWORD input)
{
    DeliverSourceResultAt(0, kind, known, input);
}

static void ConfigureTwoFilteredMonitors(void)
{
    g_monitors.count = 2;
    g_monitors.monitors[0] = MakeMonitor(TRUE, 73);
    g_monitors.monitors[1] = MakeMonitor(TRUE, 128);
    g_monitors.monitors[1].brightnessMax = 255;
    wcscpy(g_monitors.monitors[1].deviceInstance, L"TEST\\SECOND");
    for (int i = 0; i < g_monitors.count; i++) {
        BrightMonitor *monitor = &g_monitors.monitors[i];
        monitor->rangeLo = 0; monitor->rangeHi = 100;
        monitor->sourceFilter = TRUE;
        monitor->expectedInput = 0x0f;
        monitor->sourceKnown = TRUE;
        monitor->currentInput = 0x0f;
    }
    g_settings.idleDimPercent = 5;
}

static void TestIdleSourceHandoffWhilePcStaysIdle(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    BrightMonitor *first = &g_monitors.monitors[0];
    BrightMonitor *second = &g_monitors.monitors[1];
    second->desiredBrightnessValid = TRUE;
    second->desiredBrightness = 62;
    Idle_Dim();
    CHECK(g_idleDimmed && idleWrites == 2 && releaseWrites == 0);
    CHECK(first->brightnessCur == 5 && second->brightnessCur == 12);
    CHECK(first->preIdleBrightnessValid && first->preIdleBrightness == 73);
    CHECK(second->preIdleBrightnessValid && second->preIdleBrightness == 128);
    CHECK(first->idleApplied && second->idleApplied);
    CHECK(second->desiredBrightnessValid && second->desiredBrightness == 62);
    int master = g_masterTarget;

    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(g_idleDimmed && g_masterTarget == master);
    CHECK(first->brightnessCur == 5 && first->idleApplied);
    CHECK(second->brightnessCur == 128 && !second->idleApplied);
    CHECK(idleWrites == 2 && releaseWrites == 1 && actualWrites == 3);
    CHECK(second->preIdleBrightnessValid && second->preIdleBrightness == 128);
    CHECK(second->desiredBrightness == 62);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x11);
    CHECK(releaseWrites == 1 && actualWrites == 3);

    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(g_idleDimmed && second->idleApplied && second->brightnessCur == 12);
    CHECK(idleWrites == 3 && releaseWrites == 1 && first->brightnessCur == 5);
    CHECK(second->preIdleBrightness == 128 && second->desiredBrightness == 62);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(idleWrites == 3 && actualWrites == 4);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(g_idleDimmed && second->brightnessCur == 128 && releaseWrites == 2);
    CHECK(first->brightnessCur == 5 && first->preIdleBrightness == 73);
}

static void TestIdleSourceHandoffNeedsKnownSourceAndOwnership(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    BrightMonitor *second = &g_monitors.monitors[1];
    Idle_Dim();
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, FALSE, 0);
    CHECK(g_idleDimmed && second->idleApplied && second->brightnessCur == 12);
    CHECK(releaseWrites == 0 && !g_rescan.reapplyBrightness);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->brightnessCur == 128 && releaseWrites == 1);

    ResetState();
    ConfigureTwoFilteredMonitors();
    second = &g_monitors.monitors[1];
    second->currentInput = 0x12;
    Idle_Dim();
    CHECK(g_idleDimmed && idleWrites == 1 && second->brightnessCur == 128);
    CHECK(!second->idleApplied);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(releaseWrites == 0 && actualWrites == 1);
    second->brightnessCur = 173; /* Another source can change its own brightness. */
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(second->idleApplied && second->brightnessCur == 12 && idleWrites == 2);
    CHECK(second->preIdleBrightnessValid && second->preIdleBrightness == 173);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->brightnessCur == 173 && releaseWrites == 1 && g_idleDimmed);
}

static void TestIdleSkippedWriteReleasesAlreadyDimmedMonitor(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    Idle_Dim();
    DeliverSourceResultAt(1, MONITOR_RESULT_SKIPPED, TRUE, 0x12);
    CHECK(g_monitors.monitors[1].brightnessCur == 128);
    CHECK(releaseWrites == 1 && g_idleDimmed && !g_rescan.reapplyBrightness);
    CHECK(g_monitors.monitors[0].brightnessCur == 5);
}

static void DeliverIdleWriteResult(int index, DWORD epoch, DWORD preIdleRaw)
{
    MonitorResult *result = (MonitorResult *)calloc(1, sizeof(*result));
    CHECK(result != NULL);
    if (!result) return;
    result->index = index;
    result->success = TRUE;
    result->kind = MONITOR_RESULT_BRIGHTNESS;
    result->purpose = MONITOR_WRITE_IDLE;
    result->brightnessWritten = TRUE;
    result->idleEpoch = epoch;
    result->preIdleBrightnessValid = TRUE;
    result->preIdleBrightness = preIdleRaw;
    result->minimum = 0;
    result->maximum = g_monitors.monitors[index].brightnessMax;
    result->current = Brightness_ToRaw(&g_monitors.monitors[index],
                                      (DWORD)g_settings.idleDimPercent);
    result->sourceUpdated = TRUE;
    result->sourceKnown = TRUE;
    result->currentInput = 0x0f;
    result->sourceCheckedTick = 99;
    HandleMonitorResult(testWindow, result);
}

static void TestSupersededIdleSuccessStillRestoresHardware(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    BrightMonitor *second = &g_monitors.monitors[1];
    mockWorkerRunning = TRUE;
    Idle_Dim();
    DWORD epoch = second->idleEpoch;
    CHECK(g_idleDimmed && !second->idleApplied && second->idleDimPending);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->currentInput == 0x12 && releaseWrites == 0);
    second->brightnessCur = 12; /* Hardware was dimmed before its stale acknowledgement. */
    mockWorkerRunning = FALSE;
    acceptResult = FALSE;
    DeliverIdleWriteResult(1, epoch, 128);
    CHECK(second->currentInput == 0x12 && second->sourceKnown);
    CHECK(second->preIdleBrightnessValid && second->preIdleBrightness == 128);
    CHECK(second->brightnessCur == 128 && releaseWrites == 1 && !second->idleApplied);
    CHECK(g_idleDimmed);

    /* A replaced topology cannot acquire ownership from an old success. */
    acceptStateResult = FALSE;
    DeliverIdleWriteResult(1, epoch, 200);
    CHECK(second->preIdleBrightness == 128 && releaseWrites == 1 && !second->idleApplied);

    acceptResult = acceptStateResult = TRUE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(second->idleApplied && second->preIdleBrightness == 128);
    MonitorResult *refresh = (MonitorResult *)calloc(1, sizeof(*refresh));
    CHECK(refresh != NULL);
    if (!refresh) return;
    refresh->index = 1;
    refresh->success = TRUE;
    refresh->kind = MONITOR_RESULT_BRIGHTNESS;
    refresh->maximum = 255;
    refresh->current = 12;
    HandleMonitorResult(testWindow, refresh);
    CHECK(second->idleApplied && second->preIdleBrightness == 128);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->brightnessCur == 128 && releaseWrites == 2 && g_idleDimmed);
}

static void TestFailedIdleReleaseRetainsBaselineUntilNextPoll(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    Idle_Dim();
    BrightMonitor *second = &g_monitors.monitors[1];
    mockReleaseSucceeds = FALSE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(releaseRequests == 1 && releaseWrites == 0 && second->idleApplied);
    CHECK(second->preIdleBrightness == 128 && !second->idleReleasePending);
    CHECK(g_idleDimmed && !g_rescan.reapplyBrightness && timerCalls == 0);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, FALSE, 0);
    CHECK(releaseRequests == 1 && second->idleApplied && second->preIdleBrightness == 128);
    mockReleaseSucceeds = TRUE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(releaseRequests == 2 && releaseWrites == 1 && second->brightnessCur == 128);

    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    mockWorkerRunning = TRUE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->idleReleasePending && second->idleApplied && releaseRequests == 3);
    MonitorResult *failed = (MonitorResult *)calloc(1, sizeof(*failed));
    CHECK(failed != NULL);
    if (!failed) return;
    failed->index = 1;
    failed->kind = MONITOR_RESULT_BRIGHTNESS;
    failed->purpose = MONITOR_WRITE_IDLE_RELEASE;
    failed->idleEpoch = second->idleEpoch;
    failed->sourceUpdated = TRUE;
    failed->sourceKnown = FALSE;
    HandleMonitorResult(testWindow, failed);
    CHECK(second->idleApplied && !second->idleReleasePending && second->preIdleBrightness == 128);
    CHECK(releaseRequests == 3 && !g_rescan.reapplyBrightness && timerCalls == 0);
    mockWorkerRunning = FALSE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->brightnessCur == 128 && releaseRequests == 4 && releaseWrites == 2);
    CHECK(g_idleDimmed && g_monitors.monitors[0].brightnessCur == 5);
}

static void TestWakePreservesPendingAlternateInputRestore(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    Idle_Dim();
    BrightMonitor *second = &g_monitors.monitors[1];
    mockWorkerRunning = TRUE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->idleReleasePending && second->idleApplied && releaseRequests == 1);
    Idle_Restore();
    CHECK(!g_idleDimmed && second->idleApplied && second->preIdleBrightness == 128);
    CHECK(second->idleReleasePending && releaseRequests == 2);
    MonitorResult *completed = (MonitorResult *)calloc(1, sizeof(*completed));
    CHECK(completed != NULL);
    if (!completed) return;
    completed->index = 1;
    completed->kind = MONITOR_RESULT_BRIGHTNESS;
    completed->purpose = MONITOR_WRITE_IDLE_RELEASE;
    completed->success = TRUE;
    completed->brightnessWritten = TRUE;
    completed->idleEpoch = second->idleEpoch;
    completed->maximum = 255;
    completed->current = 128;
    completed->sourceUpdated = TRUE;
    completed->sourceKnown = TRUE;
    completed->currentInput = 0x12;
    HandleMonitorResult(testWindow, completed);
    CHECK(second->brightnessCur == 128 && !second->idleApplied && !second->idleReleasePending);
    CHECK(!second->preIdleBrightnessValid && !g_idleDimmed);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(releaseRequests == 2);
}

static void TestReturningIdleInputSupersedesQueuedRelease(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    Idle_Dim();
    BrightMonitor *second = &g_monitors.monitors[1];
    DWORD epoch = second->idleEpoch;
    mockWorkerRunning = TRUE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->idleReleasePending && releaseRequests == 1);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(second->idleDimPending && !second->idleReleasePending);
    CHECK(second->idleEpoch == epoch && second->preIdleBrightness == 128);
    DeliverIdleWriteResult(1, epoch, 12); /* A repeat dim cannot replace the original baseline. */
    CHECK(second->idleApplied && !second->idleDimPending && !second->idleReleasePending);
    CHECK(second->preIdleBrightness == 128 && g_idleDimmed);
    mockWorkerRunning = FALSE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(releaseRequests == 2 && releaseWrites == 1 && second->brightnessCur == 128);

    /* A policy reapply can also replace an outstanding release before its ACK. */
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    mockWorkerRunning = TRUE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->idleReleasePending && releaseRequests == 3);
    second->currentInput = 0x0f;
    ReapplyBrightness();
    CHECK(second->idleDimPending && !second->idleReleasePending);
    DeliverIdleWriteResult(1, epoch, 12);
    mockWorkerRunning = FALSE;
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(releaseRequests == 4 && releaseWrites == 2 && second->brightnessCur == 128);
    CHECK(g_idleDimmed && second->preIdleBrightness == 128);
}

static void TestSourceRuleEditKeepsPendingIdleWritePurpose(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    g_settings.monitorSelection.inputRuleCount = 2;
    for (int i = 0; i < g_monitors.count; i++) {
        MonitorInputRule *rule = &g_settings.monitorSelection.inputRules[i];
        CHECK(Settings_MonitorKey(&g_monitors.monitors[i], rule->key));
        rule->enabled = TRUE;
        rule->input = 0x0f;
    }
    BrightMonitor *second = &g_monitors.monitors[1];
    second->desiredBrightnessValid = TRUE;
    second->desiredBrightness = 62;
    Idle_Dim();
    CHECK(idleWrites == 2 && second->idleApplied && second->preIdleBrightness == 128);
    second->idleDimPending = TRUE;
    mockPendingMask = 1u << 1;
    mockPendingTarget = 5;
    int normalCalls = setOneCalls;
    g_settings.monitorSelection.inputRules[0].input = 0x11;
    UpdateMonitorSelection();
    CHECK(workerResets == 1 && g_idleDimmed && idleWrites == 3);
    CHECK(setOneCalls == normalCalls); /* Replaying an idle target must never become a normal write. */
    CHECK(second->idleApplied && !second->idleDimPending && second->brightnessCur == 12);
    CHECK(second->desiredBrightnessValid && second->desiredBrightness == 62);
    CHECK(second->preIdleBrightnessValid && second->preIdleBrightness == 128);
    DeliverSourceResultAt(1, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(second->brightnessCur == 128 && releaseWrites == 1 && g_idleDimmed);
}

static BrightMonitor PrepareReorderedRescanBeforeIdleAcknowledgement(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    g_settings.monitorSelection.inputRuleCount = 2;
    for (int i = 0; i < g_monitors.count; i++) {
        MonitorInputRule *rule = &g_settings.monitorSelection.inputRules[i];
        CHECK(Settings_MonitorKey(&g_monitors.monitors[i], rule->key));
        rule->enabled = TRUE;
        rule->input = 0x0f;
    }
    mockWorkerRunning = TRUE;
    Idle_Dim();
    BrightMonitor original = g_monitors.monitors[1];
    CHECK(original.idleDimPending && !original.preIdleBrightnessValid);
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return original;
    fresh->count = 2;
    fresh->monitors[0] = g_monitors.monitors[1];
    fresh->monitors[1] = g_monitors.monitors[0];
    fresh->monitors[0].idleDimPending = FALSE;
    fresh->monitors[1].idleDimPending = FALSE;
    fresh->monitors[0].brightnessCur = 12; /* Native dim committed, its acknowledgement is still queued. */
    AdoptMonitorList(fresh);
    CHECK(g_monitors.monitors[0].idleEpoch == original.idleEpoch);
    CHECK(!g_monitors.monitors[0].preIdleBrightnessValid && !g_monitors.monitors[0].idleApplied);
    DeliverSourceResultAt(0, MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(releaseWrites == 0 && g_monitors.monitors[0].brightnessCur == 12);
    mockWorkerRunning = FALSE;
    acceptResult = acceptStateResult = FALSE;
    return original;
}

static void DeliverCrossResetIdleSuccess(const BrightMonitor *original)
{
    MonitorResult *result = (MonitorResult *)calloc(1, sizeof(*result));
    CHECK(result != NULL);
    if (!result) return;
    result->index = 1; /* Old index now belongs to a different monitor. */
    result->kind = MONITOR_RESULT_BRIGHTNESS;
    result->purpose = MONITOR_WRITE_IDLE;
    result->success = result->brightnessWritten = TRUE;
    result->idleEpoch = original->idleEpoch;
    result->preIdleBrightnessValid = TRUE;
    result->preIdleBrightness = 128;
    result->minimum = original->brightnessMin;
    result->maximum = original->brightnessMax;
    result->current = 12;
    result->sourceKnown = result->sourceUpdated = TRUE;
    result->currentInput = 0x0f;
    result->sourceCheckedTick = 99;
    wcscpy(result->deviceInstance, original->deviceInstance);
    result->backend = original->backend;
    result->sourceFilter = original->sourceFilter;
    result->expectedInput = original->expectedInput;
    HandleMonitorResult(testWindow, result);
}

static void TestReorderedRescanReconcilesAppliedIdleByIdentity(void)
{
    BrightMonitor original = PrepareReorderedRescanBeforeIdleAcknowledgement();
    DeliverCrossResetIdleSuccess(&original);
    BrightMonitor *second = &g_monitors.monitors[0];
    CHECK(second->preIdleBrightnessValid && second->preIdleBrightness == 128);
    CHECK(second->brightnessCur == 128 && releaseWrites == 1 && !second->idleApplied);
    CHECK(second->sourceKnown && second->currentInput == 0x12);
    CHECK(g_monitors.monitors[1].brightnessCur == 73 && !g_monitors.monitors[1].preIdleBrightnessValid);
    CHECK(g_idleDimmed);
}

static void TestCrossResetIdleAcknowledgementRejectsChangedIdentityOrPolicy(void)
{
    for (int scenario = 0; scenario < 6; scenario++) {
        BrightMonitor original = PrepareReorderedRescanBeforeIdleAcknowledgement();
        switch (scenario) {
        case 0:
            wcscpy(original.deviceInstance, L"TEST\\DISCONNECTED");
            break;
        case 1:
            original.expectedInput = 0x11;
            break;
        case 2:
            original.idleEpoch++;
            break;
        case 3:
            wcscpy(g_monitors.monitors[1].deviceInstance, original.deviceInstance);
            break;
        case 4:
            original.sourceFilter = FALSE;
            break;
        case 5:
            original.deviceInstance[0] = L'\0';
            break;
        }
        DeliverCrossResetIdleSuccess(&original);
        CHECK(releaseWrites == 0 && actualWrites == 0);
        CHECK(g_monitors.monitors[0].brightnessCur == 12 && !g_monitors.monitors[0].preIdleBrightnessValid);
        CHECK(g_monitors.monitors[1].brightnessCur == 73 && !g_monitors.monitors[1].preIdleBrightnessValid);
        CHECK(g_monitors.monitors[0].currentInput == 0x12 && g_idleDimmed);
    }
}

static void TestSourceSuspensionAndResume(void)
{
    ResetState();
    AddReadyMonitor();
    BrightMonitor *monitor = &g_monitors.monitors[0];
    monitor->sourceFilter = TRUE;
    monitor->expectedInput = 0x0f;
    monitor->sourceKnown = TRUE;
    monitor->currentInput = 0x0f;
    ApplyPresetBrightness(60);
    CHECK(actualWrites == 1 && monitor->brightnessCur == 60);
    int timers = timerCalls;
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x12);
    CHECK(!Monitor_SourceAllowsControl(monitor) && Monitor_HasSelected(&g_monitors));
    ApplyPresetBrightness(35);
    ApplyPresetBrightness(42);
    CHECK(actualWrites == 1 && monitor->brightnessCur == 60);
    DeliverSourceResult(MONITOR_RESULT_SKIPPED, TRUE, 0x12);
    DeliverSourceResult(MONITOR_RESULT_SOURCE, FALSE, 0);
    CHECK(timerCalls == timers && !g_rescan.reapplyBrightness);
    CHECK(!monitor->sourceKnown);
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(actualWrites == 2 && monitor->brightnessCur == 42);
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(actualWrites == 2); /* No repeated writes on unchanged polls. */
    CHECK(pickerRefreshCalls == 5);

    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x12);
    g_settings.idleDimPercent = 5;
    g_idleDimmed = TRUE;
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(monitor->brightnessCur == 5 && g_masterTarget == 42);

    g_idleDimmed = FALSE;
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x12);
    ConfigureSchedule();
    g_scheduleSuspended = FALSE;
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(monitor->brightnessCur == 32 && g_masterTarget == 32);
}

static void TestSourcePollingAndRuleChanges(void)
{
    ResetState();
    AddReadyMonitor();
    HandleTimer(testWindow, SOURCE_TIMER_ID);
    CHECK(sourceRefreshCalls == 0);
    pickerOpen = TRUE;
    HandleTimer(testWindow, SOURCE_TIMER_ID);
    CHECK(sourceRefreshCalls == 1);
    pickerOpen = FALSE;
    MonitorInputRule *rule = &g_settings.monitorSelection.inputRules[0];
    g_settings.monitorSelection.inputRuleCount = 1;
    CHECK(Settings_MonitorKey(&g_monitors.monitors[0], rule->key));
    rule->enabled = TRUE;
    rule->input = 0x0f;
    g_masterTarget = 61;
    g_masterTargetValid = TRUE;
    g_masterTargetExplicit = TRUE;
    UpdateMonitorSelection();
    CHECK(workerResets == 1 && sourceRefreshCalls == 2);
    CHECK(g_masterTarget == 61 && !g_monitors.monitors[0].sourceKnown);
    HandleTimer(testWindow, SOURCE_TIMER_ID);
    CHECK(sourceRefreshCalls == 3); /* Suspended displays remain polled. */
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(g_monitors.monitors[0].brightnessCur == 61);
    int writes = actualWrites;
    g_monitors.monitors[0].excludedFromControl = TRUE;
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x12);
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(actualWrites == writes);
}

static void TestPollingAndPolicyAfterBrightnessRecovery(void)
{
    for (int scenario = 0; scenario < 5; scenario++) {
        ResetState();
        AddReadyMonitor();
        BrightMonitor *monitor = &g_monitors.monitors[0];
        monitor->controllable = FALSE;
        monitor->rangeLo = 0; monitor->rangeHi = 100;
        monitor->sourceFilter = TRUE;
        monitor->expectedInput = 0x0F;
        monitor->desiredBrightnessValid = TRUE;
        monitor->desiredBrightness = 62;
        monitor->excludedFromControl = scenario == 4;
        g_idleDimmed = scenario == 1;
        g_settings.idleDimPercent = 5;
        HandleTimer(testWindow, SOURCE_TIMER_ID);
        CHECK(sourceRefreshCalls == (scenario == 4 ? 0 : 1));

        MonitorResult *result = (MonitorResult *)calloc(1, sizeof(*result));
        CHECK(result != NULL);
        if (!result) return;
        result->kind = MONITOR_RESULT_SOURCE;
        result->sourceUpdated = TRUE;
        result->sourceKnown = scenario != 3;
        result->currentInput = scenario == 2 ? 0x12 : scenario == 3 ? 0 : 0x0F;
        result->brightnessUpdated = TRUE;
        result->minimum = 0;
        result->current = 80;
        result->maximum = 100;
        HandleMonitorResult(testWindow, result);
        CHECK(monitor->controllable && monitor->brightnessMax == 100);
        if (scenario == 0) CHECK(actualWrites == 1 && monitor->brightnessCur == 62);
        else if (scenario == 1) {
            CHECK(actualWrites == 1 && idleWrites == 1 && monitor->brightnessCur == 5);
            CHECK(monitor->preIdleBrightnessValid && monitor->preIdleBrightness == 80);
        } else CHECK(actualWrites == 0 && monitor->brightnessCur == 80);
    }
    ResetState();
    AddReadyMonitor();
    g_monitors.monitors[0].controllable = FALSE;
    HandleTimer(testWindow, SOURCE_TIMER_ID);
    CHECK(sourceRefreshCalls == 1); /* Retry discovery even with no source filter. */
}

static void TestIdleStateSurvivesUnavailableBrightnessOnRescan(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    g_settings.monitorSelection.inputRuleCount = 2;
    for (int i = 0; i < g_monitors.count; i++) {
        MonitorInputRule *rule = &g_settings.monitorSelection.inputRules[i];
        CHECK(Settings_MonitorKey(&g_monitors.monitors[i], rule->key));
        rule->enabled = TRUE;
        rule->input = 0x0F;
    }
    Idle_Dim();
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return;
    *fresh = g_monitors;
    fresh->monitors[1].controllable = FALSE;
    fresh->monitors[1].preIdleBrightnessValid = FALSE;
    fresh->monitors[1].idleApplied = FALSE;
    fresh->monitors[1].idleEpoch = 0;
    AdoptMonitorList(fresh);
    BrightMonitor *second = &g_monitors.monitors[1];
    CHECK(!second->controllable && second->preIdleBrightnessValid && second->preIdleBrightness == 128);
    CHECK(second->idleApplied && second->idleEpoch != 0);
    MonitorResult *result = (MonitorResult *)calloc(1, sizeof(*result));
    CHECK(result != NULL);
    if (!result) return;
    result->index = 1;
    result->kind = MONITOR_RESULT_SOURCE;
    result->sourceUpdated = result->sourceKnown = result->brightnessUpdated = TRUE;
    result->currentInput = 0x12;
    result->current = 12;
    result->maximum = 255;
    HandleMonitorResult(testWindow, result);
    CHECK(second->controllable && !second->idleApplied && releaseWrites == 1);
    CHECK(second->brightnessCur == 128 && second->preIdleBrightness == 128);
    CHECK(g_idleDimmed && g_monitors.monitors[0].brightnessCur == 5);
}

static void TestSourceResumeAfterRescanAndFilterRemoval(void)
{
    ResetState();
    AddReadyMonitor();
    MonitorInputRule *rule = &g_settings.monitorSelection.inputRules[0];
    g_settings.monitorSelection.inputRuleCount = 1;
    CHECK(Settings_MonitorKey(&g_monitors.monitors[0], rule->key));
    rule->enabled = TRUE;
    rule->input = 0x0f;
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    ApplyPresetBrightness(61);
    CHECK(actualWrites == 0);
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    fresh->count = 1;
    fresh->monitors[0] = MakeMonitor(TRUE, 50);
    fresh->monitors[0].sourceKnown = TRUE;
    fresh->monitors[0].currentInput = 0x0f;
    AdoptMonitorList(fresh);
    CHECK(!g_monitors.monitors[0].sourceKnown && sourceRefreshCalls == 1);
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(actualWrites == 1 && g_monitors.monitors[0].brightnessCur == 61);
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x12);
    ApplyPresetBrightness(42);
    rule->enabled = FALSE;
    UpdateMonitorSelection();
    CHECK(actualWrites == 2 && g_monitors.monitors[0].brightnessCur == 42);

    /* A source rule edit must not discard a pending unfiltered write. */
    g_monitors.count = 2;
    g_monitors.monitors[1] = MakeMonitor(TRUE, 20);
    wcscpy(g_monitors.monitors[1].deviceInstance, L"TEST\\SECOND");
    mockPendingMask = 1u << 1;
    mockPendingTarget = 77;
    rule->enabled = TRUE;
    UpdateMonitorSelection();
    CHECK(g_monitors.monitors[1].brightnessCur == 77);
}

static void TestSuspendedIntentSurvivesScopeChange(void)
{
    ResetState();
    ConfigureOledSelection();
    g_settings.monitorSelection.selectedOnly = FALSE;
    MonitorInputRule *rule = &g_settings.monitorSelection.inputRules[0];
    g_settings.monitorSelection.inputRuleCount = 1;
    CHECK(Settings_MonitorKey(&g_monitors.monitors[0], rule->key));
    rule->enabled = TRUE;
    rule->input = 0x0f;
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    g_monitors.monitors[0].sourceKnown = TRUE;
    g_monitors.monitors[0].currentInput = 0x12;
    ApplyPresetBrightness(42);
    g_settings.monitorSelection.selectedOnly = TRUE;
    UpdateMonitorSelection();
    CHECK(g_monitors.monitors[0].desiredBrightnessValid);
    CHECK(g_monitors.monitors[0].desiredBrightness == 42 && g_masterTarget == 42);
    int writes = actualWrites;
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 0x0f);
    CHECK(actualWrites == writes + 1 && g_monitors.monitors[0].brightnessCur == 42);
    /* Brightness-only snapshots must not replace current source telemetry. */
    MonitorResult *result = (MonitorResult *)calloc(1, sizeof(*result));
    result->success = TRUE;
    result->kind = MONITOR_RESULT_BRIGHTNESS;
    result->maximum = 100;
    result->current = 52;
    result->sourceKnown = TRUE;
    result->currentInput = 0x12;
    result->sourceCheckedTick = g_monitors.monitors[0].sourceCheckedTick;
    HandleMonitorResult(testWindow, result);
    CHECK(g_monitors.monitors[0].sourceKnown && g_monitors.monitors[0].currentInput == 0x0f);
}

static void TestOledBlackIdleKeepsPanelBrightness(void)
{
    ResetState();
    g_settings.idleDimEnabled = TRUE;
    g_settings.idleDimPercent = 5;
    g_masterTarget = 73;
    g_masterTargetValid = TRUE;
    g_monitors.count = 2;
    g_monitors.monitors[0] = MakeMonitor(TRUE, 73);
    g_monitors.monitors[1] = MakeMonitor(TRUE, 62);
    g_monitors.monitors[0].rangeHi = g_monitors.monitors[1].rangeHi = 100;
    g_monitors.monitors[1].idleBlack = TRUE;
    Idle_Dim();
    CHECK(g_idleDimmed && blackIdle && blackEnabled && blackUpdates > 0);
    CHECK(idleWrites == 1 && g_monitors.monitors[0].brightnessCur == 5);
    CHECK(g_monitors.monitors[1].brightnessCur == 62);
    CHECK(!g_monitors.monitors[1].preIdleBrightnessValid && !g_monitors.monitors[1].idleApplied);
    ReapplyBrightness();
    CHECK(g_monitors.monitors[1].brightnessCur == 62);
    int before = restoreRequests;
    Idle_Restore();
    CHECK(!g_idleDimmed && !blackIdle && blackClears > 0);
    CHECK(restoreRequests == before + 1 && g_monitors.monitors[0].brightnessCur == 73);
    CHECK(g_monitors.monitors[1].brightnessCur == 62);
    Idle_Dim();
    ManualChange();
    CHECK(!g_idleDimmed && !blackIdle);
}

static void TestIdleWakePreservesIndividualTargetsAndSchedulePrecedence(void)
{
    ResetState();
    ConfigureTwoReadyMonitors();
    Monitor_SetBrightness(&g_monitors.monitors[0], 20);
    Monitor_SetBrightness(&g_monitors.monitors[1], 80);
    g_masterTarget = 50;
    g_masterTargetValid = TRUE;
    g_settings.idleDimPercent = 5;
    Idle_Dim();
    CHECK(g_monitors.monitors[0].brightnessCur == 5 && g_monitors.monitors[1].brightnessCur == 5);
    Idle_Restore();
    CHECK(g_monitors.monitors[0].brightnessCur == 20 && g_monitors.monitors[1].brightnessCur == 80);
    CHECK(g_monitors.monitors[0].desiredBrightness == 20 && g_monitors.monitors[1].desiredBrightness == 80);
    CHECK(g_masterTarget == 50 && lastUiTarget == 50 && restoreRequests == 0);

    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return;
    *fresh = g_monitors;
    fresh->monitors[0].brightnessCur = fresh->monitors[1].brightnessCur = 100;
    g_rescan.reapplyBrightness = TRUE; /* A delayed wake rescan must preserve20/80 too. */
    AdoptMonitorList(fresh);
    CHECK(g_monitors.monitors[0].brightnessCur == 20 && g_monitors.monitors[1].brightnessCur == 80);
    CHECK(g_monitors.monitors[0].desiredBrightness == 20 && g_monitors.monitors[1].desiredBrightness == 80);

    ConfigureSchedule();
    Idle_Dim();
    nowMinute = 720; /* The current schedule wins over pre-idle individual levels. */
    Idle_Restore();
    CHECK(g_scheduleLastApplied == 20 && g_masterTarget == 20);
    CHECK(g_monitors.monitors[0].brightnessCur == 20 && g_monitors.monitors[1].brightnessCur == 20);
}

static void TestIdleWakeRestoresNativeBaselinesWithoutCreatingIntent(void)
{
    ResetState();
    ConfigureTwoReadyMonitors();
    g_monitors.monitors[0].brightnessMax = 255;
    g_monitors.monitors[0].brightnessCur = 73;
    g_monitors.monitors[1].brightnessMin = 10;
    g_monitors.monitors[1].brightnessMax = 255;
    g_monitors.monitors[1].brightnessCur = 213;
    g_settings.idleDimPercent = 5;
    Idle_Dim();
    CHECK(g_monitors.monitors[0].brightnessCur != 73 && g_monitors.monitors[1].brightnessCur != 213);
    Idle_Restore();
    CHECK(g_monitors.monitors[0].brightnessCur == 73 && g_monitors.monitors[1].brightnessCur == 213);
    CHECK(!g_monitors.monitors[0].desiredBrightnessValid && !g_monitors.monitors[1].desiredBrightnessValid);
    CHECK(!g_monitors.monitors[0].idleApplied && !g_monitors.monitors[1].idleApplied);
    CHECK(!g_monitors.monitors[0].preIdleBrightnessValid && !g_monitors.monitors[1].preIdleBrightnessValid);
    CHECK(restoreRequests == 2 && setOneCalls == 0 && setAllCalls == 0);
    CHECK(!g_masterTargetValid && !g_masterTargetExplicit); /* The displayed average is not a group request. */
    g_rescan.reapplyBrightness = TRUE;
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return;
    *fresh = g_monitors;
    AdoptMonitorList(fresh);
    CHECK(g_monitors.monitors[0].brightnessCur == 73 && g_monitors.monitors[1].brightnessCur == 213);
    CHECK(restoreRequests == 2 && setOneCalls == 0 && setAllCalls == 0);
}

static void TestDerivedRowAverageDoesNotBecomeOtherMonitorIntent(void)
{
    ResetState();
    ConfigureTwoReadyMonitors();
    g_monitors.monitors[1].brightnessCur = 80;
    Monitor_SetBrightness(&g_monitors.monitors[0], 20);
    SliderManualChange(0, 20);
    CHECK(g_masterTarget == 50 && g_masterTargetValid && !g_masterTargetExplicit);
    CHECK(!g_monitors.monitors[1].desiredBrightnessValid);
    g_settings.idleDimPercent = 5;
    Idle_Dim();
    Idle_Restore();
    CHECK(g_monitors.monitors[0].brightnessCur == 20 && g_monitors.monitors[1].brightnessCur == 80);
    g_monitors.monitors[0].brightnessCur = 100;
    ReapplyBrightness(); /* Reapply changed row0, leave untouched row1's confirmed80 alone. */
    CHECK(g_monitors.monitors[0].brightnessCur == 20 && g_monitors.monitors[1].brightnessCur == 80);
    CHECK(!g_monitors.monitors[1].desiredBrightnessValid);
    ResumeSourceMonitor(&g_monitors.monitors[1]);
    CHECK(g_monitors.monitors[1].brightnessCur == 80 && !g_monitors.monitors[1].desiredBrightnessValid);

    AppSetMaster(65); /* A deliberate group request still provides a fallback. */
    CHECK(g_masterTargetExplicit);
    g_monitors.monitors[1].desiredBrightnessValid = FALSE;
    g_monitors.monitors[1].brightnessCur = 100;
    ReapplyBrightness();
    CHECK(g_monitors.monitors[1].brightnessCur == 65 && g_monitors.monitors[1].desiredBrightnessValid);
}

static void TestGroupStepAfterIdleUsesPreDimAverage(void)
{
    ResetState();
    ConfigureTwoReadyMonitors();
    g_monitors.monitors[0].brightnessCur = 20;
    g_monitors.monitors[1].brightnessCur = 80;
    g_settings.idleDimPercent = 5;
    Idle_Dim();
    CHECK(g_masterTarget == 50 && !g_masterTargetValid && !g_masterTargetExplicit);
    StepMaster(5); /* The user's group step deliberately replaces individual levels. */
    CHECK(g_masterTarget == 55 && g_masterTargetExplicit && !g_idleDimmed);
    CHECK(g_monitors.monitors[0].brightnessCur == 55 && g_monitors.monitors[1].brightnessCur == 55);
}

static void TestIdleScopeChangeRestoresIndividualIntentOnRetainedRows(void)
{
    ResetState();
    ConfigureTwoReadyMonitors();
    Monitor_SetBrightness(&g_monitors.monitors[0], 20);
    Monitor_SetBrightness(&g_monitors.monitors[1], 80);
    SliderManualChange(0, 20);
    g_settings.idleDimPercent = 5;
    Idle_Dim();
    g_settings.monitorSelection.selectedOnly = TRUE;
    g_settings.monitorSelection.count = 1;
    CHECK(Settings_MonitorKey(&g_monitors.monitors[0], g_settings.monitorSelection.keys[0]));
    UpdateMonitorSelection();
    CHECK(!g_idleDimmed && g_monitors.monitors[0].brightnessCur == 20);
    CHECK(g_monitors.monitors[0].desiredBrightnessValid && g_monitors.monitors[0].desiredBrightness == 20);
    CHECK(g_monitors.monitors[1].excludedFromControl && g_monitors.monitors[1].brightnessCur == 5);
    CHECK(!g_masterTargetExplicit);
}

static void TestChangingDimToBlackRestoresOwnedPanelAndRetries(void)
{
    for (int fail = 0; fail < 2; fail++) {
        ResetState();
        AddReadyMonitor();
        BrightMonitor *monitor = &g_monitors.monitors[0];
        monitor->brightnessMax = 255;
        monitor->brightnessCur = 73;
        g_settings.idleDimEnabled = TRUE;
        g_settings.idleDimPercent = 5;
        Idle_Dim();
        CHECK(monitor->idleApplied && monitor->preIdleBrightness == 73);
        g_settings.monitorSelection.idleBlackCount = 1;
        CHECK(Settings_MonitorKey(monitor, g_settings.monitorSelection.idleBlackKeys[0]));
        mockNormalSucceeds = !fail;
        UpdateMonitorSelection();
        CHECK(monitor->idleBlack && !g_idleDimmed && !monitor->desiredBrightnessValid);
        CHECK(restoreRequests == 1);
        if (fail) {
            CHECK(monitor->idleApplied && monitor->preIdleBrightnessValid);
            CHECK(monitor->brightnessCur == 12);
            RetryIdleRestores();
            CHECK(restoreRequests == 2 && monitor->idleApplied);
            mockNormalSucceeds = TRUE;
            RetryIdleRestores();
        }
        CHECK(monitor->brightnessCur == 73 && !monitor->idleApplied && !monitor->preIdleBrightnessValid);
        int requests = restoreRequests;
        Idle_Dim();
        Idle_Restore();
        CHECK(restoreRequests == requests && idleWrites == 1 && monitor->brightnessCur == 73);
    }
}

static void TestLateLcdDimAcknowledgementIsRestoredUnderBlackOverlay(void)
{
    ResetState();
    ConfigureTwoReadyMonitors();
    BrightMonitor *monitor = &g_monitors.monitors[0];
    monitor->brightnessMax = 255;
    monitor->brightnessCur = 73;
    g_settings.idleDimEnabled = TRUE;
    g_settings.idleDimPercent = 5;
    mockWorkerRunning = TRUE;
    Idle_Dim();
    DWORD epoch = monitor->idleEpoch;
    g_settings.monitorSelection.idleBlackCount = 1;
    CHECK(Settings_MonitorKey(monitor, g_settings.monitorSelection.idleBlackKeys[0]));
    UpdateMonitorSelection();
    CHECK(monitor->idleBlack && !g_idleDimmed && !monitor->idleApplied);
    mockWorkerRunning = FALSE;
    Idle_Dim(); /* Black overlay now owns row0; row1 stays an ordinary dimmed LCD. */
    CHECK(g_idleDimmed && blackIdle && g_monitors.monitors[1].idleApplied);
    CHECK(monitor->idleEpoch == epoch); /* Black-only entry keeps the retained old LCD association. */
    acceptResult = FALSE;
    DeliverIdleWriteResult(0, epoch, 73);
    CHECK(monitor->idleApplied && monitor->preIdleBrightnessValid);
    mockNormalSucceeds = FALSE;
    RetryIdleRestores();
    CHECK(monitor->idleApplied && restoreRequests == 1 && g_idleDimmed && blackIdle);
    mockNormalSucceeds = TRUE;
    RetryIdleRestores();
    CHECK(monitor->brightnessCur == 73 && !monitor->idleApplied && restoreRequests == 2);
    CHECK(g_monitors.monitors[1].idleApplied && g_monitors.monitors[1].brightnessCur == 5);
    CHECK(g_idleDimmed && blackIdle); /* Cleaning old LCD ownership must not wake the other LCD. */
}

static void TestIdleWakeUsesLatestPendingIndividualTarget(void)
{
    ResetState();
    AddReadyMonitor();
    g_settings.idleDimPercent = 5;
    Monitor_SetBrightness(&g_monitors.monitors[0], 20);
    Idle_Dim();
    mockWorkerRunning = TRUE;
    Monitor_SetBrightness(&g_monitors.monitors[0], 80); /* New intent is queued after the old dim. */
    mockPendingMask = 1;
    mockPendingTarget = 80;
    Idle_Restore();
    CHECK(g_monitors.monitors[0].desiredBrightness == 80);
    CHECK(restoreRequests == 0 && g_monitors.monitors[0].idleApplied);
    int requests = setOneCalls;
    RetryIdleRestores();
    CHECK(setOneCalls == requests); /* Do not replace the queued normal restore. */
    mockPendingMask = 0;
    mockWorkerRunning = FALSE;
    RetryIdleRestores();
    CHECK(g_monitors.monitors[0].brightnessCur == 80 && !g_monitors.monitors[0].idleApplied);
}

static MonitorResult *MakeRawRestoreResult(BOOL success)
{
    MonitorResult *result = (MonitorResult *)calloc(1, sizeof(*result));
    CHECK(result != NULL);
    if (!result) return NULL;
    result->kind = MONITOR_RESULT_BRIGHTNESS;
    result->purpose = MONITOR_WRITE_IDLE_RESTORE;
    result->idleEpoch = g_monitors.monitors[0].idleEpoch;
    result->success = result->brightnessWritten = success;
    result->current = g_monitors.monitors[0].preIdleBrightness;
    result->maximum = g_monitors.monitors[0].brightnessMax;
    return result;
}

static void TestAsyncRawRestoreRetainsOwnershipAndRetriesFailure(void)
{
    ResetState();
    AddReadyMonitor();
    BrightMonitor *monitor = &g_monitors.monitors[0];
    monitor->brightnessMax = 255;
    monitor->brightnessCur = 73;
    g_settings.idleDimPercent = 5;
    Idle_Dim();
    mockWorkerRunning = TRUE;
    Idle_Restore();
    CHECK(monitor->idleApplied && monitor->idleReleasePending && monitor->preIdleBrightness == 73);
    CHECK(restoreRequests == 1 && !monitor->desiredBrightnessValid);
    RetryIdleRestores();
    CHECK(restoreRequests == 1); /* A raw pending value cannot be treated as percent intent. */
    HandleMonitorResult(testWindow, MakeRawRestoreResult(FALSE));
    CHECK(monitor->idleApplied && !monitor->idleReleasePending && monitor->preIdleBrightnessValid);
    CHECK(!g_rescan.reapplyBrightness && timerCalls == 0 && refreshCalls == 0);
    RetryIdleRestores();
    CHECK(restoreRequests == 2 && monitor->idleReleasePending);
    HandleMonitorResult(testWindow, MakeRawRestoreResult(TRUE));
    CHECK(monitor->brightnessCur == 73 && !monitor->idleApplied && !monitor->idleReleasePending);
    CHECK(!monitor->preIdleBrightnessValid && !monitor->desiredBrightnessValid);
    RetryIdleRestores();
    CHECK(restoreRequests == 2);
}

static void TestWakeCancelsBaselineLessDimAndRestoresLateAcknowledgement(void)
{
    ResetState();
    AddReadyMonitor();
    BrightMonitor *monitor = &g_monitors.monitors[0];
    monitor->brightnessMax = 255;
    monitor->brightnessCur = 73;
    g_settings.idleDimPercent = 5;
    mockWorkerRunning = TRUE;
    Idle_Dim();
    DWORD epoch = monitor->idleEpoch;
    CHECK(monitor->idleDimPending && !monitor->preIdleBrightnessValid);
    Idle_Restore();
    CHECK(cancelCalls == 1 && !monitor->idleDimPending && restoreRequests == 0 && setOneCalls == 0);
    acceptResult = FALSE; /* Native I/O completed just before cancellation. */
    DeliverIdleWriteResult(0, epoch, 73);
    CHECK(monitor->idleApplied && monitor->preIdleBrightnessValid && monitor->preIdleBrightness == 73);
    mockWorkerRunning = FALSE;
    RetryIdleRestores();
    CHECK(monitor->brightnessCur == 73 && !monitor->idleApplied && !monitor->desiredBrightnessValid);
}

static void TestRescanKeepsPendingRawRestoreNativeAndLateTelemetryStale(void)
{
    ResetState();
    AddReadyMonitor();
    BrightMonitor *monitor = &g_monitors.monitors[0];
    monitor->brightnessMax = 255;
    monitor->brightnessCur = 73;
    g_settings.idleDimPercent = 5;
    Idle_Dim();
    mockWorkerRunning = TRUE;
    Idle_Restore();
    CHECK(restoreRequests == 1 && monitor->idleReleasePending && !monitor->desiredBrightnessValid);
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return;
    *fresh = g_monitors;
    fresh->monitors[0].brightnessCur = 255;
    fresh->monitors[0].idleReleasePending = FALSE; /* Enumeration carries no canceled worker queue. */
    g_rescan.reapplyBrightness = TRUE;
    AdoptMonitorList(fresh);
    monitor = &g_monitors.monitors[0];
    CHECK(restoreRequests == 2 && monitor->idleReleasePending);
    CHECK(monitor->preIdleBrightness == 73 && !monitor->desiredBrightnessValid && setOneCalls == 0);

    acceptResult = FALSE; /* An earlier success can release ownership, never overwrite newer telemetry. */
    MonitorResult *result = MakeRawRestoreResult(TRUE);
    if (!result) return;
    result->sourceUpdated = result->sourceKnown = TRUE;
    result->currentInput = 0x12;
    monitor->sourceKnown = TRUE;
    monitor->currentInput = 0x0f;
    HandleMonitorResult(testWindow, result);
    CHECK(!monitor->idleApplied && !monitor->preIdleBrightnessValid);
    CHECK(monitor->sourceKnown && monitor->currentInput == 0x0f && monitor->brightnessCur == 255);
    CHECK(!monitor->desiredBrightnessValid);
}

static void TestMixedOledWakeRetriesOnlyUnrestoredMonitor(void)
{
    ResetState();
    ConfigureTwoFilteredMonitors();
    BrightMonitor *oled = &g_monitors.monitors[0];
    BrightMonitor *lcd = &g_monitors.monitors[1];
    oled->idleBlack = TRUE;
    lcd->brightnessMax = 100;
    lcd->brightnessCur = 60;
    g_masterTarget = 60;
    g_masterTargetValid = TRUE;
    Idle_Dim();
    CHECK(blackIdle && oled->brightnessCur == 73 && lcd->brightnessCur == 5);
    CHECK(lcd->idleApplied && lcd->preIdleBrightness == 60);

    mockNormalSucceeds = FALSE;
    Idle_Restore();
    CHECK(!blackIdle && !g_idleDimmed && lcd->brightnessCur == 5);
    CHECK(lcd->idleApplied && lcd->preIdleBrightnessValid);
    int before = restoreRequests;
    RetryIdleRestores();
    CHECK(restoreRequests == before + 1 && lcd->idleApplied); /* A failed retry keeps the restore intent. */

    before = restoreRequests;
    mockPendingMask = 1u << 1;
    RetryIdleRestores();
    CHECK(restoreRequests == before); /* Do not replace an in-flight/queued restore. */
    mockPendingMask = 0;
    lcd->excludedFromControl = TRUE;
    RetryIdleRestores();
    CHECK(restoreRequests == before);
    lcd->excludedFromControl = FALSE;
    lcd->sourceKnown = FALSE;
    RetryIdleRestores();
    CHECK(restoreRequests == before);
    lcd->sourceKnown = TRUE;
    lcd->currentInput = 0x12;
    RetryIdleRestores();
    CHECK(restoreRequests == before); /* Another input uses the guarded handoff path. */
    lcd->currentInput = 0x0F;
    g_idleDimmed = TRUE;
    RetryIdleRestores();
    CHECK(restoreRequests == before);
    g_idleDimmed = FALSE;

    lcd->desiredBrightness = 48; /* A newer request wins over the old 60% restore. */
    lcd->desiredBrightnessValid = TRUE;
    mockNormalSucceeds = TRUE;
    before = setOneCalls;
    RetryIdleRestores();
    CHECK(setOneCalls == before + 1 && lcd->brightnessCur == 48 && !lcd->idleApplied);
    CHECK(oled->brightnessCur == 73 && !blackIdle && !g_idleDimmed);
    RetryIdleRestores();
    CHECK(setOneCalls == before + 1); /* Stop once the hardware has recovered. */
}

static void TestConfiguredSourcePollingTimer(void)
{
    ResetState();
    RestartSourcePolling();
    CHECK(lastTimer == SOURCE_TIMER_ID && lastInterval == 3000);
    CHECK(sourceTelemetryInterval == 3000);
    for (int seconds = 1; seconds <= 60; seconds++) {
        g_settings.sourcePollSeconds = seconds;
        RestartSourcePolling();
        CHECK(lastTimer == SOURCE_TIMER_ID && lastInterval == (UINT)seconds * 1000u);
        CHECK(sourceTelemetryInterval == lastInterval);
    }
    g_settings.sourcePollSeconds = 0;
    RestartSourcePolling();
    CHECK(g_settings.sourcePollSeconds == 1 && lastInterval == 1000);
    g_settings.sourcePollSeconds = 100;
    RestartSourcePolling();
    CHECK(g_settings.sourcePollSeconds == 60 && lastInterval == 60000);
    CHECK(sourceRefreshCalls == 0 && actualWrites == 0); /* Saving the interval only re-arms its timer. */
}

static void ConfigureTwoReadyMonitors(void)
{
    AddReadyMonitor();
    g_monitors.count = 2;
    g_monitors.monitors[1] = MakeMonitor(TRUE, 70);
    wcscpy(g_monitors.monitors[0].name, L"First panel");
    wcscpy(g_monitors.monitors[1].name, L"Second panel");
    wcscpy(g_monitors.monitors[1].deviceInstance, L"TEST\\SECOND");
}

static void DeliverAvailability(BOOL firstReady, BOOL secondReady)
{
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return;
    *fresh = g_monitors;
    const BOOL ready[] = { firstReady, secondReady };
    for (int i = 0; i < fresh->count; i++) {
        if (ready[i] && !fresh->monitors[i].controllable) fresh->monitors[i].brightnessCur = 100;
        fresh->monitors[i].controllable = ready[i];
        fresh->monitors[i].awaitingAnswer = FALSE; /* Enumeration has not applied recovery policy. */
        fresh->monitors[i].desiredBrightnessValid = FALSE;
    }
    g_rescan.awaitedGeneration = ++g_rescan.generation;
    g_rescan.busy = 1;
    HandleRescanResult(testWindow, g_rescan.awaitedGeneration, fresh);
    CHECK(!g_rescan.busy && !g_rescan.awaitedGeneration);
}

static void TestPartialRecoveryUsesLatestCliMasterOnlyOnRecoveredPanel(void)
{
    ResetState();
    ConfigureTwoReadyMonitors();
    CHECK(AppSetMaster(60));
    int writes = actualWrites;
    DeliverAvailability(TRUE, FALSE);
    CHECK(!g_monitors.monitors[0].awaitingAnswer && g_monitors.monitors[1].awaitingAnswer);
    CHECK(actualWrites == writes); /* The panel still answering is left alone. */
    CHECK(lastTimer == RESCAN_RETRY_TIMER_ID && lastInterval == kRescanBackoffMs[0]);
    CHECK(g_awaitRetry == 1);
    CHECK(AppSetMaster(44));
    CHECK(g_masterTarget == 44 && g_monitors.monitors[0].brightnessCur == 44);
    writes = actualWrites;
    DeliverAvailability(TRUE, TRUE);
    CHECK(!g_monitors.monitors[1].awaitingAnswer && g_awaitRetry == 0);
    CHECK(g_monitors.monitors[0].brightnessCur == 44 && g_monitors.monitors[1].brightnessCur == 44);
    CHECK(actualWrites == writes + 1); /* Recovery targets only the second panel. */
}

static void TestTotalUnavailableBackoffAndPopupDeferral(void)
{
    ResetState();
    ConfigureTwoReadyMonitors();
    g_rescan.retry = RESCAN_MAX_RETRIES; /* The initial placeholder retry window has elapsed. */
    for (int i = 0; i < RESCAN_MAX_RETRIES + 2; i++) {
        DeliverAvailability(FALSE, FALSE);
        CHECK(g_monitors.monitors[0].awaitingAnswer && g_monitors.monitors[1].awaitingAnswer);
        CHECK(actualWrites == 0 && lastTimer == RESCAN_RETRY_TIMER_ID);
        CHECK(lastInterval == (i < RESCAN_MAX_RETRIES ? kRescanBackoffMs[i] : RESCAN_AWAIT_STEADY_MS));
    }
    CHECK(g_awaitRetry == RESCAN_MAX_RETRIES);
    popupVisible = TRUE;
    DWORD generation = g_rescan.generation;
    HandleTimer(testWindow, RESCAN_RETRY_TIMER_ID);
    CHECK(lastInterval == RESCAN_AWAIT_DEFER_MS && g_rescan.generation == generation);
    CHECK(g_awaitRetry == RESCAN_MAX_RETRIES && !g_rescan.busy);
    popupVisible = FALSE;
    DeliverAvailability(TRUE, TRUE);
    CHECK(g_awaitRetry == 0 && !g_monitors.monitors[0].awaitingAnswer &&
          !g_monitors.monitors[1].awaitingAnswer);
    CHECK(actualWrites == 0); /* Manual launch never invented a brightness target. */
}

static void TestValidExcludedTopologyDoesNotTriggerPlaceholderRetries(void)
{
    ResetState();
    ConfigureOledSelection();
    ApplyStartupBrightness(TRUE);
    int writes = actualWrites, timers = timerCalls;
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(*fresh));
    CHECK(fresh != NULL);
    if (!fresh) return;
    fresh->count = 1;
    fresh->monitors[0] = g_monitors.monitors[1]; /* Only the excluded LCD is still connected. */
    fresh->monitors[0].brightnessCur = 75;
    g_rescan.awaitedGeneration = ++g_rescan.generation;
    HandleRescanResult(testWindow, g_rescan.awaitedGeneration, fresh);
    CHECK(g_monitors.count == 1 && g_monitors.monitors[0].excludedFromControl);
    CHECK(g_monitors.monitors[0].brightnessCur == 75 && !Monitor_HasSelected(&g_monitors));
    CHECK(g_rescan.retry == 0 && g_awaitRetry == 0 && workerResets == 1);
    CHECK(timerCalls == timers && actualWrites == writes);
    CHECK(g_rescan.reapplyBrightness); /* Retain intent for the selected panel's return. */
}

static void TestCliActivityRestartsIdleCountdownAndLatestRowIntent(void)
{
    ResetState();
    AddReadyMonitor();
    g_settings.idleDimEnabled = TRUE;
    g_settings.idleDimMinutes = 1;
    g_settings.idleDimPercent = 5;
    nowTick = 300000;
    lastInputTick = 0;
    Idle_Tick();
    CHECK(g_idleDimmed && g_monitors.monitors[0].brightnessCur == 5);
    CHECK(AppSetMaster(60));
    CHECK(!g_idleDimmed && g_monitors.monitors[0].brightnessCur == 60);
    CHECK(g_remoteSeen && g_lastRemoteTick == nowTick && IdleMilliseconds() == 0);
    nowTick += 59000;
    Idle_Tick();
    CHECK(!g_idleDimmed && g_monitors.monitors[0].brightnessCur == 60);
    nowTick += 1000;
    Idle_Tick();
    CHECK(g_idleDimmed && g_monitors.monitors[0].brightnessCur == 5);
    CHECK(AppSetMonitor(0, 38));
    CHECK(!g_idleDimmed && g_masterTarget == 38 && g_monitors.monitors[0].brightnessCur == 38);
    CHECK(IdleMilliseconds() == 0);

    MonitorInputRule *rule = &g_settings.monitorSelection.inputRules[0];
    g_settings.monitorSelection.inputRuleCount = 1;
    CHECK(Settings_MonitorKey(&g_monitors.monitors[0], rule->key));
    rule->enabled = TRUE;
    rule->input = 15;
    Settings_ApplyMonitorSelection(&g_settings, &g_monitors);
    g_monitors.monitors[0].sourceKnown = TRUE;
    g_monitors.monitors[0].currentInput = 18;
    nowTick += 10000;
    int writes = actualWrites;
    (void)AppSetMonitor(0, 27); /* A suspended row retains this intent for its return. */
    CHECK(actualWrites == writes && g_monitors.monitors[0].desiredBrightness == 27);
    CHECK(g_lastRemoteTick == nowTick);
    DeliverRescan(1, TRUE, 100);
    CHECK(!g_monitors.monitors[0].sourceKnown && actualWrites == writes);
    DeliverSourceResult(MONITOR_RESULT_SOURCE, TRUE, 15);
    CHECK(g_monitors.monitors[0].brightnessCur == 27 && actualWrites == writes + 1);
}

static void TestCliRangeAndRescanPreserveLatestMaster(void)
{
    ResetState();
    mockRangeLo = 20;
    mockRangeHi = 80;
    ConfigureTwoReadyMonitors();
    CHECK(AppSetMaster(40));
    CHECK(g_monitors.monitors[0].brightnessCur == 44 && AppMasterLevel() == 40);
    mockRangeLo = 10;
    mockRangeHi = 90;
    for (int i = 0; i < g_monitors.count; i++) {
        g_monitors.monitors[i].rangeLo = mockRangeLo;
        g_monitors.monitors[i].rangeHi = mockRangeHi;
    }
    RangeChanged(40);
    CHECK(g_masterTarget == 40 && g_monitors.monitors[0].brightnessCur == 42);
    DeliverAvailability(TRUE, FALSE);
    CHECK(AppSetMaster(55));
    int writes = actualWrites;
    DeliverAvailability(TRUE, TRUE);
    CHECK(g_masterTarget == 55 && AppMasterLevel() == 55);
    CHECK(g_monitors.monitors[0].brightnessCur == 54 && g_monitors.monitors[1].brightnessCur == 54);
    CHECK(actualWrites == writes + 1);
}

int main(void)
{
    (void)UnusedApplicationEntryPoint; /* Compile the entry point; never run it. */
    TestCommandLine();
    TestManualStartup();
    TestLoginUntilNextAnchor();
    TestManualOverrideBeforeRescan();
    TestLoginExactlyAtAnchor();
    TestIdleTakesPrecedenceOnRescan();
    TestSingleAnchorWaitsUntilTomorrow();
    TestResumeAfterTwoDaySleep();
    TestUnavailableThenLateReady();
    TestWriteFailureRetriesLatestIntent();
    TestAllPoliciesRespectGlobalSelection();
    TestScopeChangeAndDisconnectedSelection();
    TestHotkeysAndFeedbackUseSelectedMonitor();
    TestScopeChangeWhileDimmedKeepsNewDisplayLevel();
    TestSourceSuspensionAndResume();
    TestIdleSourceHandoffWhilePcStaysIdle();
    TestIdleSourceHandoffNeedsKnownSourceAndOwnership();
    TestIdleSkippedWriteReleasesAlreadyDimmedMonitor();
    TestSupersededIdleSuccessStillRestoresHardware();
    TestFailedIdleReleaseRetainsBaselineUntilNextPoll();
    TestWakePreservesPendingAlternateInputRestore();
    TestReturningIdleInputSupersedesQueuedRelease();
    TestSourceRuleEditKeepsPendingIdleWritePurpose();
    TestReorderedRescanReconcilesAppliedIdleByIdentity();
    TestCrossResetIdleAcknowledgementRejectsChangedIdentityOrPolicy();
    TestSourcePollingAndRuleChanges();
    TestConfiguredSourcePollingTimer();
    TestPollingAndPolicyAfterBrightnessRecovery();
    TestIdleStateSurvivesUnavailableBrightnessOnRescan();
    TestSourceResumeAfterRescanAndFilterRemoval();
    TestSuspendedIntentSurvivesScopeChange();
    TestOledBlackIdleKeepsPanelBrightness();
    TestPartialRecoveryUsesLatestCliMasterOnlyOnRecoveredPanel();
    TestTotalUnavailableBackoffAndPopupDeferral();
    TestValidExcludedTopologyDoesNotTriggerPlaceholderRetries();
    TestCliActivityRestartsIdleCountdownAndLatestRowIntent();
    TestCliRangeAndRescanPreserveLatestMaster();
    TestMixedOledWakeRetriesOnlyUnrestoredMonitor();
    TestIdleWakePreservesIndividualTargetsAndSchedulePrecedence();
    TestIdleWakeRestoresNativeBaselinesWithoutCreatingIntent();
    TestIdleWakeUsesLatestPendingIndividualTarget();
    TestAsyncRawRestoreRetainsOwnershipAndRetriesFailure();
    TestWakeCancelsBaselineLessDimAndRestoresLateAcknowledgement();
    TestRescanKeepsPendingRawRestoreNativeAndLateTelemetryStale();
    TestDerivedRowAverageDoesNotBecomeOtherMonitorIntent();
    TestGroupStepAfterIdleUsesPreDimAverage();
    TestIdleScopeChangeRestoresIndividualIntentOnRetainedRows();
    TestChangingDimToBlackRestoresOwnedPanelAndRetries();
    TestLateLcdDimAcknowledgementIsRestoredUnderBlackOverlay();
    if (failures) {
        printf("startup: %d failure(s)\n", failures);
        return 1;
    }
    puts("startup: all tests passed (no windows, hardware or persistent I/O)");
    return 0;
}
