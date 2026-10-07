#ifndef LUMOS_DIAGNOSTICS_H
#define LUMOS_DIAGNOSTICS_H

#include "monitor.h"

/* Temporary diagnostics are enabled in release builds too. Every entry keeps
   the caller's Win32 last-error value intact. Monitor identity survives reorder. */
void Diagnostics_Init(void);
void Diagnostics_Close(void);
void Diagnostics_Log(const char *level, const char *scope, const char *format, ...);
void Diagnostics_Monitor(const BrightMonitor *monitor, const char *level,
                         const char *scope, const char *format, ...);
void Diagnostics_MonitorState(const BrightMonitor *monitor, const char *reason);
const char *Diagnostics_SourceReason(const BrightMonitor *monitor);
const char *Diagnostics_InputName(DWORD input);
const char *Diagnostics_WritePurpose(MonitorWritePurpose purpose);

#endif
