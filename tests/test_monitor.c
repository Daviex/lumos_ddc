/* Exercise the real monitor module without enumeration or hardware access.
   Native reads/writes/destruction are mocked before including monitor.c. */
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
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
void MonitorWorker_Refresh(const MonitorList *view)
{
    (void)view;
    CHECK(FALSE);
}

#define DestroyPhysicalMonitor MockDestroyPhysicalMonitor
#define GetMonitorBrightness MockGetMonitorBrightness
#define SetMonitorBrightness MockSetMonitorBrightness
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
    Monitor_CleanupExcept(&first, &second);
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
    CHECK(BrightnessToPercent(&monitor) == 100);
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

int main(void)
{
    TestReusedZeroHandleLeases();
    TestFlushDoesNotWaitForEnumeration();
    TestRefreshValidatesReadings();
    TestLargeBrightnessRange();
    if (failures) {
        printf("%d monitor checks failed\n", failures);
        return 1;
    }
    puts("ALL PASS: monitor leases, validated reads and scaling (mocked hardware)");
    return 0;
}
