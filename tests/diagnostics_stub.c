/* Regression suites capture hardware/window effects in memory, so logging in
   those suites must not create runtime files beside their test executable. */
#include "../diagnostics.h"

void Diagnostics_Init(void) {}
void Diagnostics_Close(void) {}
void Diagnostics_Log(const char *level, const char *scope, const char *format, ...)
{ (void)level; (void)scope; (void)format; }
void Diagnostics_Monitor(const BrightMonitor *monitor, const char *level,
                         const char *scope, const char *format, ...)
{ (void)monitor; (void)level; (void)scope; (void)format; }
void Diagnostics_MonitorState(const BrightMonitor *monitor, const char *reason)
{ (void)monitor; (void)reason; }
const char *Diagnostics_SourceReason(const BrightMonitor *monitor)
{ (void)monitor; return "test"; }
const char *Diagnostics_InputName(DWORD input)
{ (void)input; return "test"; }
const char *Diagnostics_WritePurpose(MonitorWritePurpose purpose)
{ (void)purpose; return "test"; }
