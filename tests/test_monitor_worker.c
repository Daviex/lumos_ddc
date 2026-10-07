/* Run the real worker with real Win32 threads/events and simulated monitors.
   Posted results are captured in memory; no monitor, window or registry is used. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../monitor_worker.h"

static volatile LONG failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        InterlockedIncrement(&failures); \
    } \
} while (0)

#define TEST_TIMEOUT 5000
#define RESULT_CAPACITY 16
static CRITICAL_SECTION resultLock;
static HANDLE resultReady, writeEntered, allowWrite, refreshEntered, allowRefresh;
static HANDLE sourceEntered, allowSource;
static MonitorResult *results[RESULT_CAPACITY];
static int resultCount, resultHead;
static volatile LONG writes, refreshes, retains, releases, leases;
static volatile LONG writeSuccess, refreshMask;
static volatile LONG sourceReads, sourceInput, sourceSuccess;
static volatile LONG brightnessReads, brightnessReadSuccess;
static DWORD writtenValues[RESULT_CAPACITY];

static LONG ReadCounter(volatile LONG *counter)
{
    return InterlockedCompareExchange(counter, 0, 0);
}

static BOOL WINAPI CapturePostMessageW(HWND owner, UINT message, WPARAM wparam,
                                      LPARAM lparam)
{
    BOOL accepted = FALSE;
    CHECK(owner == (HWND)(UINT_PTR)1);
    CHECK(message == WM_MONITOR_RESULT && wparam == 0);
    EnterCriticalSection(&resultLock);
    if (resultCount < RESULT_CAPACITY) {
        results[(resultHead + resultCount) % RESULT_CAPACITY] = (MonitorResult *)lparam;
        resultCount++;
        accepted = TRUE;
        SetEvent(resultReady);
    }
    LeaveCriticalSection(&resultLock);
    CHECK(accepted);
    return accepted;
}

void Monitor_Retain(const BrightMonitor *monitor)
{
    CHECK(monitor->hasHandle && monitor->backend == BACKEND_DDC);
    InterlockedIncrement(&retains);
    InterlockedIncrement(&leases);
}

void Monitor_Release(const BrightMonitor *monitor)
{
    CHECK(monitor->hasHandle && monitor->backend == BACKEND_DDC);
    InterlockedIncrement(&releases);
    CHECK(InterlockedDecrement(&leases) >= 0);
}

void Monitor_Cleanup(MonitorList *view)
{
    for (int i = 0; i < view->count; i++) Monitor_Release(&view->monitors[i]);
    view->count = 0;
}

void Monitor_FlushRetiredHandles(void) { }

void Monitor_PreviewBrightness(BrightMonitor *monitor, DWORD percent)
{
    monitor->brightnessMin = 0;
    monitor->brightnessMax = 100;
    monitor->brightnessCur = percent;
}

BOOL Monitor_SetBrightnessSync(BrightMonitor *monitor, DWORD percent)
{
    LONG index = InterlockedIncrement(&writes) - 1;
    CHECK(index < RESULT_CAPACITY);
    if (index < RESULT_CAPACITY) writtenValues[index] = percent;
    SetEvent(writeEntered);
    CHECK(WaitForSingleObject(allowWrite, TEST_TIMEOUT) == WAIT_OBJECT_0);
    if (!ReadCounter(&writeSuccess)) return FALSE;
    Monitor_PreviewBrightness(monitor, percent);
    return TRUE;
}

BOOL Monitor_ReadSourceSync(BrightMonitor *monitor)
{
    InterlockedIncrement(&sourceReads);
    SetEvent(sourceEntered);
    CHECK(WaitForSingleObject(allowSource, TEST_TIMEOUT) == WAIT_OBJECT_0);
    monitor->sourceKnown = ReadCounter(&sourceSuccess);
    monitor->currentInput = monitor->sourceKnown ? (DWORD)ReadCounter(&sourceInput) : 0;
    monitor->sourceCheckedTick = GetTickCount64();
    return monitor->sourceKnown;
}

MonitorWriteOutcome Monitor_SetBrightnessForPurposeGuardedSync(
    BrightMonitor *monitor, DWORD percent, MonitorWritePurpose purpose, DWORD otherInput,
    MonitorWriteGuard guard, void *context, BOOL *sourceUpdated)
{
    DWORD baseline = monitor->brightnessCur;
    *sourceUpdated = FALSE;
    if (monitor->sourceFilter) {
        Monitor_ReadSourceSync(monitor);
        *sourceUpdated = TRUE;
    }
    if (guard && !guard(context)) return MONITOR_WRITE_CANCELLED;
    if (purpose == MONITOR_WRITE_IDLE_RELEASE) {
        if (!monitor->sourceFilter || !monitor->sourceKnown ||
            !monitor->expectedInput || otherInput == monitor->expectedInput ||
            monitor->currentInput != otherInput)
            return MONITOR_WRITE_SKIPPED;
    } else if (monitor->sourceFilter && (!monitor->expectedInput || !monitor->sourceKnown ||
                                  monitor->currentInput != monitor->expectedInput))
        return MONITOR_WRITE_SKIPPED;
    if (!Monitor_SetBrightnessSync(monitor, percent)) return MONITOR_WRITE_FAILED;
    if (purpose == MONITOR_WRITE_IDLE && !monitor->preIdleBrightnessValid) {
        monitor->preIdleBrightnessValid = TRUE;
        monitor->preIdleBrightness = baseline;
    }
    return MONITOR_WRITE_APPLIED;
}

DWORD Monitor_RefreshBrightnessSync(MonitorList *view)
{
    InterlockedIncrement(&refreshes);
    SetEvent(refreshEntered);
    CHECK(WaitForSingleObject(allowRefresh, TEST_TIMEOUT) == WAIT_OBJECT_0);
    for (int i = 0; i < view->count; i++)
        Monitor_PreviewBrightness(&view->monitors[i], 73);
    return (DWORD)ReadCounter(&refreshMask);
}

BOOL Monitor_ReadBrightnessSync(BrightMonitor *monitor)
{
    InterlockedIncrement(&brightnessReads);
    if (!ReadCounter(&brightnessReadSuccess)) return FALSE;
    Monitor_PreviewBrightness(monitor, 73);
    monitor->controllable = TRUE;
    return TRUE;
}

#define PostMessageW CapturePostMessageW
#include "../monitor_worker.c"

static MonitorList MakeView(void)
{
    MonitorList view = { 0 };
    view.count = 1;
    view.monitors[0].hPhysical = (HANDLE)(UINT_PTR)1;
    view.monitors[0].hasHandle = TRUE;
    view.monitors[0].controllable = TRUE;
    view.monitors[0].backend = BACKEND_DDC;
    view.monitors[0].brightnessCur = 50;
    view.monitors[0].brightnessMax = 100;
    return view;
}

static void StartTest(MonitorList *view, BOOL blockWrites, BOOL blockRefresh)
{
    CHECK(!MonitorWorker_Running());
    InitializeCriticalSection(&resultLock);
    resultReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    writeEntered = CreateEventW(NULL, FALSE, FALSE, NULL);
    allowWrite = CreateEventW(NULL, TRUE, !blockWrites, NULL);
    refreshEntered = CreateEventW(NULL, FALSE, FALSE, NULL);
    allowRefresh = CreateEventW(NULL, TRUE, !blockRefresh, NULL);
    sourceEntered = CreateEventW(NULL, FALSE, FALSE, NULL);
    allowSource = CreateEventW(NULL, TRUE, TRUE, NULL);
    CHECK(resultReady && writeEntered && allowWrite && refreshEntered && allowRefresh);
    resultCount = resultHead = 0;
    writes = refreshes = retains = releases = leases = 0;
    writeSuccess = TRUE;
    sourceReads = 0;
    sourceInput = 0x0F;
    sourceSuccess = TRUE;
    brightnessReads = 0;
    brightnessReadSuccess = TRUE;
    refreshMask = 1;
    memset(writtenValues, 0, sizeof(writtenValues));
    CHECK(MonitorWorker_Start((HWND)(UINT_PTR)1, view));
    CHECK(MonitorWorker_Running());
}

static MonitorResult *WaitResult(void)
{
    MonitorResult *result = NULL;
    CHECK(WaitForSingleObject(resultReady, TEST_TIMEOUT) == WAIT_OBJECT_0);
    EnterCriticalSection(&resultLock);
    if (resultCount) {
        result = results[resultHead];
        resultHead = (resultHead + 1) % RESULT_CAPACITY;
        if (--resultCount == 0) ResetEvent(resultReady);
    }
    LeaveCriticalSection(&resultLock);
    CHECK(result != NULL);
    return result;
}

static void FinishTest(void)
{
    SetEvent(allowWrite);
    SetEvent(allowRefresh);
    SetEvent(allowSource);
    MonitorWorker_Stop();
    CHECK(!MonitorWorker_Running());
    CHECK(ReadCounter(&leases) == 0);
    CHECK(ReadCounter(&retains) == ReadCounter(&releases));
    CHECK(resultCount == 0);
    while (resultCount) {
        free(results[resultHead]);
        resultHead = (resultHead + 1) % RESULT_CAPACITY;
        resultCount--;
    }
    CloseHandle(resultReady);
    CloseHandle(writeEntered);
    CloseHandle(allowWrite);
    CloseHandle(refreshEntered);
    CloseHandle(allowRefresh);
    CloseHandle(sourceEntered);
    CloseHandle(allowSource);
    DeleteCriticalSection(&resultLock);
}

static void TestSlowWriteCoalescing(void)
{
    MonitorList view = MakeView();
    MonitorTarget pending[MAX_MONITORS];
    MonitorResult *first, *latest;
    ULONGLONG started;
    StartTest(&view, TRUE, FALSE);
    started = GetTickCount64();
    CHECK(MonitorWorker_Set(&view.monitors[0], 10));
    CHECK(GetTickCount64() - started < 250);
    CHECK(WaitForSingleObject(writeEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
    CHECK(MonitorWorker_PendingTargets(pending) == 1u);
    CHECK(pending[0].percent == 10);  /* An in-flight write is still outstanding. */
    started = GetTickCount64();
    CHECK(MonitorWorker_Set(&view.monitors[0], 20));
    CHECK(MonitorWorker_Set(&view.monitors[0], 30));
    CHECK(MonitorWorker_Set(&view.monitors[0], 45));
    CHECK(GetTickCount64() - started < 250);
    CHECK(ReadCounter(&writes) == 1);  /* Hardware is still held at the event. */
    CHECK(ReadCounter(&leases) == 2);  /* One in flight and one latest pending. */
    CHECK(MonitorWorker_PendingTargets(pending) == 1u);
    CHECK(pending[0].percent == 45);  /* Preserve the latest intent, not blocked 10. */
    CHECK(pending[0].monitor.hPhysical == view.monitors[0].hPhysical);
    CHECK(ReadCounter(&leases) == 2);  /* Identity snapshots do not own leases. */
    SetEvent(allowWrite);
    first = WaitResult();
    latest = WaitResult();
    if (first && latest) {
        CHECK(first->current == 10 && first->success);
        CHECK(first->brightnessWritten && first->purpose == MONITOR_WRITE_NORMAL);
        CHECK(!first->sourceUpdated);
        CHECK(!MonitorWorker_Accept(first));
        CHECK(MonitorWorker_AcceptState(first));
        CHECK(latest->current == 45 && latest->success);
        CHECK(MonitorWorker_Accept(latest));
        CHECK(ReadCounter(&writes) == 2);
        CHECK(writtenValues[0] == 10 && writtenValues[1] == 45);
        MonitorResult invalid = *latest;
        invalid.index = -1;
        CHECK(!MonitorWorker_Accept(&invalid));
        invalid.index = view.count;
        CHECK(!MonitorWorker_Accept(&invalid));
    }
    free(first);
    free(latest);
    CHECK(!MonitorWorker_Set(&(BrightMonitor){ 0 }, 80));
    FinishTest();
    CHECK(!MonitorWorker_Set(&view.monitors[0], 80));
    CHECK(MonitorWorker_PendingTargets(pending) == 0);
}

static void TestResetLeasesAndGeneration(void)
{
    MonitorList view = MakeView();
    MonitorTarget pending[MAX_MONITORS];
    MonitorResult *stale, *latest;
    StartTest(&view, TRUE, FALSE);
    CHECK(MonitorWorker_Set(&view.monitors[0], 10));
    CHECK(WaitForSingleObject(writeEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
    CHECK(MonitorWorker_Set(&view.monitors[0], 20));
    MonitorWorker_Refresh(&view);
    MonitorWorker_Refresh(&view);  /* Replacing a refresh must release its leases. */
    CHECK(ReadCounter(&leases) == 3);
    MonitorWorker_Reset();
    CHECK(ReadCounter(&leases) == 1);  /* Only the blocked call still owns a lease. */
    CHECK(MonitorWorker_PendingTargets(pending) == 0);
    CHECK(MonitorWorker_Set(&view.monitors[0], 30));
    CHECK(MonitorWorker_PendingTargets(pending) == 1u && pending[0].percent == 30);
    SetEvent(allowWrite);
    stale = WaitResult();
    latest = WaitResult();
    if (stale && latest) {
        CHECK(stale->generation != latest->generation);
        CHECK(!MonitorWorker_Accept(stale));
        CHECK(!MonitorWorker_AcceptState(stale));
        CHECK(MonitorWorker_Accept(latest) && latest->current == 30);
        CHECK(ReadCounter(&writes) == 2 && writtenValues[1] == 30);
        CHECK(ReadCounter(&refreshes) == 0);
    }
    free(stale);
    free(latest);
    FinishTest();
}

static void TestWritePriorityRoundRobin(void)
{
    MonitorList view = MakeView();
    MonitorTarget pending[MAX_MONITORS];
    const int expectedIndices[] = { 0, 1, 2, 0 };
    const DWORD expectedValues[] = { 10, 21, 32, 20 };
    view.count = 3;
    for (int i = 1; i < view.count; i++) {
        view.monitors[i] = view.monitors[0];
        view.monitors[i].hPhysical = (HANDLE)(UINT_PTR)(i + 1);
    }

    StartTest(&view, TRUE, FALSE);
    InterlockedExchange(&refreshMask, 7);
    CHECK(MonitorWorker_Set(&view.monitors[0], 10));
    CHECK(WaitForSingleObject(writeEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
    CHECK(MonitorWorker_Set(&view.monitors[0], 20));
    CHECK(MonitorWorker_Set(&view.monitors[2], 32));
    CHECK(MonitorWorker_Set(&view.monitors[1], 21));
    MonitorWorker_Refresh(&view);

    CHECK(MonitorWorker_PendingTargets(pending) == 7u);
    CHECK(pending[0].percent == 20 && pending[1].percent == 21 && pending[2].percent == 32);
    CHECK(ReadCounter(&leases) == 7);  /* In-flight + three writes + three reads. */
    CHECK(ReadCounter(&writes) == 1 && ReadCounter(&refreshes) == 0);
    SetEvent(allowWrite);

    /* The repeated monitor zero waits for the other monitors' writes, and all
       writes finish before any refresh result is posted. */
    for (int i = 0; i < 4; i++) {
        MonitorResult *result = WaitResult();
        if (result) {
            CHECK(result->index == expectedIndices[i]);
            CHECK(result->current == expectedValues[i] && result->success);
            CHECK(MonitorWorker_Accept(result) == (i != 0));
        }
        free(result);
        CHECK(writtenValues[i] == expectedValues[i]);
    }
    for (int i = 0; i < view.count; i++) {
        MonitorResult *result = WaitResult();
        if (result) {
            CHECK(result->index == i && result->current == 73);
            CHECK(!result->brightnessWritten);
            CHECK(!result->sourceUpdated);
            CHECK(MonitorWorker_Accept(result));
        }
        free(result);
    }
    CHECK(ReadCounter(&writes) == 4 && ReadCounter(&refreshes) == 1);
    FinishTest();
}

static void TestRefreshStaleSequence(void)
{
    MonitorList view = MakeView();
    MonitorTarget pending[MAX_MONITORS];
    MonitorResult *stale, *latest, *fresh;
    StartTest(&view, FALSE, TRUE);
    MonitorWorker_Refresh(&view);
    CHECK(WaitForSingleObject(refreshEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
    CHECK(MonitorWorker_PendingTargets(pending) == 0);  /* A read is not user intent. */
    CHECK(MonitorWorker_Set(&view.monitors[0], 66));
    CHECK(ReadCounter(&leases) == 2);
    CHECK(MonitorWorker_PendingTargets(pending) == 1u && pending[0].percent == 66);
    SetEvent(allowRefresh);
    stale = WaitResult();
    latest = WaitResult();
    if (stale && latest) {
        CHECK(stale->current == 73 && stale->success);
        CHECK(!MonitorWorker_Accept(stale));
        CHECK(latest->current == 66 && MonitorWorker_Accept(latest));
    }
    free(stale);
    free(latest);
    MonitorWorker_Refresh(&view);
    fresh = WaitResult();
    if (fresh) CHECK(fresh->current == 73 && MonitorWorker_Accept(fresh));
    free(fresh);
    CHECK(ReadCounter(&refreshes) == 2);
    FinishTest();
}

static void TestHardwareFailures(void)
{
    MonitorList view = MakeView();
    MonitorResult *failed, *retry;
    StartTest(&view, FALSE, FALSE);
    InterlockedExchange(&writeSuccess, FALSE);
    CHECK(MonitorWorker_Set(&view.monitors[0], 40));
    failed = WaitResult();
    if (failed) CHECK(!failed->success && MonitorWorker_Accept(failed));
    free(failed);
    InterlockedExchange(&writeSuccess, TRUE);
    CHECK(MonitorWorker_Set(&view.monitors[0], 40));
    retry = WaitResult();
    if (retry) CHECK(retry->success && retry->current == 40 && MonitorWorker_Accept(retry));
    free(retry);
    CHECK(ReadCounter(&writes) == 2);
    InterlockedExchange(&refreshMask, 0);
    MonitorWorker_Refresh(&view);
    CHECK(WaitForSingleObject(refreshEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
    /* Joining the worker checks that the failed refresh posted no bogus value. */
    FinishTest();
}

static void TestSourcePollingAndWriteOutcomes(void)
{
    MonitorList view = MakeView();
    MonitorResult *result;
    view.monitors[0].sourceFilter = TRUE;
    view.monitors[0].expectedInput = 0x0F;
    view.monitors[0].excludedFromControl = TRUE;
    StartTest(&view, FALSE, FALSE);
    MonitorWorker_RefreshSources(&view, TRUE);
    result = WaitResult();
    if (result) {
        CHECK(result->kind == MONITOR_RESULT_SOURCE && result->sourceKnown && result->success);
        CHECK(result->sourceUpdated);
        CHECK(result->currentInput == 0x0F && result->current == 50);
        CHECK(MonitorWorker_Accept(result));
    }
    free(result);
    CHECK(ReadCounter(&sourceReads) == 1 && ReadCounter(&refreshes) == 0);
    CHECK(ReadCounter(&writes) == 0);  /* Source polling works even for deselected displays. */
    view.monitors[0].excludedFromControl = FALSE;

    InterlockedExchange(&sourceInput, 0x12);
    CHECK(MonitorWorker_Set(&view.monitors[0], 60));
    result = WaitResult();
    if (result) {
        CHECK(result->kind == MONITOR_RESULT_SKIPPED && !result->success);
        CHECK(result->sourceUpdated);
        CHECK(result->sourceKnown && result->currentInput == 0x12 && result->current == 50);
        CHECK(MonitorWorker_Accept(result));
    }
    free(result);
    CHECK(ReadCounter(&writes) == 0);

    InterlockedExchange(&sourceSuccess, FALSE);
    MonitorWorker_RefreshSources(&view, TRUE);
    result = WaitResult();
    if (result) {
        CHECK(result->kind == MONITOR_RESULT_SOURCE && !result->sourceKnown && !result->success);
        CHECK(result->sourceUpdated);
        CHECK(result->currentInput == 0 && MonitorWorker_Accept(result));
    }
    free(result);
    CHECK(ReadCounter(&refreshes) == 0);
    CHECK(MonitorWorker_Set(&view.monitors[0], 60));
    result = WaitResult();
    if (result) CHECK(result->kind == MONITOR_RESULT_SKIPPED && !result->sourceKnown);
    free(result);
    CHECK(ReadCounter(&writes) == 0);

    InterlockedExchange(&sourceSuccess, TRUE);
    InterlockedExchange(&sourceInput, 0x0F);
    CHECK(MonitorWorker_Set(&view.monitors[0], 60));
    result = WaitResult();
    if (result) {
        CHECK(result->kind == MONITOR_RESULT_BRIGHTNESS && result->success && result->current == 60);
        CHECK(result->sourceUpdated);
    }
    free(result);
    CHECK(ReadCounter(&writes) == 1 && ReadCounter(&sourceReads) == 5);
    view.monitors[0].sourceKnown = TRUE;
    view.monitors[0].currentInput = 0x12;
    view.monitors[0].sourceCheckedTick = GetTickCount64();
    MonitorWorker_Refresh(&view);
    result = WaitResult();
    if (result) {
        CHECK(result->kind == MONITOR_RESULT_BRIGHTNESS && result->success);
        CHECK(!result->sourceUpdated && result->currentInput == 0x12);
    }
    free(result);
    CHECK(ReadCounter(&sourceReads) == 5);  /* The cached source was never re-read. */
    FinishTest();
}

static void TestSourceReadCancellation(void)
{
    /* A source read can block while selection/settings or the target changes.
       Both generation reset and superseding writes must cancel before native I/O. */
    for (int reset = 0; reset < 2; reset++) {
        MonitorList view = MakeView();
        MonitorResult *stale, *latest;
        MonitorTarget pending[MAX_MONITORS];
        view.monitors[0].sourceFilter = TRUE;
        view.monitors[0].expectedInput = 0x0F;
        StartTest(&view, FALSE, FALSE);
        ResetEvent(allowSource);
        CHECK(MonitorWorker_Set(&view.monitors[0], 10));
        CHECK(WaitForSingleObject(sourceEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
        CHECK(ReadCounter(&writes) == 0);
        ULONGLONG started = GetTickCount64();
        if (reset) MonitorWorker_Reset();
        CHECK(MonitorWorker_Set(&view.monitors[0], 80));
        CHECK(GetTickCount64() - started < 250);  /* No request lock across source I/O. */
        CHECK(MonitorWorker_PendingTargets(pending) == 1u && pending[0].percent == 80);
        MonitorWorker_RefreshSources(&view, TRUE);
        MonitorWorker_RefreshSources(&view, TRUE);  /* Replacement releases its old leases. */
        CHECK(ReadCounter(&leases) == 3);
        SetEvent(allowSource);
        stale = WaitResult();
        latest = WaitResult();
        if (stale && latest) {
            CHECK(stale->kind == MONITOR_RESULT_CANCELLED && !stale->success);
            CHECK(stale->sourceUpdated);
            CHECK(!MonitorWorker_Accept(stale));
            CHECK(latest->kind == MONITOR_RESULT_BRIGHTNESS && latest->current == 80);
            CHECK(latest->success && MonitorWorker_Accept(latest));
            CHECK(ReadCounter(&writes) == 1 && writtenValues[0] == 80);
        }
        free(stale);
        free(latest);
        MonitorResult *source = WaitResult();
        if (source) CHECK(source->kind == MONITOR_RESULT_SOURCE && MonitorWorker_Accept(source));
        free(source);
        FinishTest();
    }
}

static void TestSourcePollingScope(void)
{
    MonitorList view = MakeView();
    view.count = 4;
    for (int i = 1; i < view.count; i++) {
        view.monitors[i] = view.monitors[0];
        view.monitors[i].hPhysical = (HANDLE)(UINT_PTR)(i + 1);
        view.monitors[i].sourceFilter = TRUE;
        view.monitors[i].expectedInput = 0x0F;
    }
    view.monitors[1].excludedFromControl = TRUE;
    view.monitors[2].sourceKnown = TRUE;
    view.monitors[2].currentInput = 0x12; /* A suspended selected display must still be polled. */
    view.monitors[3].controllable = FALSE;
    StartTest(&view, FALSE, FALSE);
    MonitorWorker_RefreshSources(&view, FALSE);
    MonitorResult *result = WaitResult();
    if (result) {
        CHECK(result->index == 2 && result->kind == MONITOR_RESULT_SOURCE);
        CHECK(result->sourceUpdated && result->sourceKnown && result->currentInput == 0x0F);
        CHECK(MonitorWorker_Accept(result));
    }
    free(result);
    result = WaitResult();
    if (result) {
        CHECK(result->index == 3 && result->kind == MONITOR_RESULT_SOURCE);
        CHECK(result->brightnessUpdated && !result->brightnessWritten && result->current == 73);
        CHECK(result->sourceKnown && MonitorWorker_Accept(result));
    }
    free(result);
    FinishTest();
    CHECK(ReadCounter(&sourceReads) == 2 && ReadCounter(&refreshes) == 0);
    CHECK(ReadCounter(&brightnessReads) == 1 && ReadCounter(&writes) == 0);
}

static void TestSourcePollingWithoutBrightnessCapability(void)
{
    MonitorList view = MakeView();
    view.monitors[0].controllable = FALSE;
    StartTest(&view, FALSE, FALSE);
    InterlockedExchange(&brightnessReadSuccess, FALSE);
    MonitorWorker_RefreshSources(&view, FALSE); /* No filter is needed to retry discovery. */
    MonitorResult *result = WaitResult();
    if (result) {
        CHECK(result->kind == MONITOR_RESULT_SOURCE && result->sourceKnown);
        CHECK(!result->brightnessUpdated && !result->brightnessWritten);
        CHECK(MonitorWorker_Accept(result));
    }
    free(result);
    InterlockedExchange(&brightnessReadSuccess, TRUE);
    MonitorWorker_RefreshSources(&view, FALSE);
    result = WaitResult();
    if (result) {
        CHECK(result->kind == MONITOR_RESULT_SOURCE && result->sourceKnown);
        CHECK(result->brightnessUpdated && !result->brightnessWritten && result->current == 73);
        CHECK(MonitorWorker_Accept(result));
    }
    free(result);
    FinishTest();
    CHECK(ReadCounter(&sourceReads) == 2 && ReadCounter(&brightnessReads) == 2);
    CHECK(ReadCounter(&writes) == 0);
}

static void TestIdleReleasePurposeAndTelemetry(void)
{
    MonitorList view = MakeView();
    MonitorTarget pending[MAX_MONITORS];
    MonitorResult *result;
    view.monitors[0].sourceFilter = TRUE;
    view.monitors[0].expectedInput = 0x0F;
    view.monitors[0].idleEpoch = 17;
    StartTest(&view, FALSE, FALSE);
    CHECK(MonitorWorker_SetIdle(&view.monitors[0], 10));
    result = WaitResult();
    if (result) {
        CHECK(result->purpose == MONITOR_WRITE_IDLE && result->idleEpoch == 17);
        CHECK(result->brightnessWritten && result->success && result->current == 10);
        CHECK(result->preIdleBrightnessValid && result->preIdleBrightness == 50);
        CHECK(MonitorWorker_Accept(result) && MonitorWorker_AcceptState(result));
    }
    free(result);

    ResetEvent(allowSource);
    InterlockedExchange(&sourceInput, 0x12);
    CHECK(MonitorWorker_ReleaseIdle(&view.monitors[0], 50, 0x12));
    CHECK(WaitForSingleObject(sourceEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
    CHECK(MonitorWorker_PendingTargets(pending) == 0); /* Release is never desired PC intent. */
    SetEvent(allowSource);
    result = WaitResult();
    if (result) {
        CHECK(result->purpose == MONITOR_WRITE_IDLE_RELEASE && result->idleEpoch == 17);
        CHECK(result->brightnessWritten && result->success && result->current == 50);
        CHECK(result->sourceUpdated && result->sourceKnown && result->currentInput == 0x12);
    }
    free(result);
    CHECK(ReadCounter(&writes) == 2 && writtenValues[0] == 10 && writtenValues[1] == 50);
    FinishTest();
}

static void TestRawIdleRestoreIsNotPercentIntentAndCanBeSuperseded(void)
{
    MonitorList view = MakeView();
    MonitorTarget pending[MAX_MONITORS];
    view.monitors[0].brightnessMax = 255;
    view.monitors[0].sourceFilter = TRUE;
    view.monitors[0].expectedInput = 0x0F;
    view.monitors[0].idleEpoch = 19;
    view.monitors[0].idleApplied = view.monitors[0].preIdleBrightnessValid = TRUE;
    view.monitors[0].preIdleBrightness = 73;
    StartTest(&view, FALSE, FALSE);
    ResetEvent(allowSource);
    CHECK(MonitorWorker_RestoreIdle(&view.monitors[0], 73));
    CHECK(WaitForSingleObject(sourceEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
    CHECK(MonitorWorker_PendingTargets(pending) == 0);
    CHECK(MonitorWorker_Set(&view.monitors[0], 82)); /* A newer user request replaces raw restore. */
    CHECK(MonitorWorker_PendingTargets(pending) == 1u && pending[0].percent == 82);
    SetEvent(allowSource);
    MonitorResult *result = WaitResult();
    if (result) {
        CHECK(result->purpose == MONITOR_WRITE_IDLE_RESTORE && result->idleEpoch == 19);
        CHECK(result->kind == MONITOR_RESULT_CANCELLED && !result->brightnessWritten);
        CHECK(!MonitorWorker_Accept(result) && MonitorWorker_AcceptState(result));
    }
    free(result);
    result = WaitResult();
    if (result) {
        CHECK(result->purpose == MONITOR_WRITE_NORMAL && result->success && result->brightnessWritten);
        CHECK(result->current == 82 && MonitorWorker_Accept(result));
    }
    free(result);
    CHECK(ReadCounter(&writes) == 1 && writtenValues[0] == 82);
    FinishTest();
}

static void TestIdleReleaseRechecksSourceAndCancelsPerMonitor(void)
{
    /* Release must never brighten an input that changed while its check was blocked.
       Cancelling one display does not cancel work queued for another display. */
    for (int scenario = 0; scenario < 4; scenario++) {
        MonitorList view = MakeView();
        MonitorResult *result;
        view.count = 2;
        view.monitors[1] = view.monitors[0];
        view.monitors[1].hPhysical = (HANDLE)(UINT_PTR)2;
        view.monitors[0].sourceFilter = TRUE;
        view.monitors[0].expectedInput = 0x0F;
        view.monitors[0].idleEpoch = 23;
        StartTest(&view, FALSE, FALSE);
        ResetEvent(allowSource);
        InterlockedExchange(&sourceInput, 0x12);
        CHECK(MonitorWorker_ReleaseIdle(&view.monitors[0], 80, 0x12));
        CHECK(WaitForSingleObject(sourceEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
        CHECK(ReadCounter(&writes) == 0);
        CHECK(MonitorWorker_Set(&view.monitors[1], 40));
        if (scenario == 0) InterlockedExchange(&sourceInput, 0x0F);
        if (scenario == 1) InterlockedExchange(&sourceInput, 0x11);
        if (scenario == 2) InterlockedExchange(&sourceSuccess, FALSE);
        if (scenario == 3) MonitorWorker_Cancel(&view.monitors[0]);
        SetEvent(allowSource);
        result = WaitResult();
        if (result) {
            CHECK(result->purpose == MONITOR_WRITE_IDLE_RELEASE && result->idleEpoch == 23);
            CHECK(result->kind == (scenario == 3 ? MONITOR_RESULT_CANCELLED : MONITOR_RESULT_SKIPPED));
            CHECK(!result->brightnessWritten && !result->success && result->sourceUpdated);
            CHECK(MonitorWorker_Accept(result) == (scenario != 3));
            CHECK(MonitorWorker_AcceptState(result));
        }
        free(result);
        result = WaitResult();
        if (result) CHECK(result->index == 1 && result->success && result->current == 40 &&
                          result->brightnessWritten && MonitorWorker_Accept(result));
        free(result);
        CHECK(ReadCounter(&writes) == 1 && writtenValues[0] == 40);
        FinishTest();
    }
}

static void TestAppliedIdleIdentitySurvivesReset(void)
{
    for (int purpose = 0; purpose < 3; purpose++) {
        MonitorList view = MakeView();
        MonitorResult *result;
        view.monitors[0].sourceFilter = TRUE;
        view.monitors[0].expectedInput = 0x0F;
        view.monitors[0].idleEpoch = 31;
        wcscpy(view.monitors[0].deviceInstance, L"DISPLAY\\MOCK\\ORIGINAL");
        StartTest(&view, TRUE, FALSE);
        if (purpose == 1) {
            InterlockedExchange(&sourceInput, 0x12);
            CHECK(MonitorWorker_ReleaseIdle(&view.monitors[0], 80, 0x12));
        } else if (purpose == 2) {
            view.monitors[0].idleApplied = view.monitors[0].preIdleBrightnessValid = TRUE;
            view.monitors[0].preIdleBrightness = 80;
            CHECK(MonitorWorker_RestoreIdle(&view.monitors[0], 80));
        } else {
            CHECK(MonitorWorker_SetIdle(&view.monitors[0], 10));
        }
        /* The final guard has passed and native I/O cannot be retracted. Reset
           may replace index zero before its success is delivered to the app. */
        CHECK(WaitForSingleObject(writeEntered, TEST_TIMEOUT) == WAIT_OBJECT_0);
        MonitorWorker_Reset();
        wcscpy(view.monitors[0].deviceInstance, L"DISPLAY\\MOCK\\REPLACEMENT");
        view.monitors[0].backend = BACKEND_WMI;
        view.monitors[0].sourceFilter = FALSE;
        view.monitors[0].expectedInput = 0x11;
        view.monitors[0].idleEpoch = 32;
        SetEvent(allowWrite);
        result = WaitResult();
        if (result) {
            CHECK(result->success && result->brightnessWritten);
            CHECK(result->purpose == (purpose == 1 ? MONITOR_WRITE_IDLE_RELEASE :
                                     purpose == 2 ? MONITOR_WRITE_IDLE_RESTORE : MONITOR_WRITE_IDLE));
            CHECK(result->idleEpoch == 31);
            CHECK(wcscmp(result->deviceInstance, L"DISPLAY\\MOCK\\ORIGINAL") == 0);
            CHECK(result->backend == BACKEND_DDC && result->sourceFilter);
            CHECK(result->expectedInput == 0x0F);
            CHECK(!MonitorWorker_Accept(result) && !MonitorWorker_AcceptState(result));
        }
        free(result);
        CHECK(ReadCounter(&writes) == 1);
        FinishTest();
    }
}

int main(void)
{
    TestSlowWriteCoalescing();
    TestResetLeasesAndGeneration();
    TestWritePriorityRoundRobin();
    TestRefreshStaleSequence();
    TestHardwareFailures();
    TestSourcePollingAndWriteOutcomes();
    TestSourceReadCancellation();
    TestSourcePollingScope();
    TestSourcePollingWithoutBrightnessCapability();
    TestIdleReleasePurposeAndTelemetry();
    TestRawIdleRestoreIsNotPercentIntentAndCanBeSuperseded();
    TestIdleReleaseRechecksSourceAndCancelsPerMonitor();
    TestAppliedIdleIdentitySurvivesReset();
    if (ReadCounter(&failures)) {
        printf("%ld monitor worker checks failed\n", ReadCounter(&failures));
        return 1;
    }
    puts("ALL PASS: asynchronous monitor worker (mocked hardware)");
    return 0;
}
