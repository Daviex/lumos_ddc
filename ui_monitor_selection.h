#ifndef UI_MONITOR_SELECTION_H
#define UI_MONITOR_SELECTION_H

#include "presets.h"

typedef void (*MonitorSelectionClosedCallback)(BOOL applied);

BOOL UI_MonitorSelectionInit(HINSTANCE instance);
HWND UI_ShowMonitorSelection(HWND owner, MonitorSelection *working,
                             const MonitorList *monitors,
                             MonitorSelectionClosedCallback closed);
void UI_CloseMonitorSelection(void);
BOOL UI_MonitorSelectionMessage(MSG *message);
/* Merge current hardware telemetry without replacing unsaved picker edits. */
void UI_MonitorSelectionRefresh(const MonitorList *monitors);
/* Keep telemetry freshness aligned with the actual, saved polling interval. */
void UI_MonitorSelectionSetSourcePollInterval(UINT milliseconds);
BOOL UI_MonitorSelectionIsOpen(void);

#endif /* UI_MONITOR_SELECTION_H */
