#include "monitor_worker.h"
#include <stdlib.h>

typedef struct {
    BrightMonitor monitor;
    DWORD percent, sequence, generation;
    MonitorWritePurpose purpose;
    DWORD otherInput;
    BOOL pending;
} WriteRequest;

typedef struct {
    MonitorList monitors;
    DWORD sequences[MAX_MONITORS];
    DWORD generation;
    BOOL pending;
    BOOL allMonitors;
} RefreshRequest;

typedef enum { WORK_WAIT, WORK_WRITE, WORK_REFRESH, WORK_SOURCES, WORK_STOP } WorkKind;

typedef struct {
    int index;
    union {
        WriteRequest write;
        RefreshRequest refresh;
    } request;
} WorkItem;

typedef struct {
    CRITICAL_SECTION lock;
    HANDLE event, thread;
    HWND owner;
    MonitorList *view;
    BOOL stop;
    DWORD generation;
    DWORD sequences[MAX_MONITORS];
    DWORD targets[MAX_MONITORS];
    MonitorWritePurpose purposes[MAX_MONITORS];
    BOOL inFlight[MAX_MONITORS];
    WriteRequest writes[MAX_MONITORS];
    RefreshRequest refresh;
    RefreshRequest sources;
} Worker;

static Worker g_worker;
static volatile LONG g_running;
static SRWLOCK g_lifecycleLock = SRWLOCK_INIT;

/* Requests own their snapshots' leases. Dequeue transfers that ownership to
   the local work item; cancellation and completion release it exactly once. */
static void ReleaseWriteRequest(WriteRequest *request)
{
    if (request->pending) Monitor_Release(&request->monitor);
    request->pending = FALSE;
}

static void ReleaseRefreshRequest(RefreshRequest *request)
{
    if (request->pending) Monitor_Cleanup(&request->monitors);
    request->pending = FALSE;
}

/* All helpers ending in Locked require the request lock to be held. */
static WorkKind DequeueLocked(int *nextIndex, WorkItem *work)
{
    if (g_worker.stop) return WORK_STOP;

    /* Writes take priority; round-robin keeps a dragged monitor from starving
       other monitors. The cursor stays local to the worker across resets. */
    for (int n = 0; n < MAX_MONITORS; n++) {
        int index = (*nextIndex + n) % MAX_MONITORS;
        WriteRequest *request = &g_worker.writes[index];
        if (!request->pending) continue;
        work->index = index;
        work->request.write = *request;
        request->pending = FALSE;
        g_worker.inFlight[index] = TRUE;
        *nextIndex = (index + 1) % MAX_MONITORS;
        return WORK_WRITE;
    }

    if (g_worker.sources.pending) {
        work->request.refresh = g_worker.sources;
        ZeroMemory(&g_worker.sources, sizeof(g_worker.sources));
        if (work->request.refresh.monitors.count > 0) return WORK_SOURCES;
    }
    if (g_worker.refresh.pending) {
        work->request.refresh = g_worker.refresh;
        ZeroMemory(&g_worker.refresh, sizeof(g_worker.refresh));
        if (work->request.refresh.monitors.count > 0) return WORK_REFRESH;
    }
    return WORK_WAIT;
}

static void CancelPendingLocked(void)
{
    for (int i = 0; i < MAX_MONITORS; i++) {
        ReleaseWriteRequest(&g_worker.writes[i]);
        g_worker.inFlight[i] = FALSE;
    }
    ReleaseRefreshRequest(&g_worker.refresh);
    ReleaseRefreshRequest(&g_worker.sources);
}

static void SnapshotRefreshLocked(RefreshRequest *request, const MonitorList *view)
{
    ReleaseRefreshRequest(request);
    request->monitors = *view;
    for (int i = 0; i < view->count; i++)
        Monitor_Retain(&request->monitors.monitors[i]);
    CopyMemory(request->sequences, g_worker.sequences, sizeof(request->sequences));
    request->generation = g_worker.generation;
    request->pending = TRUE;
}

/* Identity-only snapshots keep the latest write intent, including in-flight
   writes, without retaining handles or treating refresh reads as user intent. */
static DWORD SnapshotTargetsLocked(MonitorTarget targets[MAX_MONITORS])
{
    DWORD mask = 0;
    for (int i = 0; i < g_worker.view->count; i++) {
        if (!g_worker.writes[i].pending && !g_worker.inFlight[i]) continue;
        if (g_worker.purposes[i] == MONITOR_WRITE_IDLE_RELEASE) continue;
        targets[i].monitor = g_worker.view->monitors[i];
        targets[i].percent = g_worker.targets[i];
        mask |= 1u << i;
    }
    return mask;
}

static void PostResult(int index, DWORD generation, DWORD sequence,
                       const BrightMonitor *monitor, BOOL success, MonitorResultKind kind,
                       BOOL sourceUpdated, MonitorWritePurpose purpose, BOOL brightnessWritten,
                       BOOL brightnessUpdated)
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
    result->kind = kind;
    result->currentInput = monitor->currentInput;
    result->sourceKnown = monitor->sourceKnown;
    result->sourceUpdated = sourceUpdated;
    result->sourceCheckedTick = monitor->sourceCheckedTick;
    result->purpose = purpose;
    result->idleEpoch = monitor->idleEpoch;
    result->preIdleBrightnessValid = monitor->preIdleBrightnessValid;
    result->preIdleBrightness = monitor->preIdleBrightness;
    result->brightnessWritten = brightnessWritten;
    result->brightnessUpdated = brightnessUpdated;
    CopyMemory(result->deviceInstance, monitor->deviceInstance, sizeof(result->deviceInstance));
    result->deviceInstance[ARRAYSIZE(result->deviceInstance) - 1] = L'\0';
    result->backend = monitor->backend;
    result->sourceFilter = monitor->sourceFilter;
    result->expectedInput = monitor->expectedInput;
    if (!PostMessageW(g_worker.owner, WM_MONITOR_RESULT, 0, (LPARAM)result))
        free(result);
}

/* Hardware calls and result delivery never hold the request lock. */
typedef struct {
    int index;
    DWORD generation, sequence;
} WriteGuardContext;

static BOOL WriteIsCurrent(void *context)
{
    const WriteGuardContext *guard = (const WriteGuardContext *)context;
    BOOL current;
    EnterCriticalSection(&g_worker.lock);
    current = !g_worker.stop && guard->generation == g_worker.generation &&
              guard->sequence == g_worker.sequences[guard->index];
    LeaveCriticalSection(&g_worker.lock);
    return current;
}

static void ApplyWrite(int index, WriteRequest *request)
{
    WriteGuardContext guard = { index, request->generation, request->sequence };
    BOOL sourceUpdated;
    MonitorWriteOutcome outcome = Monitor_SetBrightnessForPurposeGuardedSync(
        &request->monitor, request->percent, request->purpose, request->otherInput,
        WriteIsCurrent, &guard, &sourceUpdated);
    MonitorResultKind kind = outcome == MONITOR_WRITE_SKIPPED ? MONITOR_RESULT_SKIPPED :
                             outcome == MONITOR_WRITE_CANCELLED ? MONITOR_RESULT_CANCELLED :
                             MONITOR_RESULT_BRIGHTNESS;
    PostResult(index, request->generation, request->sequence,
               &request->monitor, outcome == MONITOR_WRITE_APPLIED, kind, sourceUpdated,
               request->purpose, outcome == MONITOR_WRITE_APPLIED, outcome == MONITOR_WRITE_APPLIED);
    ReleaseWriteRequest(request);
    EnterCriticalSection(&g_worker.lock);
    /* Reset may have already cancelled this generation's outstanding intent. */
    if (request->generation == g_worker.generation)
        g_worker.inFlight[index] = FALSE;
    LeaveCriticalSection(&g_worker.lock);
}

static void ApplyRefresh(RefreshRequest *request)
{
    DWORD readMask = Monitor_RefreshBrightnessSync(&request->monitors);
    for (int i = 0; i < request->monitors.count; i++) {
        if (readMask & (1u << i))
            PostResult(i, request->generation, request->sequences[i],
                       &request->monitors.monitors[i], TRUE, MONITOR_RESULT_BRIGHTNESS, FALSE,
                       MONITOR_WRITE_NORMAL, FALSE, TRUE);
    }
    ReleaseRefreshRequest(request);
}

static void ApplySourceRefresh(RefreshRequest *request)
{
    for (int i = 0; i < request->monitors.count; i++) {
        BrightMonitor *monitor = &request->monitors.monitors[i];
        if (monitor->backend != BACKEND_DDC || !monitor->hasHandle) continue;
        if (!request->allMonitors && (monitor->excludedFromControl ||
            (!monitor->sourceFilter && monitor->controllable))) continue;
        /* A failed initial brightness read must not permanently hide a monitor
           or stop its source polling. Reads recover capability without writing. */
        BOOL brightnessUpdated = !monitor->controllable && Monitor_ReadBrightnessSync(monitor);
        BOOL success = Monitor_ReadSourceSync(monitor);
        /* A failed read must replace stale success with "unknown" in the UI. */
        PostResult(i, request->generation, request->sequences[i], monitor,
                   success, MONITOR_RESULT_SOURCE, TRUE, MONITOR_WRITE_NORMAL, FALSE, brightnessUpdated);
    }
    ReleaseRefreshRequest(request);
}

static DWORD WINAPI WorkerProc(LPVOID unused)
{
    int nextIndex = 0;
    (void)unused;

    for (;;) {
        WorkItem work;
        EnterCriticalSection(&g_worker.lock);
        WorkKind kind = DequeueLocked(&nextIndex, &work);
        LeaveCriticalSection(&g_worker.lock);
        if (kind == WORK_STOP) break;

        if (kind == WORK_WRITE)
            ApplyWrite(work.index, &work.request.write);
        else if (kind == WORK_REFRESH)
            ApplyRefresh(&work.request.refresh);
        else if (kind == WORK_SOURCES)
            ApplySourceRefresh(&work.request.refresh);
        else {
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

static int FindMonitorIndex(const BrightMonitor *monitor)
{
    for (int i = 0; i < g_worker.view->count; i++)
        if (monitor == &g_worker.view->monitors[i]) return i;
    return -1;
}

static BOOL QueueWrite(BrightMonitor *monitor, DWORD percent,
                       MonitorWritePurpose purpose, DWORD otherInput)
{
    if (!MonitorWorker_Running()) return FALSE;
    int index = FindMonitorIndex(monitor);
    if (index < 0) return FALSE;

    EnterCriticalSection(&g_worker.lock);
    WriteRequest *request = &g_worker.writes[index];
    ReleaseWriteRequest(request);
    request->monitor = *monitor;
    Monitor_Retain(&request->monitor);
    request->percent = percent;
    request->purpose = purpose;
    request->otherInput = otherInput;
    g_worker.targets[index] = percent;
    g_worker.purposes[index] = purpose;
    request->sequence = ++g_worker.sequences[index];
    request->generation = g_worker.generation;
    request->pending = TRUE;
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
    return TRUE;
}

BOOL MonitorWorker_Set(BrightMonitor *monitor, DWORD percent)
{
    return QueueWrite(monitor, percent, MONITOR_WRITE_NORMAL, 0);
}

BOOL MonitorWorker_SetIdle(BrightMonitor *monitor, DWORD percent)
{
    return QueueWrite(monitor, percent, MONITOR_WRITE_IDLE, 0);
}

BOOL MonitorWorker_ReleaseIdle(BrightMonitor *monitor, DWORD rawBrightness, DWORD otherInput)
{
    return QueueWrite(monitor, rawBrightness, MONITOR_WRITE_IDLE_RELEASE, otherInput);
}

void MonitorWorker_Cancel(BrightMonitor *monitor)
{
    if (!MonitorWorker_Running()) return;
    int index = FindMonitorIndex(monitor);
    if (index < 0) return;
    EnterCriticalSection(&g_worker.lock);
    ++g_worker.sequences[index];
    ReleaseWriteRequest(&g_worker.writes[index]);
    g_worker.inFlight[index] = FALSE;
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
}

void MonitorWorker_Refresh(const MonitorList *view)
{
    if (!MonitorWorker_Running()) return;
    EnterCriticalSection(&g_worker.lock);
    SnapshotRefreshLocked(&g_worker.refresh, view);
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
}

void MonitorWorker_RefreshSources(const MonitorList *view, BOOL allMonitors)
{
    if (!MonitorWorker_Running()) return;
    EnterCriticalSection(&g_worker.lock);
    SnapshotRefreshLocked(&g_worker.sources, view);
    g_worker.sources.allMonitors = allMonitors;
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
}

void MonitorWorker_Reset(void)
{
    if (!MonitorWorker_Running()) return;
    EnterCriticalSection(&g_worker.lock);
    ++g_worker.generation;
    CancelPendingLocked();
    LeaveCriticalSection(&g_worker.lock);
    SetEvent(g_worker.event);
}

DWORD MonitorWorker_PendingTargets(MonitorTarget targets[MAX_MONITORS])
{
    if (!MonitorWorker_Running()) return 0;
    EnterCriticalSection(&g_worker.lock);
    DWORD mask = SnapshotTargetsLocked(targets);
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

BOOL MonitorWorker_AcceptState(const MonitorResult *result)
{
    BOOL accept;
    if (!MonitorWorker_Running() || result->index < 0 || result->index >= g_worker.view->count)
        return FALSE;
    EnterCriticalSection(&g_worker.lock);
    accept = result->generation == g_worker.generation;
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
