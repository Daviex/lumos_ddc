#ifndef LUMOS_DIAGNOSTICS_H
#define LUMOS_DIAGNOSTICS_H

#include "monitor.h"

/* Diagnostics start only after the persisted setting is loaded. Disabling
   closes the file and stops all output; enabling resumes without a restart.
   Every entry keeps the caller's Win32 last-error value intact. */
void Diagnostics_Init(BOOL enabled);
void Diagnostics_SetEnabled(BOOL enabled);
void Diagnostics_Close(void);
void Diagnostics_Log(const char *level, const char *scope, const char *format, ...);
void Diagnostics_Monitor(const BrightMonitor *monitor, const char *level,
                         const char *scope, const char *format, ...);
void Diagnostics_MonitorState(const BrightMonitor *monitor, const char *reason);
const char *Diagnostics_SourceReason(const BrightMonitor *monitor);
const char *Diagnostics_InputName(DWORD input);
const char *Diagnostics_WritePurpose(MonitorWritePurpose purpose);

#endif
