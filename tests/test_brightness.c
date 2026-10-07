/* Pure brightness arithmetic and display identity regression checks.
   No hardware, registry, worker or window operations are linked or invoked. */
#include <limits.h>
#include <stdio.h>
#include <wchar.h>
#include "../brightness.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static BrightMonitor MakeMonitor(DWORD minimum, DWORD current, DWORD maximum, int delta)
{
    BrightMonitor monitor = { 0 };
    monitor.backend = BACKEND_DDC;
    monitor.controllable = TRUE;
    monitor.brightnessMin = minimum;
    monitor.brightnessCur = current;
    monitor.brightnessMax = maximum;
    monitor.delta = delta;
    monitor.rangeHi = 100;
    return monitor;
}

static void TestRawConversions(void)
{
    BrightMonitor monitor = MakeMonitor(20, 60, 100, 0);
    CHECK(Brightness_GetPercent(&monitor) == 50);
    CHECK(Brightness_ToRaw(&monitor, 0) == 20);
    CHECK(Brightness_ToRaw(&monitor, 50) == 60);
    CHECK(Brightness_ToRaw(&monitor, 100) == 100);
    CHECK(Brightness_ToRaw(&monitor, 101) == 100);
    CHECK(Brightness_ToRaw(&monitor, MAXDWORD) == 100);
    monitor = MakeMonitor(10, 11, 13, 0);
    CHECK(Brightness_GetPercent(&monitor) == 33);  /* Preserve integer truncation. */
    CHECK(Brightness_ToRaw(&monitor, 33) == 10);
    CHECK(Brightness_ToRaw(&monitor, 34) == 11);
    monitor = MakeMonitor(1, 0x80000000u, MAXDWORD, 0);
    CHECK(Brightness_GetPercent(&monitor) == 50);
    CHECK(Brightness_ToRaw(&monitor, 50) == 0x80000000u);
    CHECK(Brightness_ToRaw(&monitor, 1) == 42949673u);
    CHECK(Brightness_ToRaw(&monitor, 100) == MAXDWORD);
    monitor.brightnessCur = MAXDWORD;
    CHECK(Brightness_GetPercent(&monitor) == 100);
    monitor = MakeMonitor(20, 10, 100, 0);
    CHECK(Brightness_GetPercent(&monitor) == 0);
    monitor.brightnessCur = 101;
    CHECK(Brightness_GetPercent(&monitor) == 100);
    monitor = MakeMonitor(20, 20, 20, 0);
    CHECK(Brightness_GetPercent(&monitor) == 0);
    CHECK(Brightness_ToRaw(&monitor, 50) == 20);
    monitor.brightnessMax = 10;
    CHECK(Brightness_GetPercent(&monitor) == 0);
    CHECK(Brightness_ToRaw(&monitor, 50) == 20);
    monitor = MakeMonitor(0, 73, 100, 0);
    monitor.backend = BACKEND_WMI;
    CHECK(Brightness_GetPercent(&monitor) == 73);
    CHECK(Brightness_ToRaw(&monitor, 73) == 73);
    CHECK(Brightness_ToRaw(&monitor, 101) == 100);
}

static void TestMasterTargets(void)
{
    MonitorList view = { 0 };
    int minimum, maximum;
    CHECK(Brightness_MasterTarget(&view) == 50);
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);
    view.count = 3;
    view.monitors[0] = MakeMonitor(0, 70, 100, 40);
    view.monitors[0].rangeLo = 40;
    view.monitors[1] = MakeMonitor(0, 30, 100, -40);
    view.monitors[1].rangeHi = 60;
    view.monitors[2] = MakeMonitor(0, 100, 100, 0);
    view.monitors[2].controllable = FALSE;
    CHECK(Brightness_MasterTarget(&view) == 50);  /* Both ranges map their middle to 50. */
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);
    CHECK(Brightness_SliderToTarget(&view, 0) == 0);
    CHECK(Brightness_SliderToTarget(&view, 50) == 50);
    CHECK(Brightness_SliderToTarget(&view, 100) == 100);
    CHECK(Brightness_SliderToTarget(&view, 1) == 1);
    CHECK(Brightness_TargetToSlider(&view, -39) == 0);
    CHECK(Brightness_TargetToSlider(&view, 50) == 50);
    CHECK(Brightness_TargetToSlider(&view, 140) == 100);
    CHECK(Brightness_TargetToSlider(&view, -40) == 0);
    CHECK(Brightness_TargetToSlider(&view, INT_MIN) == 0);
    CHECK(Brightness_TargetToSlider(&view, INT_MAX) == 100);
    CHECK(Brightness_SliderToTarget(&view, INT_MIN) == 0);
    CHECK(Brightness_SliderToTarget(&view, INT_MAX) == 100);
    view.count = 1;
    view.monitors[0] = MakeMonitor(0, 50, 100, 40);
    view.monitors[0].rangeLo = 40;
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);
    CHECK(Brightness_MasterTarget(&view) == 17); /* Inverse range rounds to nearest. */
    CHECK(Brightness_TargetToSlider(&view, 17) == 17);
    view.monitors[0].delta = -40;
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100); /* Legacy offset no longer widens master. */
    view.count = 2;
    view.monitors[0] = MakeMonitor(0, 0, 100, 7);
    view.monitors[1] = MakeMonitor(0, 0, 100, 0);
    CHECK(Brightness_MasterTarget(&view) == 0);  /* Legacy deltas do not affect ranges. */
    view.monitors[0].controllable = view.monitors[1].controllable = FALSE;
    CHECK(Brightness_MasterTarget(&view) == 50);
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);
}

static void TestDisplayIdentity(void)
{
    BrightMonitor source = MakeMonitor(0, 50, 100, 0);
    BrightMonitor candidate = source;
    MonitorList view = { 0 };
    source.hMonitor = candidate.hMonitor = (HMONITOR)(UINT_PTR)1;
    source.hPhysical = (HANDLE)(UINT_PTR)5;
    candidate.hPhysical = (HANDLE)(UINT_PTR)6;
    wcscpy(source.name, L"Same friendly name");
    wcscpy(candidate.name, L"Same friendly name");
    CHECK(Monitor_SameDisplay(&source, &candidate));  /* Physical handles may change. */
    candidate.hMonitor = (HMONITOR)(UINT_PTR)2;
    CHECK(!Monitor_SameDisplay(&source, &candidate));  /* Friendly names are insufficient. */
    wcscpy(source.deviceInstance, L"DISPLAY\\VENDOR\\Device1");
    CHECK(!Monitor_SameDisplay(&source, &candidate));  /* One missing stable identity. */
    wcscpy(candidate.deviceInstance, L"display\\vendor\\device1");
    CHECK(Monitor_SameDisplay(&source, &candidate));  /* Reordered display, same PnP key. */
    candidate.hMonitor = source.hMonitor;
    wcscpy(candidate.deviceInstance, L"DISPLAY\\VENDOR\\Device2");
    CHECK(!Monitor_SameDisplay(&source, &candidate));  /* Reused HMONITOR cannot override key. */
    wcscpy(candidate.deviceInstance, source.deviceInstance);
    candidate.controllable = FALSE;
    CHECK(!Monitor_SameDisplay(&source, &candidate));
    candidate.hasHandle = TRUE;
    CHECK(Monitor_SameDisplay(&source, &candidate)); /* Physical identity survives failed brightness. */
    candidate.hasHandle = FALSE;
    candidate.controllable = TRUE;
    candidate.backend = BACKEND_WMI;
    CHECK(!Monitor_SameDisplay(&source, &candidate));
    source.backend = BACKEND_WMI;
    wcscpy(source.wmiInstance, L"DISPLAY\\PANEL\\Instance_0");
    wcscpy(candidate.wmiInstance, L"display\\panel\\instance_0");
    CHECK(Monitor_SameDisplay(&source, &candidate));
    wcscpy(candidate.wmiInstance, L"DISPLAY\\PANEL\\Instance_1");
    CHECK(!Monitor_SameDisplay(&source, &candidate));
    wcscpy(candidate.wmiInstance, source.wmiInstance);
    view.count = 2;
    view.monitors[0] = MakeMonitor(0, 50, 100, 0);
    view.monitors[1] = candidate;
    CHECK(Monitor_FindUniqueDisplay(&view, &source) == 1);
    view.monitors[0] = candidate;
    CHECK(Monitor_FindUniqueDisplay(&view, &source) == -1);  /* Ambiguous identity. */
    view.monitors[0].controllable = FALSE;
    CHECK(Monitor_FindUniqueDisplay(&view, &source) == 1);
    view.monitors[1].controllable = FALSE;
    CHECK(Monitor_FindUniqueDisplay(&view, &source) == -1);
    source = candidate = MakeMonitor(0, 50, 100, 0);
    CHECK(!Monitor_SameDisplay(&source, &candidate));  /* Null HMONITOR is not an identity. */
}

static void TestSelectedMonitorTargets(void)
{
    MonitorList view = { 0 };
    int minimum, maximum;
    CHECK(!Monitor_CanControl(NULL) && !Monitor_HasSelected(NULL));
    CHECK(!Monitor_HasSelected(&view));
    view.count = 3;
    view.selectedOnly = TRUE;
    view.monitors[0] = MakeMonitor(0, 70, 100, 10);
    view.monitors[0].rangeLo = 10;
    view.monitors[1] = MakeMonitor(0, 50, 100, -10);
    view.monitors[1].rangeHi = 90;
    view.monitors[2] = MakeMonitor(0, 5, 100, 40);
    view.monitors[2].excludedFromControl = TRUE;
    CHECK(Monitor_CanControl(&view.monitors[0]) && Monitor_HasSelected(&view));
    CHECK(!Monitor_CanControl(&view.monitors[2]) && view.monitors[2].controllable);
    CHECK(Brightness_MasterTarget(&view) == 61); /* Average inverse levels 67 and 56. */
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);
    CHECK(Brightness_SliderToTarget(&view, 0) == 0);
    CHECK(Brightness_SliderToTarget(&view, 100) == 100);
    CHECK(Brightness_TargetToSlider(&view, 50) == 50);
    CHECK(Brightness_GetPercent(&view.monitors[2]) == 5); /* Its reading stays available. */

    view.monitors[1].excludedFromControl = TRUE;
    CHECK(Brightness_MasterTarget(&view) == 67);
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);
    view.monitors[0].excludedFromControl = TRUE;
    CHECK(!Monitor_HasSelected(&view) && Brightness_MasterTarget(&view) == 50);
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);
    CHECK(Brightness_SliderToTarget(&view, 50) == 50);

    view.monitors[1].excludedFromControl = FALSE;
    view.monitors[1].controllable = FALSE;
    CHECK(!Monitor_CanControl(&view.monitors[1]) && !Monitor_HasSelected(&view));
    view.monitors[1].controllable = TRUE;
    CHECK(Monitor_HasSelected(&view) && Brightness_MasterTarget(&view) == 56);
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);

    view.monitors[1].sourceFilter = TRUE;
    view.monitors[1].sourceKnown = FALSE;
    CHECK(Brightness_MasterTarget(&view) == 56); /* Suspended source preserves master intent. */

    view.selectedOnly = FALSE;
    for (int i = 0; i < view.count; i++) view.monitors[i].excludedFromControl = FALSE;
    CHECK(Monitor_CanControl(&view.monitors[2]));
    CHECK(Brightness_MasterTarget(&view) == 42); /* Default All includes all three. */
    Brightness_TargetRange(&view, &minimum, &maximum);
    CHECK(minimum == 0 && maximum == 100);
}

int main(void)
{
    TestRawConversions();
    TestMasterTargets();
    TestDisplayIdentity();
    TestSelectedMonitorTargets();
    if (failures) {
        printf("%d brightness checks failed\n", failures);
        return 1;
    }
    puts("ALL PASS: brightness arithmetic, range targets and display identity");
    return 0;
}
