#ifndef BRIGHTNESS_H
#define BRIGHTNESS_H

#include "monitor.h"

/* Pure conversions over a validated monitor snapshot. Integer division
   preserves the existing truncation; percentages are bounded to 0-100. */
int Brightness_GetPercent(const BrightMonitor *monitor);
DWORD Brightness_ToRaw(const BrightMonitor *monitor, DWORD percent);

/* Recover the master by inverting each selected controllable monitor's range.
   Suspended source filters retain their intent; empty lists use 50%. */
int Brightness_MasterTarget(const MonitorList *view);

/* Master and slider levels both use 0-100. These pure helpers clamp input. */
void Brightness_TargetRange(const MonitorList *view, int *minimum, int *maximum);
int Brightness_SliderToTarget(const MonitorList *view, int sliderPercent);
int Brightness_TargetToSlider(const MonitorList *view, int target);

/* Match a source identity to a controllable candidate without relying on
   names or physical handles. The list search rejects ambiguous matches. */
BOOL Monitor_SameDisplay(const BrightMonitor *source, const BrightMonitor *candidate);
int Monitor_FindUniqueDisplay(const MonitorList *view, const BrightMonitor *source);

#endif /* BRIGHTNESS_H */
