#ifndef BRIGHTNESS_H
#define BRIGHTNESS_H

#include "monitor.h"

/* Pure conversions over a validated monitor snapshot. Integer division
   preserves the existing truncation; percentages are bounded to 0-100. */
int Brightness_GetPercent(const BrightMonitor *monitor);
DWORD Brightness_ToRaw(const BrightMonitor *monitor, DWORD percent);

/* Recover the base target by averaging controllable monitors after subtracting
   their deltas. Empty/uncontrollable lists use the existing 50% fallback. */
int Brightness_MasterTarget(const MonitorList *view);

/* Deltas extend the master target range so every monitor can reach both ends.
   The range always includes 0-100; monitor deltas are the configured -40..40. */
void Brightness_TargetRange(const MonitorList *view, int *minimum, int *maximum);
int Brightness_SliderToTarget(const MonitorList *view, int sliderPercent);
int Brightness_TargetToSlider(const MonitorList *view, int target);

/* Match a source identity to a controllable candidate without relying on
   names or physical handles. The list search rejects ambiguous matches. */
BOOL Monitor_SameDisplay(const BrightMonitor *source, const BrightMonitor *candidate);
int Monitor_FindUniqueDisplay(const MonitorList *view, const BrightMonitor *source);

#endif /* BRIGHTNESS_H */
