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
static int nowMinute, nowDay, dayValue, dayCalls, mockDelta;
static int setAllCalls, setOneCalls, actualWrites, lastBase, lastUiTarget;
static int refreshCalls, popupCalls, destroyCalls, workerResets, cleanupCalls;
static int timerCalls, killCalls;
static BOOL acceptResult;
static int acceptCalls;
static UINT_PTR lastTimer;
static UINT lastInterval;
static UINT_PTR lastKilledTimer;

static void WINAPI MockGetLocalTime(LPSYSTEMTIME time);
static DWORD WINAPI MockGetTickCount(void);
static UINT_PTR WINAPI MockSetTimer(HWND, UINT_PTR, UINT, TIMERPROC);
static BOOL WINAPI MockKillTimer(HWND, UINT_PTR);
static BOOL WINAPI MockDestroyWindow(HWND);

#define GetLocalTime MockGetLocalTime
#define GetTickCount MockGetTickCount
#define SetTimer MockSetTimer
#define KillTimer MockKillTimer
#define DestroyWindow MockDestroyWindow
#define WinMain static UnusedApplicationEntryPoint
#include "../lumos.c"
#undef GetLocalTime
#undef GetTickCount
#undef SetTimer
#undef KillTimer
#undef DestroyWindow
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

static DWORD WINAPI MockGetTickCount(void) { return 60000; }

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

void Settings_LoadDeltas(Settings *settings, MonitorList *view)
{
    CHECK(settings == &g_settings && view == &g_monitors);
    for (int i = 0; i < view->count; i++) view->monitors[i].delta = mockDelta;
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
    CHECK(monitor->controllable);
    if (!monitor->controllable) return FALSE;
    monitor->brightnessCur = Brightness_ToRaw(monitor, percent);
    actualWrites++;
    return TRUE;
}

void Monitor_SetAllBrightness(MonitorList *view, int base)
{
    CHECK(view == &g_monitors);
    setAllCalls++;
    lastBase = base;
    for (int i = 0; i < view->count; i++) {
        BrightMonitor *monitor = &view->monitors[i];
        if (!monitor->controllable) continue;
        int percent = base + monitor->delta;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        Monitor_SetBrightness(monitor, (DWORD)percent);
    }
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

void MonitorWorker_Reset(void) { workerResets++; }
BOOL MonitorWorker_Accept(const MonitorResult *result)
{
    CHECK(result != NULL);
    acceptCalls++;
    return acceptResult;
}
DWORD MonitorWorker_PendingTargets(MonitorTarget targets[MAX_MONITORS])
{
    memset(targets, 0, sizeof(MonitorTarget) * MAX_MONITORS);
    return 0;
}

void UI_SetMasterTarget(int target) { lastUiTarget = target; }
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
    monitor.brightnessMax = 100;
    monitor.brightnessCur = (DWORD)percent;
    monitor.delta = mockDelta;
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
    g_idleDimmed = FALSE;
    nowMinute = 570; /* 09:30 */
    nowDay = 1;
    dayValue = 82;
    mockDelta = 10;
    dayCalls = setAllCalls = setOneCalls = actualWrites = 0;
    lastBase = lastUiTarget = -999;
    refreshCalls = popupCalls = destroyCalls = workerResets = cleanupCalls = 0;
    timerCalls = killCalls = 0;
    acceptResult = TRUE;
    acceptCalls = 0;
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
    CHECK(actualWrites == 1 && g_monitors.monitors[0].brightnessCur == 92);
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
    CHECK(g_monitors.monitors[0].brightnessCur == 30);
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
    CHECK(g_monitors.monitors[0].brightnessCur == 35 && dayCalls == 1);
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
    CHECK(lastBase == 12 && lastUiTarget == 12 && g_masterTarget == 82);
    CHECK(g_monitors.monitors[0].brightnessCur == 22 && dayCalls == 1);
    Schedule_ApplyNow();
    CHECK(lastBase == 12 && g_scheduleLastApplied == -1);
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
    CHECK(g_monitors.monitors[0].brightnessCur == 45 && dayCalls == 1);
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
    CHECK(!g_rescan.reapplyBrightness && g_monitors.monitors[0].brightnessCur == 92);
    int oldTimers = timerCalls, oldRefreshes = refreshCalls;
    DeliverFailedWrite(TRUE);
    CHECK(acceptCalls == 1 && g_rescan.reapplyBrightness);
    CHECK(refreshCalls == oldRefreshes + 1 && timerCalls == oldTimers + 1);
    CHECK(lastTimer == RESCAN_TIMER_ID && lastInterval == RESCAN_DEBOUNCE_MS);

    Monitor_SetAllBrightness(&g_monitors, 25);
    SliderManualChange(-1, 25);
    DeliverRescan(1, TRUE, 100);
    CHECK(!g_rescan.reapplyBrightness && lastBase == 25 && g_masterTarget == 25);
    CHECK(g_monitors.monitors[0].brightnessCur == 35 && dayCalls == 1);
    oldTimers = timerCalls;
    oldRefreshes = refreshCalls;
    DeliverFailedWrite(FALSE); /* Stale failure cannot schedule another recovery. */
    CHECK(acceptCalls == 2 && !g_rescan.reapplyBrightness);
    CHECK(timerCalls == oldTimers && refreshCalls == oldRefreshes && lastBase == 25);
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
    if (failures) {
        printf("startup: %d failure(s)\n", failures);
        return 1;
    }
    puts("startup: all tests passed (no windows, hardware or persistent I/O)");
    return 0;
}
