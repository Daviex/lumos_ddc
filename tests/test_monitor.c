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

static BOOL WINAPI MockGetVCPFeatureAndVCPFeatureReply(HANDLE handle, BYTE code,
        LPMC_VCP_CODE_TYPE type, LPDWORD current, LPDWORD maximum)
{
    (void)handle;
    CHECK(code == 0x60);
    sourceCalls++;
    *type = MC_SET_PARAMETER;
    *current = sourceInput;
    *maximum = 0x12;
    if (sourceFailuresRemaining > 0) { sourceFailuresRemaining--; return FALSE; }
    return sourceReadSuccess;
}

static void WINAPI MockSleep(DWORD milliseconds)
{
    CHECK(milliseconds == 100);
    retrySleeps++;
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
    if (sourceAfterBrightnessRead) sourceInput = sourceAfterBrightnessRead;
    CHECK(index < MAX_MONITORS);
    if (index >= MAX_MONITORS) return FALSE;
    *minimum = readings[index].minimum;
    *current = readings[index].current;
    *maximum = readings[index].maximum;
    return readings[index].success;
}

static BOOL WINAPI MockSetMonitorBrightness(HANDLE handle, DWORD value)
{
    (void)handle;
    setCalls++;
    lastWrite = value;
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
    view.monitors[1].delta = 10;
    view.monitors[2].excludedFromControl = TRUE;
    view.monitors[2].backend = BACKEND_WMI;
    view.monitors[2].brightnessCur = 65;
    CHECK(!Monitor_SetBrightness(&view.monitors[0], 5));
    CHECK(!Monitor_SetBrightnessSync(&view.monitors[0], 5));
    CHECK(!Monitor_SetBrightnessSync(&view.monitors[2], 5));
    Monitor_PreviewBrightness(&view.monitors[0], 5);
    Monitor_AdjustActive(&view, -20);
    CHECK(setCalls == 0 && view.monitors[0].brightnessCur == 87);
    Monitor_SetAllBrightness(&view, 20);
    CHECK(setCalls == 1 && lastWrite == 30 && view.monitors[1].brightnessCur == 30);
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

int main(void)
{
    TestReusedZeroHandleLeases();
    TestFlushDoesNotWaitForEnumeration();
    TestRefreshValidatesReadings();
    TestIndependentSourceAndBrightnessRecovery();
    TestTransientSourceReadRetry();
    TestLargeBrightnessRange();
    TestExcludedMonitorsAreNeverWritten();
    TestSourceFilterChecksBeforeEveryWrite();
    TestIdleRestoresExactRawBrightness();
    TestIdleReleaseNeverBypassesUnknownOrChangedSource();
    TestIdleRequiresValidatedBaselineAndAppliedWrite();
    TestNormalSyncWriteClearsIdleOwnershipOnlyWhenApplied();
    if (failures) {
        printf("%d monitor checks failed\n", failures);
        return 1;
    }
    puts("ALL PASS: monitor leases, validated reads and scaling (mocked hardware)");
    return 0;
}
