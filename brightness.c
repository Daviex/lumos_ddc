#include "brightness.h"
#include "brightmap.h"
#include <wchar.h>

BOOL Monitor_CanControl(const BrightMonitor *monitor)
{
    return monitor && monitor->controllable && !monitor->excludedFromControl;
}

/* Source eligibility is transient. Keep it separate from the saved scope:
   rescan recovery and master intent must still include suspended monitors. */
BOOL Monitor_SourceAllowsControl(const BrightMonitor *monitor)
{
    if (!monitor) return FALSE;
    if (!monitor->sourceFilter || monitor->backend == BACKEND_WMI) return TRUE;
    return monitor->expectedInput > 0 && monitor->expectedInput <= 255 &&
           monitor->sourceKnown && monitor->currentInput == monitor->expectedInput;
}

BOOL Monitor_HasSelected(const MonitorList *view)
{
    if (view) {
        for (int i = 0; i < view->count; i++)
            if (Monitor_CanControl(&view->monitors[i])) return TRUE;
    }
    return FALSE;
}

int Brightness_GetPercent(const BrightMonitor *monitor)
{
    if (!monitor || monitor->brightnessMax <= monitor->brightnessMin) return 0;
    if (monitor->brightnessCur <= monitor->brightnessMin) return 0;
    if (monitor->brightnessCur >= monitor->brightnessMax) return 100;

    DWORD range = monitor->brightnessMax - monitor->brightnessMin;
    return (int)(((ULONGLONG)(monitor->brightnessCur - monitor->brightnessMin) * 100) / range);
}

DWORD Brightness_ToRaw(const BrightMonitor *monitor, DWORD percent)
{
    if (!monitor) return 0;
    if (percent > 100) percent = 100;
    if (monitor->backend == BACKEND_WMI) return percent;
    if (monitor->brightnessMax <= monitor->brightnessMin) return monitor->brightnessMin;

    DWORD range = monitor->brightnessMax - monitor->brightnessMin;
    return monitor->brightnessMin + (DWORD)(((ULONGLONG)range * percent) / 100);
}

int Brightness_MasterTarget(const MonitorList *view)
{
    int sum = 0, count = 0;
    if (view) {
        for (int i = 0; i < view->count; i++) {
            const BrightMonitor *monitor = &view->monitors[i];
            if (!Monitor_CanControl(monitor)) continue;
            sum += BrightMap_Master(Brightness_GetPercent(monitor),
                                    monitor->rangeLo, monitor->rangeHi);
            count++;
        }
    }
    return count > 0 ? sum / count : 50;
}

void Brightness_TargetRange(const MonitorList *view, int *minimum, int *maximum)
{
    (void)view;
    if (minimum) *minimum = 0;
    if (maximum) *maximum = 100;
}

int Brightness_SliderToTarget(const MonitorList *view, int sliderPercent)
{
    (void)view;
    if (sliderPercent < 0) sliderPercent = 0;
    if (sliderPercent > 100) sliderPercent = 100;
    return sliderPercent;
}

int Brightness_TargetToSlider(const MonitorList *view, int target)
{
    (void)view;
    return target < 0 ? 0 : (target > 100 ? 100 : target);
}

BOOL Monitor_SameDisplay(const BrightMonitor *source, const BrightMonitor *candidate)
{
    if (!source || !candidate || source->backend != candidate->backend ||
        (!candidate->controllable && !(candidate->backend == BACKEND_DDC &&
          candidate->hasHandle && candidate->deviceInstance[0]))) return FALSE;
    /* A connected physical display keeps its stable identity through transient
       brightness failures, so rescans cannot lose its pending idle restore. */
    if (source->backend == BACKEND_WMI)
        return _wcsicmp(source->wmiInstance, candidate->wmiInstance) == 0;
    if (source->deviceInstance[0] || candidate->deviceInstance[0])
        return _wcsicmp(source->deviceInstance, candidate->deviceInstance) == 0;
    return source->hMonitor && source->hMonitor == candidate->hMonitor;
}

int Monitor_FindUniqueDisplay(const MonitorList *view, const BrightMonitor *source)
{
    int match = -1;
    if (!view || !source) return -1;
    for (int i = 0; i < view->count; i++) {
        if (!Monitor_SameDisplay(source, &view->monitors[i])) continue;
        if (match >= 0) return -1;
        match = i;
    }
    return match;
}
