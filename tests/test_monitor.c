/* Exercise the real monitor module without enumeration or hardware access.
   Native reads/writes/destruction are mocked before including monitor.c. */
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <stdio.h>
#include <string.h>
#include "../monitor.h"
#include "../monitor_worker.h"
#include "../wmibright.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

typedef struct {
    DWORD minimum, current, maximum;
    BOOL success;
} MockReading;
static MockReading readings[MAX_MONITORS];
static HANDLE destroyed[MAX_MONITORS];
static int destroyCalls, setCalls, wakeCalls;
static DWORD lastWrite, wmiRead;
static BOOL setSuccess, wmiReadSuccess;
static int sourceCalls;
static int brightnessReadCalls;
static DWORD sourceInput;
static DWORD sourceAfterBrightnessRead;
static BOOL sourceReadSuccess;
static int sourceFailuresRemaining, retrySleeps;
static DWORD sourceAfterRetrySleep;
static int setFailuresRemaining, readFailuresRemaining;
static DWORD setError, readError, sourceOnWriteFailure;
static DWORD commandGapMs;
static BOOL requireCommandGap;

static BOOL WINAPI MockGetVCPFeatureAndVCPFeatureReply(HANDLE handle, BYTE code,
        LPMC_VCP_CODE_TYPE type, LPDWORD current, LPDWORD maximum)
{
    (void)handle;
    CHECK(code == 0x60);
    sourceCalls++;
    commandGapMs = 0;
    *type = MC_SET_PARAMETER;
    *current = sourceInput;
    *maximum = 0x12;
    if (sourceFailuresRemaining > 0) {
        sourceFailuresRemaining--;
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    SetLastError(sourceReadSuccess ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED);
    return sourceReadSuccess;
}

static void WINAPI MockSleep(DWORD milliseconds)
{
    CHECK(milliseconds == 100);
    retrySleeps++;
    commandGapMs += milliseconds;
    if (sourceAfterRetrySleep) sourceInput = sourceAfterRetrySleep;
}

static BOOL WINAPI MockDestroyPhysicalMonitor(HANDLE handle)
{
    CHECK(destroyCalls < MAX_MONITORS);
    if (destroyCalls < MAX_MONITORS) destroyed[destroyCalls] = handle;
    destroyCalls++;
    return TRUE;
}

static BOOL WINAPI MockGetMonitorBrightness(HANDLE handle, LPDWORD minimum,
                                            LPDWORD current, LPDWORD maximum)
{
    size_t index = (size_t)(UINT_PTR)handle;
    brightnessReadCalls++;
    commandGapMs = 0;
    if (sourceAfterBrightnessRead) sourceInput = sourceAfterBrightnessRead;
    CHECK(index < MAX_MONITORS);
    if (index >= MAX_MONITORS) return FALSE;
    *minimum = readings[index].minimum;
    *current = readings[index].current;
    *maximum = readings[index].maximum;
    if (readFailuresRemaining > 0) {
        readFailuresRemaining--;
        SetLastError(readError);
        return FALSE;
    }
    SetLastError(readings[index].success ? ERROR_SUCCESS : readError);
    return readings[index].success;
}

static BOOL WINAPI MockSetMonitorBrightness(HANDLE handle, DWORD value)
{
    (void)handle;
    setCalls++;
    lastWrite = value;
    if (requireCommandGap && commandGapMs < 100) {
        SetLastError(0xC0262582u);
        return FALSE;
    }
    commandGapMs = 0;
    if (setFailuresRemaining > 0) {
        setFailuresRemaining--;
        sourceAfterRetrySleep = sourceOnWriteFailure;
        SetLastError(setError);
        return FALSE;
    }
    SetLastError(setSuccess ? ERROR_SUCCESS : setError);
    return setSuccess;
}

int Wmi_QueryPanels(WmiPanel *out, int maximum)
{
    (void)out; (void)maximum;
    CHECK(FALSE);  /* Enumeration is forbidden in this harness. */
    return 0;
}

BOOL Wmi_GetBrightness(const WCHAR *instanceName, DWORD *percent)
{
    (void)instanceName;
    *percent = wmiRead;
    return wmiReadSuccess;
}

BOOL Wmi_SetBrightness(const WCHAR *instanceName, DWORD percent)
{
    (void)instanceName;
    setCalls++;
    lastWrite = percent;
    return setSuccess;
}

BOOL MonitorWorker_Running(void) { return FALSE; }
void MonitorWorker_Wake(void) { wakeCalls++; }
BOOL MonitorWorker_Set(BrightMonitor *monitor, DWORD percent)
{
    (void)monitor; (void)percent;
    CHECK(FALSE);
    return FALSE;
}
BOOL MonitorWorker_SetIdle(BrightMonitor *monitor, DWORD percent)
{
    return MonitorWorker_Set(monitor, percent);
}
BOOL MonitorWorker_ReleaseIdle(BrightMonitor *monitor, DWORD rawBrightness, DWORD otherInput)
{
    (void)otherInput;
    return MonitorWorker_Set(monitor, rawBrightness);
}
void MonitorWorker_Refresh(const MonitorList *view)
{
    (void)view;
    CHECK(FALSE);
}

#define DestroyPhysicalMonitor MockDestroyPhysicalMonitor
#define GetMonitorBrightness MockGetMonitorBrightness
#define SetMonitorBrightness MockSetMonitorBrightness
#define GetVCPFeatureAndVCPFeatureReply MockGetVCPFeatureAndVCPFeatureReply
#define Sleep MockSleep
#include "../monitor.c"

static BrightMonitor MakeDdc(UINT_PTR handle)
{
    BrightMonitor monitor = { 0 };
    monitor.hPhysical = (HANDLE)handle;
    monitor.hasHandle = TRUE;
    monitor.controllable = TRUE;
    monitor.backend = BACKEND_DDC;
    monitor.brightnessCur = 50;
    monitor.brightnessMax = 100;
    monitor.rangeHi = 100;
    return monitor;
}

static void ResetMocks(void)
{
    CHECK(g_leases == NULL);
    memset(readings, 0, sizeof(readings));
    memset(destroyed, 0, sizeof(destroyed));
    for (int i = 0; i < MAX_MONITORS; i++) {
        readings[i].minimum = 10;
        readings[i].current = 50;
        readings[i].maximum = 90;
        readings[i].success = TRUE;
    }
    destroyCalls = setCalls = wakeCalls = 0;
    lastWrite = 0;
    setSuccess = wmiReadSuccess = TRUE;
    wmiRead = 52;
    sourceCalls = 0;
    brightnessReadCalls = 0;
    sourceInput = 0x0F;
    sourceAfterBrightnessRead = 0;
    sourceReadSuccess = TRUE;
    sourceFailuresRemaining = retrySleeps = 0;
    sourceAfterRetrySleep = 0;
    setFailuresRemaining = readFailuresRemaining = 0;
    setError = readError = ERROR_NOT_SUPPORTED;
    sourceOnWriteFailure = 0;
    commandGapMs = 0;
    requireCommandGap = FALSE;
}

static void TestReusedZeroHandleLeases(void)
{
    MonitorList first = { 0 }, second = { 0 };
    BrightMonitor queued;
    ResetMocks();
    first.count = second.count = 1;
    first.monitors[0] = second.monitors[0] = MakeDdc(0);
    CHECK(TrackHandle(NULL));  /* Each enumeration owns a reference even for 0. */
    CHECK(TrackHandle(NULL));
    queued = first.monitors[0];
    Monitor_Retain(&queued);
    Monitor_Cleanup(&first);
    Monitor_FlushRetiredHandles();
    CHECK(first.count == 0 && destroyCalls == 0);
    Monitor_Cleanup(&second);
    Monitor_FlushRetiredHandles();
    CHECK(destroyCalls == 0);  /* The queued snapshot keeps the shared handle live. */
    Monitor_Release(&queued);
    CHECK(destroyCalls == 0);  /* UI release must defer native destruction. */
    Monitor_FlushRetiredHandles();
    CHECK(destroyCalls == 1 && destroyed[0] == NULL);
    CHECK(g_leases == NULL && wakeCalls >= 3);
    Monitor_FlushRetiredHandles();
    CHECK(destroyCalls == 1);
}

static void TestFlushDoesNotWaitForEnumeration(void)
{
    BrightMonitor monitor = MakeDdc(2);
    ULONGLONG started;
    ResetMocks();
    CHECK(TrackHandle(monitor.hPhysical));
    Monitor_Release(&monitor);
    AcquireSRWLockExclusive(&g_acquireLock);
    started = GetTickCount64();
    Monitor_FlushRetiredHandles();
    CHECK(GetTickCount64() - started < 250);
    CHECK(destroyCalls == 0);
    ReleaseSRWLockExclusive(&g_acquireLock);
    Monitor_FlushRetiredHandles();
    CHECK(destroyCalls == 1 && destroyed[0] == (HANDLE)(UINT_PTR)2);
    CHECK(g_leases == NULL);
}

static void TestRefreshValidatesReadings(void)
{
    MonitorList view = { 0 };
    ResetMocks();
    view.count = 6;
    for (int i = 0; i < 5; i++) view.monitors[i] = MakeDdc((UINT_PTR)i);
    view.monitors[5] = MakeDdc(5);
    view.monitors[5].hasHandle = FALSE;
    view.monitors[5].backend = BACKEND_WMI;
    readings[1].maximum = readings[1].minimum;  /* Empty range. */
    readings[2].current = readings[2].maximum + 1;
    readings[3].current = readings[3].minimum - 1;
    readings[4].success = FALSE;
    CHECK(Monitor_RefreshBrightnessSync(&view) == ((1u << 0) | (1u << 5)));
    CHECK(view.monitors[0].brightnessMin == 10 && view.monitors[0].brightnessMax == 90);
    CHECK(view.monitors[5].brightnessCur == 52);
    for (int i = 1; i < 5; i++) {
        CHECK(view.monitors[i].brightnessMin == 0);
        CHECK(view.monitors[i].brightnessCur == 50);
        CHECK(view.monitors[i].brightnessMax == 100);
    }
    wmiRead = 101;
    CHECK(Monitor_RefreshBrightnessSync(&view) == 1u);
    CHECK(view.monitors[5].brightnessCur == 52);
    wmiReadSuccess = FALSE;
    CHECK(Monitor_RefreshBrightnessSync(&view) == 1u);
    CHECK(view.monitors[5].brightnessCur == 52);
}

static void TestLargeBrightnessRange(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.brightnessMin = 1;
    monitor.brightnessMax = MAXDWORD;
    CHECK(Monitor_SetBrightnessSync(&monitor, 50));
    CHECK(lastWrite == 0x80000000u && monitor.brightnessCur == 0x80000000u);
    Monitor_PreviewBrightness(&monitor, 100);
    CHECK(monitor.brightnessCur == MAXDWORD);
    CHECK(Brightness_GetPercent(&monitor) == 100);
    CHECK(Monitor_SetBrightnessSync(&monitor, 1));
    CHECK(lastWrite == 42949673u);
    setSuccess = FALSE;
    CHECK(!Monitor_SetBrightnessSync(&monitor, 60));
    CHECK(monitor.brightnessCur == 42949673u);
    setSuccess = TRUE;
    CHECK(Monitor_SetBrightnessSync(&monitor, 101));
    CHECK(lastWrite == MAXDWORD);
    monitor.backend = BACKEND_WMI;
    monitor.hasHandle = FALSE;
    CHECK(Monitor_SetBrightnessSync(&monitor, 101));
    CHECK(lastWrite == 100 && monitor.brightnessCur == 100);
}

static void TestIndependentSourceAndBrightnessRecovery(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.controllable = FALSE;
    readings[0].success = FALSE;
    CHECK(Monitor_ReadSourceSync(&monitor));
    CHECK(monitor.sourceKnown && monitor.currentInput == 0x0F && !monitor.controllable);
    CHECK(!Monitor_ReadBrightnessSync(&monitor) && !monitor.controllable);
    readings[0].success = TRUE;
    readings[0].maximum = readings[0].minimum;
    CHECK(!Monitor_ReadBrightnessSync(&monitor) && !monitor.controllable);
    readings[0].maximum = 90;
    CHECK(Monitor_ReadBrightnessSync(&monitor) && monitor.controllable);
    CHECK(monitor.brightnessMin == 10 && monitor.brightnessCur == 50 && monitor.brightnessMax == 90);
    readings[0].success = FALSE;
    CHECK(!Monitor_ReadBrightnessSync(&monitor) && monitor.controllable);
    CHECK(monitor.brightnessMax == 90 && monitor.sourceKnown);
    CHECK(setCalls == 0);
}

static void TestTransientSourceReadRetry(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    sourceFailuresRemaining = 1;
    CHECK(Monitor_ReadSourceSync(&monitor));
    CHECK(sourceCalls == 2 && retrySleeps == 1 && monitor.currentInput == 0x0F);

    sourceReadSuccess = FALSE;
    CHECK(Monitor_SetBrightnessGuardedSync(&monitor, 40, NULL, NULL) == MONITOR_WRITE_SKIPPED);
    CHECK(sourceCalls == 4 && retrySleeps == 2 && !monitor.sourceKnown && !monitor.currentInput);
    CHECK(setCalls == 0); /* Cached success cannot authorize a write. */

    sourceReadSuccess = TRUE;
    sourceFailuresRemaining = 1;
    sourceAfterRetrySleep = 0x12;
    CHECK(Monitor_SetBrightnessGuardedSync(&monitor, 40, NULL, NULL) == MONITOR_WRITE_SKIPPED);
    CHECK(sourceCalls == 6 && monitor.currentInput == 0x12 && setCalls == 0);

    sourceInput = 0;
    int slept = retrySleeps;
    CHECK(!Monitor_ReadSourceSync(&monitor) && retrySleeps == slept);
    CHECK(sourceCalls == 7); /* A valid API response with an invalid input stays unknown. */
    monitor.hasHandle = FALSE;
    CHECK(!Monitor_ReadSourceSync(&monitor) && sourceCalls == 7 && retrySleeps == slept);
}

static void TestExcludedMonitorsAreNeverWritten(void)
{
    MonitorList view = { 0 };
    ResetMocks();
    view.count = 3;
    for (int i = 0; i < view.count; i++) view.monitors[i] = MakeDdc((UINT_PTR)i);
    view.monitors[0].excludedFromControl = TRUE;
    view.monitors[0].brightnessCur = 87;
    view.monitors[1].rangeLo = 10;
    view.monitors[2].excludedFromControl = TRUE;
    view.monitors[2].backend = BACKEND_WMI;
    view.monitors[2].brightnessCur = 65;
    CHECK(!Monitor_SetBrightness(&view.monitors[0], 5));
    CHECK(!Monitor_SetBrightnessSync(&view.monitors[0], 5));
    CHECK(!Monitor_SetBrightnessSync(&view.monitors[2], 5));
    Monitor_PreviewBrightness(&view.monitors[0], 5);
    Monitor_AdjustActive(&view, -20);
    CHECK(setCalls == 0 && view.monitors[0].brightnessCur == 87);
    CHECK(Monitor_SetAllBrightness(&view, 20));
    CHECK(setCalls == 1 && lastWrite == 28 && view.monitors[1].brightnessCur == 28);
    CHECK(view.monitors[0].brightnessCur == 87 && view.monitors[2].brightnessCur == 65);
    Monitor_CycleActive(&view, 1);
    CHECK(view.active == 1);
    Monitor_CycleActive(&view, -1);
    CHECK(view.active == 1);
    view.monitors[1].excludedFromControl = TRUE;
    CHECK(Monitor_HasControllable(&view) && !Monitor_HasSelected(&view));
    Monitor_SetAllBrightness(&view, 80);
    CHECK(setCalls == 1);
}

static void TestRangeMasterMappingAndWriteFailures(void)
{
    MonitorList view = { 0 };
    ResetMocks();
    view.count = 2;
    view.monitors[0] = MakeDdc(0);
    view.monitors[0].brightnessMin = 20;
    view.monitors[0].rangeLo = 40;
    view.monitors[1] = MakeDdc(1);
    view.monitors[1].backend = BACKEND_WMI;
    view.monitors[1].hasHandle = FALSE;
    view.monitors[1].rangeHi = 60;
    CHECK(Monitor_SetAllBrightness(&view, 50));
    CHECK(setCalls == 2 && lastWrite == 30);
    CHECK(view.monitors[0].brightnessCur == 76 && view.monitors[1].brightnessCur == 30);
    CHECK(view.monitors[0].desiredBrightness == 70 && view.monitors[1].desiredBrightness == 30);
    CHECK(Monitor_MasterFromSnapshot(&view) == 50);
    CHECK(Monitor_StepAllBrightness(&view, 25));
    CHECK(view.monitors[0].desiredBrightness == 85 && view.monitors[1].desiredBrightness == 45);
    CHECK(Monitor_StepAllBrightness(&view, 1000));
    CHECK(view.monitors[0].desiredBrightness == 100 && view.monitors[1].desiredBrightness == 60);
    CHECK(Monitor_StepAllBrightness(&view, -1000));
    CHECK(view.monitors[0].desiredBrightness == 40 && view.monitors[1].desiredBrightness == 0);

    setSuccess = FALSE;
    CHECK(!Monitor_SetAllBrightness(&view, 50));
    CHECK(view.monitors[0].desiredBrightness == 70 && view.monitors[1].desiredBrightness == 30);
    setSuccess = TRUE;
    view.monitors[0].sourceFilter = TRUE;
    view.monitors[0].expectedInput = 0x0F;
    sourceInput = 0x12;
    int before = setCalls;
    CHECK(!Monitor_SetAllBrightness(&view, 75));
    CHECK(setCalls == before + 1); /* Other source is skipped, WMI still progresses. */
    CHECK(view.monitors[0].desiredBrightness == 85 && view.monitors[1].desiredBrightness == 45);
    CHECK(!Monitor_SourceAllowsControl(&view.monitors[0]));
}

static void TestUnavailableMonitorsRetainNewestMasterIntent(void)
{
    MonitorList view = { 0 };
    ResetMocks();
    view.count = 4;
    for (int i = 0; i < view.count; i++) view.monitors[i] = MakeDdc((UINT_PTR)i);
    view.monitors[1].controllable = FALSE;
    view.monitors[1].awaitingAnswer = TRUE;
    view.monitors[1].rangeLo = 40;
    view.monitors[1].desiredBrightnessValid = TRUE;
    view.monitors[1].desiredBrightness = 90;
    view.monitors[2].controllable = FALSE;
    view.monitors[2].rangeHi = 60; /* Initial read failed; source identity remains. */
    view.monitors[3].excludedFromControl = TRUE;
    view.monitors[3].controllable = FALSE;
    view.monitors[3].awaitingAnswer = TRUE;
    view.monitors[3].desiredBrightnessValid = TRUE;
    view.monitors[3].desiredBrightness = 91;
    CHECK(Monitor_SetAllBrightness(&view, 44));
    CHECK(setCalls == 1 && lastWrite == 44);
    CHECK(view.monitors[1].desiredBrightness == 66 && view.monitors[1].brightnessCur == 50);
    CHECK(view.monitors[2].desiredBrightnessValid && view.monitors[2].desiredBrightness == 26);
    CHECK(view.monitors[3].desiredBrightness == 91); /* Excluded intent is untouched. */
}

static void TestUnansweredRecoveryUsesStableIdentity(void)
{
    MonitorList previous = { 0 }, fresh = { 0 };
    unsigned recovered = 99;
    previous.count = fresh.count = 2;
    for (int i = 0; i < 2; i++) {
        previous.monitors[i] = fresh.monitors[i] = MakeDdc((UINT_PTR)i);
        wcscpy(previous.monitors[i].name, L"Identical model");
        wcscpy(fresh.monitors[i].name, L"Identical model");
    }
    wcscpy(previous.monitors[0].deviceInstance, L"DISPLAY\\A");
    wcscpy(previous.monitors[1].deviceInstance, L"DISPLAY\\B");
    previous.monitors[1].controllable = FALSE;
    previous.monitors[1].awaitingAnswer = TRUE;
    wcscpy(fresh.monitors[0].deviceInstance, L"display\\b");
    wcscpy(fresh.monitors[1].deviceInstance, L"DISPLAY\\A");
    fresh.monitors[1].controllable = FALSE;
    CHECK(Monitor_TrackUnanswered(&fresh, &previous, &recovered) == 1);
    CHECK(recovered == 1u && !fresh.monitors[0].awaitingAnswer && fresh.monitors[1].awaitingAnswer);

    previous = fresh;
    fresh.monitors[1].controllable = TRUE;
    CHECK(Monitor_TrackUnanswered(&fresh, &previous, &recovered) == 0);
    CHECK(recovered == 2u && !fresh.monitors[1].awaitingAnswer);

    previous.count = fresh.count = 1;
    previous.monitors[0].controllable = FALSE;
    previous.monitors[0].awaitingAnswer = TRUE;
    wcscpy(fresh.monitors[0].deviceInstance, L"DISPLAY\\REPLACEMENT");
    fresh.monitors[0].controllable = TRUE;
    CHECK(Monitor_TrackUnanswered(&fresh, &previous, &recovered) == 0);
    CHECK(recovered == 0); /* Matching friendly names cannot replace a stable key. */

    previous.monitors[0].controllable = TRUE;
    previous.monitors[0].awaitingAnswer = FALSE;
    fresh.monitors[0].deviceInstance[0] = L'\0';
    wcscpy(fresh.monitors[0].name, L"Digital Flat Panel");
    fresh.monitors[0].controllable = FALSE;
    CHECK(Monitor_TrackUnanswered(&fresh, &previous, &recovered) == 1);
    CHECK(fresh.monitors[0].awaitingAnswer && recovered == 0);
    previous = fresh;
    fresh.monitors[0].controllable = TRUE;
    CHECK(Monitor_TrackUnanswered(&fresh, &previous, &recovered) == 0);
    CHECK(recovered == 1u && !fresh.monitors[0].awaitingAnswer);

    previous.monitors[0].awaitingAnswer = TRUE;
    previous.monitors[0].controllable = FALSE;
    fresh.monitors[0].controllable = FALSE;
    fresh.monitors[0].excludedFromControl = TRUE;
    CHECK(Monitor_TrackUnanswered(&fresh, &previous, &recovered) == 0);
    CHECK(!fresh.monitors[0].awaitingAnswer && recovered == 0);

    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.awaitingAnswer = TRUE;
    monitor.controllable = FALSE;
    CHECK(Monitor_ReadBrightnessSync(&monitor));
    CHECK(monitor.controllable && !monitor.awaitingAnswer);
}

static BOOL RejectAfterSourceRead(void *context)
{
    int expectedCalls = *(int *)context;
    CHECK(sourceCalls == expectedCalls);
    return FALSE;
}

static void TestSourceFilterChecksBeforeEveryWrite(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    CHECK(Monitor_SetBrightness(&monitor, 40));
    CHECK(sourceCalls == 1 && setCalls == 1 && lastWrite == 40);
    CHECK(monitor.sourceKnown && monitor.currentInput == 0x0F);
    CHECK(monitor.desiredBrightnessValid && monitor.desiredBrightness == 40);

    sourceInput = 0x12;  /* A cached match must never authorize another write. */
    CHECK(!Monitor_SetBrightness(&monitor, 60));
    CHECK(sourceCalls == 2 && setCalls == 1 && monitor.brightnessCur == 40);
    CHECK(monitor.sourceKnown && monitor.currentInput == 0x12);
    CHECK(monitor.desiredBrightness == 60);
    Monitor_PreviewBrightness(&monitor, 80);
    CHECK(monitor.brightnessCur == 40);

    sourceReadSuccess = FALSE;
    CHECK(Monitor_SetBrightnessGuardedSync(&monitor, 70, NULL, NULL) == MONITOR_WRITE_SKIPPED);
    CHECK(sourceCalls == 4 && setCalls == 1 && !monitor.sourceKnown);
    CHECK(monitor.currentInput == 0);
    sourceReadSuccess = TRUE;
    sourceInput = 0;  /* Invalid replies also suspend control. */
    CHECK(!Monitor_SetBrightnessSync(&monitor, 70));
    CHECK(!monitor.sourceKnown && setCalls == 1);
    sourceInput = 0x0F;
    monitor.expectedInput = 0;
    CHECK(!Monitor_SetBrightnessSync(&monitor, 70));
    CHECK(sourceCalls == 6 && setCalls == 1);
    monitor.expectedInput = 0x0F;
    int expectedCalls = 7;
    CHECK(Monitor_SetBrightnessGuardedSync(&monitor, 70, RejectAfterSourceRead,
                                          &expectedCalls) == MONITOR_WRITE_CANCELLED);
    CHECK(setCalls == 1 && monitor.brightnessCur == 40);
    CHECK(Monitor_SetBrightnessSync(&monitor, 70));
    CHECK(sourceCalls == 8 && setCalls == 2 && monitor.brightnessCur == 70);
    sourceInput = 256;
    CHECK(!Monitor_SetBrightnessSync(&monitor, 80));
    CHECK(sourceCalls == 9 && !monitor.sourceKnown && monitor.currentInput == 0 && setCalls == 2);

    monitor.sourceFilter = FALSE;
    sourceReadSuccess = FALSE;
    CHECK(Monitor_SetBrightnessSync(&monitor, 80));
    CHECK(sourceCalls == 9 && setCalls == 3);  /* Legacy unfiltered behavior. */
    monitor.sourceFilter = TRUE;
    monitor.backend = BACKEND_WMI;
    monitor.hasHandle = FALSE;
    CHECK(Monitor_SetBrightnessSync(&monitor, 90));
    CHECK(sourceCalls == 9 && setCalls == 4 && monitor.brightnessCur == 90);
}

static void TestIdleRestoresExactRawBrightness(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    monitor.desiredBrightnessValid = TRUE;
    monitor.desiredBrightness = 77;
    readings[0].minimum = 0;
    readings[0].maximum = 255;
    readings[0].current = 128;
    CHECK(Monitor_SetIdleBrightness(&monitor, 10));
    CHECK(brightnessReadCalls == 1 && sourceCalls == 1 && setCalls == 1);
    CHECK(monitor.preIdleBrightnessValid && monitor.preIdleBrightness == 128);
    CHECK(monitor.brightnessCur == 25 && lastWrite == 25);
    CHECK(!monitor.idleApplied); /* Only the app consumes ownership acknowledgements. */
    CHECK(monitor.desiredBrightnessValid && monitor.desiredBrightness == 77);

    sourceInput = 0x12;
    CHECK(Monitor_ReleaseIdleBrightness(&monitor, monitor.preIdleBrightness, 0x12));
    CHECK(lastWrite == 128 && monitor.brightnessCur == 128 && setCalls == 2);
    CHECK(monitor.desiredBrightness == 77);
    CHECK(brightnessReadCalls == 1 && sourceCalls == 2);

    sourceInput = 0x0F;
    readings[0].current = 25; /* An existing session baseline must not be captured again. */
    CHECK(Monitor_SetIdleBrightness(&monitor, 10));
    CHECK(lastWrite == 25 && monitor.preIdleBrightness == 128 && brightnessReadCalls == 1);
    CHECK(monitor.desiredBrightness == 77);
}

static void TestIdleReleaseNeverBypassesUnknownOrChangedSource(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    monitor.currentInput = 0x12;
    monitor.sourceKnown = TRUE;
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x12)); /* Already returned to PC. */
    CHECK(setCalls == 0 && sourceCalls == 1 && monitor.currentInput == 0x0F);
    sourceInput = 0x11;
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x12)); /* Another external input. */
    CHECK(setCalls == 0 && sourceCalls == 2 && monitor.currentInput == 0x11);
    sourceReadSuccess = FALSE;
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x12));
    CHECK(setCalls == 0 && sourceCalls == 4 && !monitor.sourceKnown);
    sourceReadSuccess = TRUE;
    sourceInput = 0;
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x12));
    sourceInput = 0x12;
    int expectedCalls = 6;
    BOOL sourceUpdated = FALSE;
    CHECK(Monitor_SetBrightnessForPurposeGuardedSync(&monitor, 80, MONITOR_WRITE_IDLE_RELEASE,
        0x12, RejectAfterSourceRead, &expectedCalls, &sourceUpdated) == MONITOR_WRITE_CANCELLED);
    CHECK(sourceUpdated && setCalls == 0);
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 101, 0x12)); /* Native range is enforced. */
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x0F));
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0));
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 256));
    monitor.expectedInput = 0;
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x12));
    monitor.expectedInput = 0x0F;
    monitor.sourceFilter = FALSE;
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x12));
    monitor.sourceFilter = TRUE;
    monitor.excludedFromControl = TRUE;
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x12));
    monitor.excludedFromControl = FALSE;
    monitor.backend = BACKEND_WMI;
    CHECK(!Monitor_ReleaseIdleBrightness(&monitor, 80, 0x12));
    CHECK(setCalls == 0 && sourceCalls == 7);
}

static void TestIdleRequiresValidatedBaselineAndAppliedWrite(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    readings[0].success = FALSE;
    CHECK(!Monitor_SetIdleBrightness(&monitor, 10));
    CHECK(!monitor.preIdleBrightnessValid && setCalls == 0 && sourceCalls == 0);
    readings[0].success = TRUE;
    readings[0].current = readings[0].maximum + 1;
    CHECK(!Monitor_SetIdleBrightness(&monitor, 10));
    CHECK(!monitor.preIdleBrightnessValid && setCalls == 0 && sourceCalls == 0);
    readings[0].current = 50;
    setSuccess = FALSE;
    CHECK(!Monitor_SetIdleBrightness(&monitor, 10));
    CHECK(!monitor.preIdleBrightnessValid && setCalls == 1 && sourceCalls == 1);
    setSuccess = TRUE;
    sourceAfterBrightnessRead = 0x12; /* Source changes during the potentially slow baseline read. */
    CHECK(!Monitor_SetIdleBrightness(&monitor, 10));
    CHECK(!monitor.preIdleBrightnessValid && setCalls == 1 && sourceCalls == 2);
    sourceAfterBrightnessRead = 0;
    sourceInput = 0x0F;
    CHECK(Monitor_SetIdleBrightness(&monitor, 10));
    CHECK(monitor.preIdleBrightnessValid && monitor.preIdleBrightness == 50);
    CHECK(setCalls == 2 && sourceCalls == 3 && brightnessReadCalls == 5);
    CHECK(!monitor.desiredBrightnessValid);

    monitor.backend = BACKEND_WMI;
    monitor.preIdleBrightnessValid = FALSE;
    wmiReadSuccess = FALSE;
    CHECK(!Monitor_SetIdleBrightness(&monitor, 10));
    wmiReadSuccess = TRUE;
    wmiRead = 101;
    CHECK(!Monitor_SetIdleBrightness(&monitor, 10));
    wmiRead = 67;
    CHECK(Monitor_SetIdleBrightness(&monitor, 10));
    CHECK(monitor.preIdleBrightnessValid && monitor.preIdleBrightness == 67 && lastWrite == 10);
}

static void TestNormalSyncWriteClearsIdleOwnershipOnlyWhenApplied(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    monitor.preIdleBrightnessValid = TRUE;
    monitor.preIdleBrightness = 80;
    monitor.idleApplied = TRUE;
    monitor.idleDimPending = TRUE;
    monitor.idleReleasePending = TRUE;

    sourceInput = 0x12;
    CHECK(!Monitor_SetBrightness(&monitor, 70));
    CHECK(monitor.idleApplied && monitor.idleDimPending && monitor.idleReleasePending);
    CHECK(setCalls == 0);

    sourceInput = 0x0F;
    setSuccess = FALSE;
    CHECK(!Monitor_SetBrightness(&monitor, 70));
    CHECK(monitor.idleApplied && monitor.idleDimPending && monitor.idleReleasePending);

    setSuccess = TRUE;
    CHECK(Monitor_SetBrightness(&monitor, 70));
    CHECK(!monitor.idleApplied && !monitor.idleDimPending && !monitor.idleReleasePending);
    CHECK(monitor.preIdleBrightnessValid && monitor.preIdleBrightness == 80);
    CHECK(monitor.brightnessCur == 70 && monitor.desiredBrightness == 70);
}

static BOOL CurrentBeforeSleepCount(void *context)
{
    return retrySleeps < *(int *)context;
}

static void TestDdcCommandGapAndTransientWriteRecovery(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    monitor.brightnessCur = 1;
    monitor.idleApplied = TRUE;
    monitor.preIdleBrightnessValid = TRUE;
    monitor.preIdleBrightness = 60;
    requireCommandGap = TRUE;
    CHECK(Monitor_SetBrightness(&monitor, 60));
    CHECK(setCalls == 1 && sourceCalls == 1 && retrySleeps == 1);
    CHECK(monitor.brightnessCur == 60 && !monitor.idleApplied);

    ResetMocks();
    setFailuresRemaining = 2;
    setError = 0xC0262582u;
    CHECK(Monitor_SetBrightnessSync(&monitor, 70));
    CHECK(setCalls == 3 && sourceCalls == 3 && lastWrite == 70);
    CHECK(monitor.brightnessCur == 70);

    ResetMocks();
    setSuccess = FALSE;
    setError = 0xC0262583u;
    CHECK(Monitor_SetBrightnessGuardedSync(&monitor, 80, NULL, NULL) == MONITOR_WRITE_FAILED);
    CHECK(setCalls == 3 && sourceCalls == 3 && monitor.brightnessCur == 70);

    ResetMocks();
    setSuccess = FALSE;
    setError = ERROR_INVALID_HANDLE;
    CHECK(!Monitor_SetBrightnessSync(&monitor, 80));
    CHECK(setCalls == 1 && sourceCalls == 1); /* Unsupported/stale handles are not retried. */

    ResetMocks();
    monitor.sourceFilter = FALSE;
    setFailuresRemaining = 1;
    setError = 0xC0262582u;
    CHECK(Monitor_SetBrightnessSync(&monitor, 80));
    CHECK(setCalls == 2 && sourceCalls == 0 && retrySleeps == 1);
}

static void TestDdcRetriesRespectSourceAndCancellation(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    setFailuresRemaining = 1;
    setError = 0xC0262582u;
    sourceOnWriteFailure = 0x12;
    CHECK(Monitor_SetBrightnessGuardedSync(&monitor, 60, NULL, NULL) == MONITOR_WRITE_SKIPPED);
    CHECK(setCalls == 1 && sourceCalls == 2 && monitor.currentInput == 0x12);
    CHECK(monitor.brightnessCur == 50);

    ResetMocks();
    sourceInput = 0x12;
    setFailuresRemaining = 1;
    setError = 0xC0262582u;
    sourceOnWriteFailure = 0x0F;
    CHECK(Monitor_SetBrightnessForPurposeGuardedSync(&monitor, 60, MONITOR_WRITE_IDLE_RELEASE,
        0x12, NULL, NULL, NULL) == MONITOR_WRITE_SKIPPED);
    CHECK(setCalls == 1 && sourceCalls == 2 && monitor.currentInput == 0x0F);

    ResetMocks();
    int cancelAfterSleeps = 1;
    CHECK(Monitor_SetBrightnessGuardedSync(&monitor, 60, CurrentBeforeSleepCount,
        &cancelAfterSleeps) == MONITOR_WRITE_CANCELLED);
    CHECK(setCalls == 0 && sourceCalls == 1 && monitor.brightnessCur == 50);

    ResetMocks();
    setFailuresRemaining = 1;
    setError = 0xC0262582u;
    cancelAfterSleeps = 2;
    CHECK(Monitor_SetBrightnessGuardedSync(&monitor, 60, CurrentBeforeSleepCount,
        &cancelAfterSleeps) == MONITOR_WRITE_CANCELLED);
    CHECK(setCalls == 1 && sourceCalls == 2 && monitor.brightnessCur == 50);
}

static void TestDdcReadRetryAndOriginalIdleBaseline(void)
{
    BrightMonitor monitor = MakeDdc(0);
    ResetMocks();
    readFailuresRemaining = 1;
    readError = 0xC0262582u;
    CHECK(Monitor_ReadBrightnessSync(&monitor));
    CHECK(brightnessReadCalls == 2 && retrySleeps == 1 && monitor.brightnessCur == 50);

    ResetMocks();
    readings[0].success = FALSE;
    readError = 0xC0262583u;
    CHECK(!Monitor_ReadBrightnessSync(&monitor));
    CHECK(brightnessReadCalls == 2 && monitor.controllable && monitor.brightnessCur == 50);

    ResetMocks();
    monitor.sourceFilter = TRUE;
    monitor.expectedInput = 0x0F;
    setFailuresRemaining = 1;
    setError = 0xC0262582u;
    requireCommandGap = TRUE;
    CHECK(Monitor_SetIdleBrightness(&monitor, 1));
    CHECK(brightnessReadCalls == 1 && sourceCalls == 2 && setCalls == 2);
    CHECK(monitor.preIdleBrightnessValid && monitor.preIdleBrightness == 50);
    CHECK(monitor.brightnessCur == 10 && monitor.brightnessMin == 10);

    ResetMocks();
    monitor.preIdleBrightnessValid = FALSE;
    setSuccess = FALSE;
    setError = 0xC0262582u;
    CHECK(!Monitor_SetIdleBrightness(&monitor, 1));
    CHECK(brightnessReadCalls == 1 && setCalls == 3 && !monitor.preIdleBrightnessValid);
}

int main(void)
{
    TestReusedZeroHandleLeases();
    TestFlushDoesNotWaitForEnumeration();
    TestRefreshValidatesReadings();
    TestIndependentSourceAndBrightnessRecovery();
    TestTransientSourceReadRetry();
    TestLargeBrightnessRange();
    TestExcludedMonitorsAreNeverWritten();
    TestRangeMasterMappingAndWriteFailures();
    TestUnavailableMonitorsRetainNewestMasterIntent();
    TestUnansweredRecoveryUsesStableIdentity();
    TestSourceFilterChecksBeforeEveryWrite();
    TestIdleRestoresExactRawBrightness();
    TestIdleReleaseNeverBypassesUnknownOrChangedSource();
    TestIdleRequiresValidatedBaselineAndAppliedWrite();
    TestNormalSyncWriteClearsIdleOwnershipOnlyWhenApplied();
    TestDdcCommandGapAndTransientWriteRecovery();
    TestDdcRetriesRespectSourceAndCancellation();
    TestDdcReadRetryAndOriginalIdleBaseline();
    if (failures) {
        printf("%d monitor checks failed\n", failures);
        return 1;
    }
    puts("ALL PASS: monitor leases, validated reads and scaling (mocked hardware)");
    return 0;
}
