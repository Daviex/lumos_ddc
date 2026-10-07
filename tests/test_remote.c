/* Exercise real IPC validation and command dispatch with in-memory callbacks.
   No windows, worker threads, hardware, registry or configuration are used. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../remote.h"
#include "../monitor_worker.h"
#include "../ipc.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static MonitorList view;
static Settings settings;
static BOOL workerRunning;
static DWORD pendingMask;
static int targets[16], targetCount, presetCalls, master;
static int acceptedCalls, replyCalls;
static IpcReply reply;
static const HWND appWindow = (HWND)(UINT_PTR)1;
static const HWND replyWindow = (HWND)(UINT_PTR)2;

BOOL MonitorWorker_Running(void) { return workerRunning; }
DWORD MonitorWorker_PendingTargets(MonitorTarget out[MAX_MONITORS])
{
    memset(out, 0, sizeof(MonitorTarget) * MAX_MONITORS);
    return pendingMask;
}
BOOL Monitor_HasSelected(const MonitorList *monitors)
{
    for (int i = 0; i < monitors->count; i++)
        if (monitors->monitors[i].controllable && !monitors->monitors[i].excludedFromControl)
            return TRUE;
    return FALSE;
}
int Monitor_GetPercent(const BrightMonitor *monitor)
{
    CHECK(monitor->brightnessMin == 0 && monitor->brightnessMax == 100);
    return (int)monitor->brightnessCur;
}

static MonitorList *AppMonitors(void) { return &view; }
static Settings *AppSettings(void) { return &settings; }
static int Master(void) { return master; }
static BOOL SetMaster(int target) { master = target; return TRUE; }
static BOOL StepMaster(int step) { master += step; return TRUE; }
static BOOL SetMonitor(int index, int target)
{
    CHECK(index >= 0 && index < view.count && targetCount < 16);
    if (index < 0 || index >= view.count || targetCount >= 16) return FALSE;
    targets[targetCount++] = target;
    view.monitors[index].desiredBrightnessValid = TRUE;
    view.monitors[index].desiredBrightness = (DWORD)target;
    /* Deliberately retain confirmed brightness: a slow/filtered queued write
       has not produced a hardware result yet. */
    pendingMask |= 1u << index;
    return TRUE;
}
static BOOL ApplyPreset(int index)
{
    CHECK(index == 0);
    presetCalls++;
    return TRUE;
}
static void Switch(BOOL on) { (void)on; }
static void Rescan(void) { }
static const AppControl app = {
    AppMonitors, AppSettings, Master, SetMaster, StepMaster,
    SetMonitor, ApplyPreset, Switch, Switch, Rescan
};

static BOOL WINAPI MockIsWindow(HWND hwnd) { return hwnd == replyWindow; }
static BOOL WINAPI MockReplyMessage(LRESULT result)
{
    CHECK(result == IPC_RESULT_ACCEPTED);
    acceptedCalls++;
    return TRUE;
}
static LRESULT WINAPI MockSendMessageTimeoutW(HWND hwnd, UINT message,
    WPARAM from, LPARAM data, UINT flags, UINT timeout, PDWORD_PTR result)
{
    const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)data;
    CHECK(hwnd == replyWindow && from == (WPARAM)appWindow && message == WM_COPYDATA);
    CHECK(flags == SMTO_ABORTIFHUNG && timeout == 2000);
    CHECK(cds && cds->dwData == IPC_REPLY_MAGIC && cds->cbData == sizeof(reply));
    if (cds && cds->lpData) memcpy(&reply, cds->lpData, sizeof(reply));
    replyCalls++;
    *result = TRUE;
    return TRUE;
}

#define IsWindow MockIsWindow
#define ReplyMessage MockReplyMessage
#define SendMessageTimeoutW MockSendMessageTimeoutW
#include "../remote.c"

static void Reset(void)
{
    memset(&view, 0, sizeof(view));
    memset(&settings, 0, sizeof(settings));
    memset(&reply, 0, sizeof(reply));
    memset(targets, 0, sizeof(targets));
    view.count = 1;
    view.monitors[0].controllable = TRUE;
    view.monitors[0].backend = BACKEND_DDC;
    view.monitors[0].brightnessCur = 50;
    view.monitors[0].brightnessMax = 100;
    view.monitors[0].rangeHi = 100;
    wcscpy(view.monitors[0].name, L"Mock display");
    settings.step = 5;
    settings.presetCount = 1;
    wcscpy(settings.presets[0].name, L"Day");
    settings.presets[0].brightness = 80;
    workerRunning = TRUE;
    pendingMask = 0;
    targetCount = presetCalls = acceptedCalls = replyCalls = 0;
    master = 50;
    Remote_Init(&app);
}

static void Command(int command, int value, const WCHAR *monitor, const WCHAR *name)
{
    IpcRequest request = { 0 };
    COPYDATASTRUCT cds;
    LRESULT result = 0;
    request.size = sizeof(request);
    request.command = command;
    request.value = value;
    if (monitor) wcscpy(request.monitor, monitor);
    if (name) wcscpy(request.name, name);
    cds.dwData = IPC_REQUEST_MAGIC;
    cds.cbData = sizeof(request);
    cds.lpData = &request;
    CHECK(Remote_HandleCopyData(appWindow, (WPARAM)replyWindow, (LPARAM)&cds, &result));
    CHECK(result == IPC_RESULT_ACCEPTED || result == IPC_RESULT_FAILED);
}

static void TestRelativeCommandsUseFilteredIntent(void)
{
    Reset();
    BrightMonitor *monitor = &view.monitors[0];
    monitor->sourceFilter = TRUE;
    monitor->sourceKnown = TRUE;
    monitor->expectedInput = 0x0F;
    monitor->currentInput = 0x12;
    Command(CLI_UP, 5, L"1", NULL);
    CHECK(reply.result == IPC_RESULT_OK && targets[0] == 55);
    CHECK(wcsstr(reply.text, L"queued") != NULL);
    CHECK(monitor->brightnessCur == 50 && monitor->currentInput == 0x12);
    pendingMask = 0; /* Even after a skipped write, the filtered intent remains. */
    Command(CLI_UP, 5, L"1", NULL);
    CHECK(reply.result == IPC_RESULT_OK && targets[1] == 60);
    Command(CLI_DOWN, 0, L"Mock display", NULL);
    CHECK(reply.result == IPC_RESULT_OK && targets[2] == 55);
    CHECK(targetCount == 3 && acceptedCalls == 3 && replyCalls == 3);
}

static void TestRelativeCommandsDistinguishPendingAndConfirmedLevels(void)
{
    Reset();
    view.monitors[0].desiredBrightnessValid = TRUE;
    view.monitors[0].desiredBrightness = 75;
    pendingMask = 1u;
    Command(CLI_UP, 5, L"1", NULL);
    CHECK(targets[0] == 80);
    pendingMask = 0;
    view.monitors[0].brightnessCur = 30; /* Fresh external hardware change. */
    Command(CLI_UP, 5, L"1", NULL);
    CHECK(targets[1] == 35);
    view.monitors[0].sourceFilter = TRUE;
    view.monitors[0].desiredBrightness = MAXDWORD;
    Command(CLI_UP, 100, L"1", NULL);
    CHECK(targets[2] == 100); /* Untrusted/stale intent cannot overflow addition. */
}

static void TestPresetRequiresSelectedControllableMonitor(void)
{
    Reset();
    view.monitors[0].excludedFromControl = TRUE;
    Command(CLI_PRESET, 0, NULL, L"Day");
    CHECK(reply.result == IPC_RESULT_FAILED && presetCalls == 0);
    view.monitors[0].excludedFromControl = FALSE;
    view.monitors[0].controllable = FALSE;
    Command(CLI_PRESET, 0, NULL, L"Day");
    CHECK(reply.result == IPC_RESULT_FAILED && presetCalls == 0);
    view.monitors[0].controllable = TRUE;
    Command(CLI_PRESET, 0, NULL, L"Day");
    CHECK(reply.result == IPC_RESULT_OK && presetCalls == 1);
    CHECK(wcsstr(reply.text, L"queued") != NULL);
}

static void TestValidationAndAmbiguousLookupDoNotQueue(void)
{
    Reset();
    Command(CLI_SET, 101, L"1", NULL);
    CHECK(reply.result == IPC_RESULT_FAILED && targetCount == 0 && acceptedCalls == 0);
    view.count = 2;
    view.monitors[1] = view.monitors[0];
    Command(CLI_SET, 60, L"Mock display", NULL);
    CHECK(reply.result == IPC_RESULT_FAILED && targetCount == 0);
    CHECK(wcsstr(reply.text, L"More than one") != NULL);
}

int main(void)
{
    TestRelativeCommandsUseFilteredIntent();
    TestRelativeCommandsDistinguishPendingAndConfirmedLevels();
    TestPresetRequiresSelectedControllableMonitor();
    TestValidationAndAmbiguousLookupDoNotQueue();
    if (failures) {
        printf("%d remote checks failed\n", failures);
        return 1;
    }
    puts("ALL PASS: IPC validation, asynchronous relative intent and selected preset scope");
    return 0;
}
