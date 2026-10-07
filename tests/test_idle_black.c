/* Exercise real overlay/power policy with mock windows, input and monitor
   geometry. GDI painting targets an offscreen DIB; no screen is covered. */
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
#include "../idle_black.h"

static int failures, creates, destroys, wakes, powerCalls, positions;
static DWORD inputTick = 100;
static BOOL inputOk = TRUE, createOk = TRUE, timerOk = TRUE;
static BOOL inputDuringCreate;
static int powerFailuresRemaining;
static EXECUTION_STATE powerFlags;
static EXECUTION_STATE mockPowerState = ES_CONTINUOUS;
static DWORD windowStyle, windowExStyle;
static RECT lastBounds;
static int offset;
static HWND owner = (HWND)(UINT_PTR)99;
typedef struct {
    HWND window;
    RECT screen;
    BOOL visible, minimized, maximized, clientFailure, screenFailure;
    DWORD cloaked;
    LONG_PTR exStyle;
    LONG_PTR frameStyle;
    const WCHAR *className;
    HMONITOR monitor;
} ProbeWindow;
static ProbeWindow probeWindows[8];
static int probeCount;
static BOOL enumerationOk;
static BOOL dpiAvailable, rejectDpiV2;
static int dpiCalls;
static HANDLE dpiContext;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static ProbeWindow *FindProbe(HWND window)
{
    for (int i = 0; i < probeCount; i++)
        if (probeWindows[i].window == window) return &probeWindows[i];
    return NULL;
}
static ProbeWindow *AddProbe(HWND window, const WCHAR *className)
{
    CHECK(probeCount < (int)ARRAYSIZE(probeWindows));
    ProbeWindow *probe = &probeWindows[probeCount++];
    probe->window = window;
    probe->screen = (RECT){0, 0, 1920, 1080};
    probe->visible = TRUE;
    probe->className = className;
    probe->monitor = (HMONITOR)(UINT_PTR)1;
    return probe;
}
static BOOL WINAPI MockEnumWindows(WNDENUMPROC callback, LPARAM parameter)
{
    if (!enumerationOk) return FALSE;
    for (int i = 0; i < probeCount; i++)
        if (!callback(probeWindows[i].window, parameter)) return FALSE;
    return TRUE;
}
static BOOL WINAPI MockVisible(HWND window)
{ ProbeWindow *probe = FindProbe(window); return probe && probe->visible; }
static BOOL WINAPI MockIconic(HWND window)
{ ProbeWindow *probe = FindProbe(window); return probe && probe->minimized; }
static BOOL WINAPI MockZoomed(HWND window)
{ ProbeWindow *probe = FindProbe(window); return probe && probe->maximized; }
static LONG_PTR WINAPI MockWindowStyle(HWND window, int index)
{
    CHECK(index == GWL_EXSTYLE || index == GWL_STYLE);
    ProbeWindow *probe = FindProbe(window);
    return probe ? (index == GWL_EXSTYLE ? probe->exStyle : probe->frameStyle) : 0;
}
static int WINAPI MockClassName(HWND window, LPWSTR name, int capacity)
{
    ProbeWindow *probe = FindProbe(window);
    if (!probe) return 0;
    int length = (int)wcslen(probe->className);
    CHECK(length < capacity);
    memcpy(name, probe->className, ((size_t)length + 1) * sizeof(WCHAR));
    return length;
}
static HRESULT WINAPI MockDwmAttribute(HWND window, DWORD attribute, PVOID data, DWORD size)
{
    CHECK(attribute == DWMWA_CLOAKED && size == sizeof(DWORD));
    ProbeWindow *probe = FindProbe(window);
    if (!probe) return E_FAIL;
    *(DWORD *)data = probe->cloaked;
    return S_OK;
}
static BOOL WINAPI MockClientToScreen(HWND window, LPPOINT point)
{
    ProbeWindow *probe = FindProbe(window);
    if (!probe || probe->screenFailure) return FALSE;
    point->x += probe->screen.left;
    point->y += probe->screen.top;
    return TRUE;
}
static HMONITOR WINAPI MockMonitorFromWindow(HWND window, DWORD flags)
{
    CHECK(flags == MONITOR_DEFAULTTONULL);
    ProbeWindow *probe = FindProbe(window);
    return probe ? probe->monitor : NULL;
}
static DWORD WINAPI MockWindowProcess(HWND window, LPDWORD process)
{ CHECK(FindProbe(window) != NULL); *process = 1234; return 1; }

static ATOM WINAPI MockRegister(const WNDCLASSEXW *window)
{
    CHECK(window->hbrBackground == GetStockObject(BLACK_BRUSH));
    CHECK(window->hCursor == NULL);
    return 1;
}
static BOOL WINAPI MockUnregister(LPCWSTR name, HINSTANCE instance)
{ (void)name; (void)instance; return TRUE; }
static HWND WINAPI MockCreate(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style,
    int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID data)
{
    (void)cls; (void)title; (void)menu; (void)instance; (void)data;
    CHECK(parent == owner);
    creates++;
    windowStyle = style;
    windowExStyle = ex;
    lastBounds = (RECT){x, y, x + width, y + height};
    if (inputDuringCreate) inputTick++;
    return createOk ? (HWND)(UINT_PTR)(creates + 100) : NULL;
}
static BOOL WINAPI MockDestroy(HWND window)
{ CHECK(window != NULL); destroys++; return TRUE; }
static BOOL WINAPI MockPosition(HWND window, HWND after, int x, int y, int w, int h, UINT flags)
{
    CHECK(window != NULL && after == HWND_TOPMOST);
    CHECK((flags & (SWP_NOACTIVATE | SWP_SHOWWINDOW)) == (SWP_NOACTIVATE | SWP_SHOWWINDOW));
    positions++;
    lastBounds = (RECT){x, y, x + w, y + h};
    return TRUE;
}
static BOOL WINAPI MockMonitorInfo(HMONITOR monitor, LPMONITORINFO info)
{
    if (monitor == (HMONITOR)(UINT_PTR)1) info->rcMonitor = (RECT){0, 0, 1920, 1080};
    else if (monitor == (HMONITOR)(UINT_PTR)2)
        info->rcMonitor = (RECT){-2560 + offset, -200, offset, 1240};
    else return FALSE;
    info->rcWork = (RECT){0, 0, 1, 1}; /* Work area must not limit the cover. */
    return TRUE;
}
static BOOL WINAPI MockInput(PLASTINPUTINFO input)
{ input->dwTime = inputTick; return inputOk; }
static UINT_PTR WINAPI MockTimer(HWND window, UINT_PTR id, UINT delay, TIMERPROC callback)
{
    CHECK(window != NULL && delay == 100 && callback == NULL);
    return timerOk ? id : 0;
}
static BOOL WINAPI MockPost(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    CHECK(window == owner && message == WM_IDLE_BLACK_WAKE && wp == 0 && lp == 0);
    wakes++;
    return TRUE;
}
static EXECUTION_STATE WINAPI MockPower(EXECUTION_STATE flags)
{
    powerCalls++;
    powerFlags = flags;
    if (powerFailuresRemaining) {
        powerFailuresRemaining--;
        return 0;
    }
    EXECUTION_STATE previous = mockPowerState;
    mockPowerState = flags;
    return previous;
}
static HMODULE WINAPI MockModule(LPCWSTR module) { (void)module; return (HMODULE)(UINT_PTR)1; }
static HANDLE WINAPI MockSetDpi(HANDLE context)
{
    dpiCalls++;
    if (rejectDpiV2 && context == (HANDLE)(LONG_PTR)-4) return NULL;
    HANDLE previous = dpiContext;
    dpiContext = context;
    return previous;
}
static FARPROC WINAPI MockProc(HMODULE module, LPCSTR name)
{
    (void)module;
    if (!dpiAvailable || strcmp(name, "SetThreadDpiAwarenessContext")) return NULL;
    union { FARPROC generic; HANDLE (WINAPI *setDpi)(HANDLE); } dpi;
    dpi.setDpi = MockSetDpi;
    return dpi.generic;
}
static HCURSOR WINAPI MockCursor(HCURSOR cursor) { CHECK(cursor == NULL); return NULL; }
static BOOL WINAPI MockClient(HWND window, LPRECT rect)
{
    ProbeWindow *probe = FindProbe(window);
    if (probe) {
        if (probe->clientFailure) return FALSE;
        *rect = (RECT){0, 0, probe->screen.right - probe->screen.left, probe->screen.bottom - probe->screen.top};
    } else *rect = (RECT){0, 0, 8, 8};
    return TRUE;
}

#define RegisterClassExW MockRegister
#define UnregisterClassW MockUnregister
#define CreateWindowExW MockCreate
#define DestroyWindow MockDestroy
#define SetWindowPos MockPosition
#define GetMonitorInfoW MockMonitorInfo
#define GetLastInputInfo MockInput
#define SetTimer MockTimer
#define PostMessageW MockPost
#define SetThreadExecutionState MockPower
#define GetModuleHandleW MockModule
#define GetProcAddress MockProc
#define SetCursor MockCursor
#define GetClientRect MockClient
#define EnumWindows MockEnumWindows
#define IsWindowVisible MockVisible
#define IsIconic MockIconic
#define IsZoomed MockZoomed
#define GetWindowLongPtrW MockWindowStyle
#define GetClassNameW MockClassName
#define DwmGetWindowAttribute MockDwmAttribute
#define ClientToScreen MockClientToScreen
#define MonitorFromWindow MockMonitorFromWindow
#define GetWindowThreadProcessId MockWindowProcess
#include "../idle_black.c"
#undef RegisterClassExW
#undef UnregisterClassW
#undef CreateWindowExW
#undef DestroyWindow
#undef SetWindowPos
#undef GetMonitorInfoW
#undef GetLastInputInfo
#undef SetTimer
#undef PostMessageW
#undef SetThreadExecutionState
#undef GetModuleHandleW
#undef GetProcAddress
#undef SetCursor
#undef GetClientRect
#undef EnumWindows
#undef IsWindowVisible
#undef IsIconic
#undef IsZoomed
#undef GetWindowLongPtrW
#undef GetClassNameW
#undef DwmGetWindowAttribute
#undef ClientToScreen
#undef MonitorFromWindow
#undef GetWindowThreadProcessId

static MonitorList MakeView(void)
{
    MonitorList view = {0};
    view.count = 2;
    for (int i = 0; i < view.count; i++) {
        view.monitors[i].controllable = TRUE;
        view.monitors[i].backend = BACKEND_DDC;
        view.monitors[i].hMonitor = (HMONITOR)(UINT_PTR)(i + 1);
    }
    view.monitors[1].idleBlack = TRUE;
    view.monitors[1].sourceFilter = TRUE;
    view.monitors[1].sourceKnown = TRUE;
    view.monitors[1].expectedInput = view.monitors[1].currentInput = 0x0F;
    return view;
}

static void Reset(void)
{
    powerFailuresRemaining = 0;
    IdleBlack_Shutdown();
    creates = destroys = wakes = powerCalls = positions = 0;
    inputOk = createOk = timerOk = TRUE;
    inputDuringCreate = FALSE;
    offset = 0;
    inputTick = 100;
    mockPowerState = ES_CONTINUOUS;
    memset(probeWindows, 0, sizeof(probeWindows));
    probeCount = 0;
    enumerationOk = TRUE;
    dpiAvailable = rejectDpiV2 = FALSE;
    dpiCalls = 0;
    dpiContext = (HANDLE)(LONG_PTR)-1;
    IdleBlack_Init((HINSTANCE)(UINT_PTR)1, owner);
}

static void TestExternalFullscreenExcludesOwnCoverAndDesktop(void)
{
    Reset();
    MonitorList view = MakeView();
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(IdleBlack_Active());
    AddProbe(g_blackWindows[0].window, L"LumosIdleBlack");
    CHECK(!IdleBlack_HasExternalFullscreen()); /* Even with full client bounds and no style hint. */
    AddProbe((HWND)(UINT_PTR)201, L"Progman");
    AddProbe((HWND)(UINT_PTR)202, L"WorkerW");
    CHECK(!IdleBlack_HasExternalFullscreen());
    ProbeWindow *app = AddProbe((HWND)(UINT_PTR)203, L"VideoApp");
    app->screen.bottom = 1040; /* Maximized content leaves the taskbar/work-area margin. */
    CHECK(!IdleBlack_HasExternalFullscreen());
    app->screen.bottom = 1080;
    app->maximized = TRUE;
    app->frameStyle = WS_CAPTION | WS_THICKFRAME;
    CHECK(!IdleBlack_HasExternalFullscreen()); /* Custom title bars with an auto-hidden taskbar. */
    app->frameStyle = 0;
    CHECK(IdleBlack_HasExternalFullscreen()); /* A borderless fullscreen can still be maximized. */
    app->maximized = FALSE;
    app->visible = FALSE;
    CHECK(!IdleBlack_HasExternalFullscreen());
    app->visible = TRUE;
    app->minimized = TRUE;
    CHECK(!IdleBlack_HasExternalFullscreen());
    app->minimized = FALSE;
    app->cloaked = DWM_CLOAKED_SHELL;
    CHECK(!IdleBlack_HasExternalFullscreen()); /* Another virtual desktop is not this user's content. */
    app->cloaked = 0;
    app->exStyle = WS_EX_TOOLWINDOW;
    CHECK(!IdleBlack_HasExternalFullscreen());
    app->exStyle = WS_EX_NOACTIVATE;
    CHECK(!IdleBlack_HasExternalFullscreen());
    app->exStyle = 0;
    CHECK(IdleBlack_HasExternalFullscreen()); /* Detect a real app even under the topmost cover. */
    app->monitor = (HMONITOR)(UINT_PTR)2;
    app->screen = (RECT){-2560, -200, 0, 1240};
    CHECK(IdleBlack_HasExternalFullscreen()); /* Secondary display and negative coordinates. */
    app->screen.right--;
    CHECK(!IdleBlack_HasExternalFullscreen());
    app->screen.right++;
    app->clientFailure = TRUE;
    CHECK(!IdleBlack_HasExternalFullscreen());
    app->clientFailure = FALSE;
    app->screenFailure = TRUE;
    CHECK(!IdleBlack_HasExternalFullscreen());
    app->screenFailure = FALSE;
    app->monitor = NULL;
    CHECK(!IdleBlack_HasExternalFullscreen());
    enumerationOk = FALSE;
    CHECK(IdleBlack_HasExternalFullscreen()); /* Preserve the shell block if enumeration fails. */
}

static void TestFullscreenProbeRestoresDpiContext(void)
{
    Reset();
    AddProbe((HWND)(UINT_PTR)203, L"VideoApp");
    dpiAvailable = TRUE;
    CHECK(IdleBlack_HasExternalFullscreen());
    CHECK(dpiCalls == 2 && dpiContext == (HANDLE)(LONG_PTR)-1);
    dpiCalls = 0;
    enumerationOk = FALSE;
    CHECK(IdleBlack_HasExternalFullscreen());
    CHECK(dpiCalls == 2 && dpiContext == (HANDLE)(LONG_PTR)-1);
    dpiCalls = 0;
    rejectDpiV2 = TRUE;
    CHECK(IdleBlack_HasExternalFullscreen());
    CHECK(dpiCalls == 3 && dpiContext == (HANDLE)(LONG_PTR)-1);
}

static void TestCoverageAndWake(void)
{
    Reset();
    MonitorList view = MakeView();
    IdleBlack_Update(&view, TRUE, FALSE);
    CHECK(!IdleBlack_Active() && creates == 0 && powerCalls == 1);
    CHECK(powerFlags == (ES_CONTINUOUS | ES_DISPLAY_REQUIRED));
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(IdleBlack_Active() && creates == 1);
    CHECK(lastBounds.left == -2560 && lastBounds.top == -200 && lastBounds.right == 0 && lastBounds.bottom == 1240);
    CHECK(windowStyle == WS_POPUP);
    CHECK((windowExStyle & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) ==
          (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
    CHECK(!(windowExStyle & WS_EX_LAYERED)); /* Fully opaque, with no alpha blend. */
    HWND window = g_blackWindows[0].window;
    CHECK(BlackWndProc(window, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE);
    CHECK(BlackWndProc(window, WM_SETCURSOR, 0, 0) == TRUE);
    BlackWndProc(window, WM_TIMER, BLACK_INPUT_TIMER, 0);
    CHECK(IdleBlack_Active() && wakes == 0); /* Synthetic pointer events do not wake. */
    offset = 200;
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(creates == 1 && lastBounds.left == -2360 && lastBounds.right == 200);
    inputTick++;
    BlackWndProc(window, WM_TIMER, BLACK_INPUT_TIMER, 0);
    CHECK(!IdleBlack_Active() && destroys == 1 && wakes == 1);
    CHECK(powerCalls == 1); /* Input wake keeps protection against automatic DPMS. */
    IdleBlack_Update(&view, FALSE, FALSE);
    CHECK(powerCalls == 2 && powerFlags == ES_CONTINUOUS);
}

static void TestSourceScopeAndFailures(void)
{
    Reset();
    MonitorList view = MakeView();
    view.monitors[1].controllable = FALSE; /* A black cover requires no DDC brightness support. */
    IdleBlack_Update(&view, TRUE, TRUE);
    view.monitors[1].currentInput = 0x12;
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(!IdleBlack_Active() && destroys == 1);
    view.monitors[1].sourceKnown = FALSE;
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(!IdleBlack_Active() && creates == 1);
    view.monitors[1].sourceKnown = TRUE;
    view.monitors[1].currentInput = 0x0F;
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(IdleBlack_Active() && creates == 2);
    view.monitors[1].excludedFromControl = TRUE;
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(!IdleBlack_Active() && powerCalls == 2);
    view.monitors[1].excludedFromControl = FALSE;
    inputOk = FALSE;
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(!IdleBlack_Active() && creates == 2);
    inputOk = TRUE;
    timerOk = FALSE;
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(!IdleBlack_Active());
    timerOk = TRUE;
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(IdleBlack_Active());
    IdleBlack_SetSessionLocked(TRUE);
    CHECK(!IdleBlack_Active() && !g_blackPreviousState);
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(!IdleBlack_Active() && !g_blackPreviousState);
    IdleBlack_SetSessionLocked(FALSE);
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(IdleBlack_Active());
    BlackWndProc(g_blackWindows[0].window, WM_LBUTTONDOWN, 0, 0);
    CHECK(!IdleBlack_Active() && wakes == 1);
    IdleBlack_Shutdown();
    CHECK(!g_blackPreviousState);
}

static void TestPreserveExistingPowerRequest(void)
{
    Reset();
    mockPowerState = ES_CONTINUOUS | ES_SYSTEM_REQUIRED;
    MonitorList view = MakeView();
    IdleBlack_Update(&view, TRUE, FALSE);
    CHECK(mockPowerState == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED));
    IdleBlack_Update(&view, FALSE, FALSE);
    CHECK(mockPowerState == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED));
}

static void TestPowerAcquisitionFailureAndRetry(void)
{
    Reset();
    MonitorList view = MakeView();
    CHECK(!IdleBlack_HoldsDisplayRequest());
    powerFailuresRemaining = 1;
    IdleBlack_Update(&view, TRUE, FALSE);
    CHECK(!IdleBlack_HoldsDisplayRequest() && powerCalls == 1);
    CHECK(mockPowerState == ES_CONTINUOUS);
    IdleBlack_Update(&view, TRUE, FALSE);
    CHECK(IdleBlack_HoldsDisplayRequest() && powerCalls == 2);
    CHECK(mockPowerState == (ES_CONTINUOUS | ES_DISPLAY_REQUIRED));
    IdleBlack_Update(&view, FALSE, FALSE);
    CHECK(!IdleBlack_HoldsDisplayRequest() && mockPowerState == ES_CONTINUOUS);
}

static void TestPowerReleaseFailureAndRetry(void)
{
    Reset();
    MonitorList view = MakeView();
    mockPowerState = ES_CONTINUOUS | ES_SYSTEM_REQUIRED;
    IdleBlack_Update(&view, TRUE, FALSE);
    CHECK(IdleBlack_HoldsDisplayRequest());
    CHECK(mockPowerState == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED));
    powerFailuresRemaining = 1;
    IdleBlack_Update(&view, FALSE, FALSE);
    CHECK(IdleBlack_HoldsDisplayRequest());
    CHECK(mockPowerState == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED));
    IdleBlack_Update(&view, FALSE, FALSE);
    CHECK(!IdleBlack_HoldsDisplayRequest());
    CHECK(mockPowerState == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED));
}

static void TestPowerLockAndShutdownCleanup(void)
{
    Reset();
    MonitorList view = MakeView();
    mockPowerState = ES_CONTINUOUS | ES_SYSTEM_REQUIRED;
    IdleBlack_Update(&view, TRUE, TRUE);
    powerFailuresRemaining = 1;
    IdleBlack_SetSessionLocked(TRUE);
    CHECK(!IdleBlack_Active() && IdleBlack_HoldsDisplayRequest());
    IdleBlack_Update(&view, TRUE, TRUE); /* The locked session retries release. */
    CHECK(!IdleBlack_Active() && !IdleBlack_HoldsDisplayRequest());
    CHECK(mockPowerState == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED));
    IdleBlack_SetSessionLocked(FALSE);
    IdleBlack_Update(&view, TRUE, TRUE);
    CHECK(IdleBlack_Active() && IdleBlack_HoldsDisplayRequest());
    powerFailuresRemaining = 1;
    IdleBlack_Shutdown();
    CHECK(!IdleBlack_Active() && IdleBlack_HoldsDisplayRequest());
    IdleBlack_Shutdown();
    CHECK(!IdleBlack_HoldsDisplayRequest());
    CHECK(mockPowerState == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED));
}

static void TestDecisionInputBeforeCover(void)
{
    Reset();
    MonitorList view = MakeView();
    inputTick = 101; /* Input arrived after the main thread decided to dim. */
    IdleBlack_UpdateForInput(&view, TRUE, TRUE, 100);
    CHECK(!IdleBlack_Active() && creates == 0 && wakes == 1);
    CHECK(powerCalls == 1); /* A wake retains the existing OLED power policy. */

    IdleBlack_UpdateForInput(&view, TRUE, TRUE, inputTick);
    CHECK(IdleBlack_Active() && creates == 1 && g_blackLastInput == 101);
    IdleBlack_UpdateForInput(&view, TRUE, TRUE, inputTick);
    CHECK(IdleBlack_Active() && creates == 1 && wakes == 1);

    inputTick = 99; /* A changed retrograde input sample is activity too. */
    IdleBlack_UpdateForInput(&view, TRUE, TRUE, inputTick);
    CHECK(!IdleBlack_Active() && creates == 1 && destroys == 1 && wakes == 2);
    CHECK(g_blackLastInput == 101); /* Do not rebase an existing idle stretch. */
}

static void TestDecisionInputAfterValidation(void)
{
    Reset();
    MonitorList view = MakeView();
    inputDuringCreate = TRUE;
    IdleBlack_UpdateForInput(&view, TRUE, TRUE, inputTick);
    CHECK(IdleBlack_Active() && inputTick == 101 && g_blackLastInput == 100);
    BlackWndProc(g_blackWindows[0].window, WM_TIMER, BLACK_INPUT_TIMER, 0);
    CHECK(!IdleBlack_Active() && destroys == 1 && wakes == 1);
}

static void TestDecisionInputFailure(void)
{
    Reset();
    MonitorList view = MakeView();
    inputOk = FALSE;
    IdleBlack_UpdateForInput(&view, TRUE, TRUE, inputTick);
    CHECK(!IdleBlack_Active() && creates == 0 && wakes == 1);
    inputOk = TRUE;
    IdleBlack_UpdateForInput(&view, TRUE, TRUE, inputTick);
    CHECK(IdleBlack_Active() && creates == 1);
    inputOk = FALSE;
    IdleBlack_UpdateForInput(&view, TRUE, TRUE, inputTick);
    CHECK(!IdleBlack_Active() && destroys == 1 && wakes == 2);
    IdleBlack_UpdateForInput(&view, TRUE, FALSE, inputTick);
    CHECK(wakes == 2); /* Non-idle updates need no input validation. */
}

static void TestActualBlackPixels(void)
{
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 8;
    info.bmiHeader.biHeight = -8;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *pixels = NULL;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    CHECK(dc && bitmap && pixels);
    if (dc && bitmap && pixels) {
        HGDIOBJ previous = SelectObject(dc, bitmap);
        memset(pixels, 0xFF, 8 * 8 * 4);
        BlackWndProc((HWND)(UINT_PTR)1, WM_ERASEBKGND, (WPARAM)dc, 0);
        GdiFlush();
        DWORD *rgb = pixels;
        for (int i = 0; i < 64; i++) CHECK((rgb[i] & 0xFFFFFF) == 0);
        SelectObject(dc, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
}

int main(void)
{
    TestCoverageAndWake();
    TestExternalFullscreenExcludesOwnCoverAndDesktop();
    TestFullscreenProbeRestoresDpiContext();
    TestSourceScopeAndFailures();
    TestPreserveExistingPowerRequest();
    TestPowerAcquisitionFailureAndRetry();
    TestPowerReleaseFailureAndRetry();
    TestPowerLockAndShutdownCleanup();
    TestDecisionInputBeforeCover();
    TestDecisionInputAfterValidation();
    TestDecisionInputFailure();
    TestActualBlackPixels();
    IdleBlack_Shutdown();
    if (failures) return 1;
    puts("ALL PASS: opaque black pixels, monitor bounds, source/scope, decision-input races, input wake, lock and power cleanup (mocked windows)");
    return 0;
}
