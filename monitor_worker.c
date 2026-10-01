#include "monitor_worker.h"
#include <stdlib.h>

typedef struct {
    BrightMonitor monitor;
    DWORD percent, sequence, generation;
    BOOL pending;
} WriteRequest;

typedef struct {
    CRITICAL_SECTION lock;
    HANDLE event, thread;
    HWND owner;
    MonitorList *view;
    BOOL stop, refreshPending;
    DWORD generation;
    DWORD sequences[MAX_MONITORS];
    DWORD targets[MAX_MONITORS];
    BOOL inFlight[MAX_MONITORS];
    WriteRequest writes[MAX_MONITORS];
    MonitorList refresh;
    DWORD refreshSequences[MAX_MONITORS];
    DWORD refreshGeneration;
} Worker;

static Worker g_worker;
static volatile LONG g_running;
static SRWLOCK g_lifecycleLock = SRWLOCK_INIT;

static void PostResult(int index, DWORD generation, DWORD sequence,
                       const BrightMonitor *monitor, BOOL success)
{
    MonitorResult *result = (MonitorResult *)malloc(sizeof(*result));
    if (!result) return;
    result->index = index;
    result->generation = generation;
    result->sequence = sequence;
    result->success = success;
    result->minimum = monitor->brightnessMin;
    result->current = monitor->brightnessCur;
    result->maximum = monitor->brightnessMax;
    if (!PostMessageW(g_worker.owner, WM_MONITOR_RESULT, 0, (LPARAM)result))
        free(result);
}

static DWORD WINAPI WorkerProc(LPVOID unused)
{
    int nextIndex = 0;
    (void)unused;

    for (;;) {
        WriteRequest request;
        MonitorList refresh = { 0 };
        DWORD refreshSequences[MAX_MONITORS] = { 0 };
        DWORD refreshGeneration = 0;
        int index = -1;
        BOOL stop;

        EnterCriticalSection(&g_worker.lock);
        stop = g_worker.stop;
        if (!stop) {
            /* Round-robin prevents a continuously dragged monitor starving others. */
            for (int n = 0; n < MAX_MONITORS; n++) {
                int candidate = (nextIndex + n) % MAX_MONITORS;
                if (g_worker.writes[candidate].pending) {
                    index = candidate;
                    request = g_worker.writes[index];
                    g_worker.writes[index].pending = FALSE;
                    g_worker.inFlight[index] = TRUE;
                    nextIndex = (index + 1) % MAX_MONITORS;
                    break;
                }
            }
            if (index < 0 && g_worker.refreshPending) {
                refresh = g_worker.refresh;
                ZeroMemory(&g_worker.refresh, sizeof(g_worker.refresh));
                CopyMemory(refreshSequences, g_worker.refreshSequences,
                           sizeof(refreshSequences));
                refreshGeneration = g_worker.refreshGeneration;
                g_worker.refreshPending = FALSE;
            }
        }
        LeaveCriticalSection(&g_worker.lock);
        if (stop) break;

        if (index >= 0) {
            BOOL success = Monitor_SetBrightnessSync(&request.monitor, request.percent);
            PostResult(index, request.generation, request.sequence,
                       &request.monitor, success);
            Monitor_Release(&request.monitor);
            EnterCriticalSection(&g_worker.lock);
            g_worker.inFlight[index] = FALSE;
            LeaveCriticalSection(&g_worker.lock);
        } else if (refresh.count > 0) {
            DWORD readMask = Monitor_RefreshBrightnessSync(&refresh);
            for (int i = 0; i < refresh.count; i++) {
                if (readMask & (1u << i))
                    PostResult(i, refreshGeneration, refreshSequences[i],
                               &refresh.monitors[i], TRUE);
            }
            Monitor_Cleanup(&refresh);
        } else {
            Monitor_FlushRetiredHandles();
            WaitForSingleObject(g_worker.event, INFINITE);
        }
    }
    Monitor_FlushRetiredHandles();
    return 0;
}

BOOL MonitorWorker_Start(HWND owner, MonitorList *view)
{
    if (MonitorWorker_Running()) return FALSE;
    ZeroMemory(&g_worker, sizeof(g_worker));
    InitializeCriticalSection(&g_worker.lock);
    g_worker.owner = owner;
    g_worker.view = view;
    g_worker.generation = 1;
    g_worker.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (g_worker.event)
        g_worker.thread = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
    if (!g_worker.thread) {
        if (g_worker.event) CloseHandle(g_worker.event);
        DeleteCriticalSection(&g_worker.lock);
        return FALSE;
    }
    InterlockedExchange(&g_running, TRUE);
    return TRUE;
}

BOOL MonitorWorker_Running(void) { return InterlockedCompareExchange(&g_running, 0, 0) != 0; }

void MonitorWorker_Wake(void)
{
    /* Rescan threads may release leases during shutdown. Serialize that wake
       with event closure, without taking the request lock. */
    AcquireSRWLockShared(&g_lifecycleLock);
    if (MonitorWorker_Running()) SetEvent(g_worker.event);
    ReleaseSRWLockShared(&g_lifecycleLock);
}

BOOL MonitorWorker_Set(BrightMonitor *monitor, DWORD percent)
{
    int index = -1;
    if (!MonitorWorker_Running()) return FALSE;
    for (int i = 0; i < g_worker.view->count; i++)
        if (monitor == &g_worker.view->monitors[i]) { index = i; break; }
    if (index < 0) return FALSE;

    EnterCriticalSection(&g_worker.lock);
    WriteRequest *request = &g_worker.writes[index];
    if (request->pending) Monitor_Release(&request->monitor);
    request->monitor = *monitor;
    Monitor_Retain(&request->monitor);
    request->percent = percent;
    g_worker.targets[index] = percent;
    request->sequence = ++g_worker.sequences[index];
    request->generation = g_worker.generation;
    request->pending = TRUE;
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
    return TRUE;
}

void MonitorWorker_Refresh(const MonitorList *view)
{
    if (!MonitorWorker_Running()) return;
    EnterCriticalSection(&g_worker.lock);
    if (g_worker.refreshPending) Monitor_Cleanup(&g_worker.refresh);
    g_worker.refresh = *view;
    for (int i = 0; i < view->count; i++)
        Monitor_Retain(&g_worker.refresh.monitors[i]);
    CopyMemory(g_worker.refreshSequences, g_worker.sequences,
               sizeof(g_worker.sequences));
    g_worker.refreshGeneration = g_worker.generation;
    g_worker.refreshPending = TRUE;
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
}

void MonitorWorker_Reset(void)
{
    if (!MonitorWorker_Running()) return;
    EnterCriticalSection(&g_worker.lock);
    ++g_worker.generation;
    for (int i = 0; i < MAX_MONITORS; i++) {
        if (g_worker.writes[i].pending)
            Monitor_Release(&g_worker.writes[i].monitor);
        g_worker.writes[i].pending = FALSE;
        g_worker.inFlight[i] = FALSE;
    }
    if (g_worker.refreshPending) Monitor_Cleanup(&g_worker.refresh);
    g_worker.refreshPending = FALSE;
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
}

DWORD MonitorWorker_PendingTargets(MonitorTarget targets[MAX_MONITORS])
{
    DWORD mask = 0;
    if (!MonitorWorker_Running()) return 0;
    EnterCriticalSection(&g_worker.lock);
    for (int i = 0; i < g_worker.view->count; i++) {
        if (g_worker.writes[i].pending || g_worker.inFlight[i]) {
            targets[i].monitor = g_worker.view->monitors[i];
            targets[i].percent = g_worker.targets[i];
            mask |= 1u << i;
        }
    }
    LeaveCriticalSection(&g_worker.lock);
    return mask;
}

BOOL MonitorWorker_Accept(const MonitorResult *result)
{
    BOOL accept;
    if (!MonitorWorker_Running() || result->index < 0 || result->index >= g_worker.view->count)
        return FALSE;
    EnterCriticalSection(&g_worker.lock);
    accept = result->generation == g_worker.generation &&
             result->sequence == g_worker.sequences[result->index];
    LeaveCriticalSection(&g_worker.lock);
    return accept;
}

void MonitorWorker_Stop(void)
{
    if (!MonitorWorker_Running()) return;
    MonitorWorker_Reset();
    EnterCriticalSection(&g_worker.lock);
    g_worker.stop = TRUE;
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
    /* Never hang exit on an unresponsive display driver. Process teardown will
       reclaim a stuck worker and its leases; its state must remain alive. */
    if (WaitForSingleObject(g_worker.thread, 2000) == WAIT_OBJECT_0) {
        AcquireSRWLockExclusive(&g_lifecycleLock);
        InterlockedExchange(&g_running, FALSE);
        CloseHandle(g_worker.thread);
        CloseHandle(g_worker.event);
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        DeleteCriticalSection(&g_worker.lock);
    }
}
