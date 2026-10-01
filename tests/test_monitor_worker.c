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
static MonitorResult *results[RESULT_CAPACITY];
static int resultCount, resultHead;
static volatile LONG writes, refreshes, retains, releases, leases;
static volatile LONG writeSuccess, refreshMask;
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

DWORD Monitor_RefreshBrightnessSync(MonitorList *view)
{
    InterlockedIncrement(&refreshes);
    SetEvent(refreshEntered);
    CHECK(WaitForSingleObject(allowRefresh, TEST_TIMEOUT) == WAIT_OBJECT_0);
    for (int i = 0; i < view->count; i++)
        Monitor_PreviewBrightness(&view->monitors[i], 73);
    return (DWORD)ReadCounter(&refreshMask);
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
    CHECK(resultReady && writeEntered && allowWrite && refreshEntered && allowRefresh);
    resultCount = resultHead = 0;
    writes = refreshes = retains = releases = leases = 0;
    writeSuccess = TRUE;
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
        CHECK(!MonitorWorker_Accept(first));
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

int main(void)
{
    TestSlowWriteCoalescing();
    TestResetLeasesAndGeneration();
    TestWritePriorityRoundRobin();
    TestRefreshStaleSequence();
    TestHardwareFailures();
    if (ReadCounter(&failures)) {
        printf("%ld monitor worker checks failed\n", ReadCounter(&failures));
        return 1;
    }
    puts("ALL PASS: asynchronous monitor worker (mocked hardware)");
    return 0;
}
