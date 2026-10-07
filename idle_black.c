#include "idle_black.h"
#include "brightness.h"
#include "diagnostics.h"
#include <dwmapi.h>

#define BLACK_INPUT_TIMER 1
#define BLACK_INPUT_POLL_MS 100

/* Keep monitor/client coordinates physical on mixed-DPI desktops without
   changing the awareness of Lumos's other windows. Available since Win10 1607. */
typedef HANDLE (WINAPI *SetDpiContextProc)(HANDLE);

static SetDpiContextProc EnterPhysicalDpiContext(HANDLE *previous)
{
    union { FARPROC generic; SetDpiContextProc setDpi; } dpi;
    dpi.generic = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    SetDpiContextProc setDpi = dpi.setDpi;
    *previous = setDpi ? setDpi((HANDLE)(LONG_PTR)-4) : NULL;
    if (setDpi && !*previous) *previous = setDpi((HANDLE)(LONG_PTR)-3);
    return setDpi;
}

typedef struct {
    HWND window;
    HMONITOR monitor;
    RECT bounds;
    BrightMonitor identity;
} BlackWindow;

static const WCHAR BLACK_CLASS[] = L"LumosIdleBlack";
static HINSTANCE g_blackInstance;
static HWND g_blackOwner;
static BOOL g_blackRegistered;
static BlackWindow g_blackWindows[MAX_MONITORS];
static DWORD g_blackLastInput;
static EXECUTION_STATE g_blackPreviousState;
static BOOL g_blackSessionLocked;

BOOL IdleBlack_Active(void)
{
    for (int i = 0; i < MAX_MONITORS; i++)
        if (g_blackWindows[i].window) return TRUE;
    return FALSE;
}

static BOOL CALLBACK FindExternalFullscreen(HWND window, LPARAM parameter)
{
    for (int i = 0; i < MAX_MONITORS; i++)
        if (g_blackWindows[i].window == window) return TRUE;
    if (!IsWindowVisible(window) || IsIconic(window)) return TRUE;
    LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    LONG_PTR frame = GetWindowLongPtrW(window, GWL_STYLE);
    if (IsZoomed(window) && (frame & WS_CAPTION) == WS_CAPTION) return TRUE;
    WCHAR className[64] = {0};
    GetClassNameW(window, className, ARRAYSIZE(className));
    if (!wcscmp(className, L"Progman") || !wcscmp(className, L"WorkerW")) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE; /* Includes applications on another virtual desktop. */

    RECT client;
    if (!GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top)
        return TRUE;
    POINT first = {client.left, client.top}, last = {client.right, client.bottom};
    if (!ClientToScreen(window, &first) || !ClientToScreen(window, &last)) return TRUE;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info = {0};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return TRUE;
    /* Client bounds distinguish fullscreen content from a maximized window's
       invisible frame, which can extend beyond the work area. */
    if (first.x > info.rcMonitor.left || first.y > info.rcMonitor.top ||
        last.x < info.rcMonitor.right || last.y < info.rcMonitor.bottom) return TRUE;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    Diagnostics_Log("INFO", "idle", "external fullscreen window=%p pid=%lu", (void *)window, process);
    *(BOOL *)parameter = TRUE;
    return FALSE;
}

BOOL IdleBlack_HasExternalFullscreen(void)
{
    HANDLE previous;
    SetDpiContextProc setDpi = EnterPhysicalDpiContext(&previous);
    BOOL found = FALSE;
    BOOL enumerated = EnumWindows(FindExternalFullscreen, (LPARAM)&found);
    DWORD error = !enumerated && !found ? GetLastError() : ERROR_SUCCESS;
    if (previous) setDpi(previous);
    if (!enumerated && !found) {
        Diagnostics_Log("WARN", "idle", "fullscreen window enumeration FAILED error=0x%08lX", error);
        return TRUE; /* Keep the shell exclusion if we cannot check its owner. */
    }
    return found;
}

BOOL IdleBlack_HoldsDisplayRequest(void)
{
    return g_blackPreviousState != 0;
}

static void ReleaseDisplayRequest(void)
{
    if (!g_blackPreviousState) return;
    EXECUTION_STATE result = SetThreadExecutionState(g_blackPreviousState | ES_CONTINUOUS);
    Diagnostics_Log(result ? "INFO" : "ERROR", "power", "release-display-request state=0x%08lX error=0x%08lX",
                    g_blackPreviousState, result ? ERROR_SUCCESS : GetLastError());
    /* A failed call leaves our display request active. Retain the original
       state so the next disable, lock or shutdown can retry its release. */
    if (result) g_blackPreviousState = 0;
}

void IdleBlack_Clear(void)
{
    for (int i = 0; i < MAX_MONITORS; i++) {
        if (g_blackWindows[i].window) {
            Diagnostics_Monitor(&g_blackWindows[i].identity, "INFO", "overlay", "HIDE reason=clear-request window=%p",
                                (void *)g_blackWindows[i].window);
            if (!DestroyWindow(g_blackWindows[i].window))
                Diagnostics_Monitor(&g_blackWindows[i].identity, "ERROR", "overlay", "DestroyWindow FAILED error=0x%08lX", GetLastError());
        }
        ZeroMemory(&g_blackWindows[i], sizeof(g_blackWindows[i]));
    }
}

static void NotifyBlackWake(const char *reason)
{
    Diagnostics_Log("INFO", "overlay", "WAKE reason=%s", reason);
    IdleBlack_Clear();
    if (!PostMessageW(g_blackOwner, WM_IDLE_BLACK_WAKE, 0, 0))
        Diagnostics_Log("ERROR", "overlay", "wake notification FAILED error=0x%08lX", GetLastError());
}

static void WakeFromBlack(const char *reason)
{
    if (IdleBlack_Active()) NotifyBlackWake(reason);
}

static LRESULT CALLBACK BlackWndProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT bounds;
        GetClientRect(window, &bounds);
        FillRect((HDC)wp, &bounds, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        WakeFromBlack("mouse-button-or-wheel");
        return 0;
    case WM_TIMER: {
        LASTINPUTINFO input = { sizeof(input), 0 };
        if (wp == BLACK_INPUT_TIMER) {
            BOOL ok = GetLastInputInfo(&input);
            if (!ok) Diagnostics_Log("ERROR", "overlay", "GetLastInputInfo FAILED error=0x%08lX", GetLastError());
            if (!ok || input.dwTime != g_blackLastInput)
                WakeFromBlack(ok ? "last-input-changed" : "last-input-query-failed");
        }
        return 0;
    }
    case WM_CLOSE:
        WakeFromBlack("window-close");
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void IdleBlack_Init(HINSTANCE instance, HWND owner)
{
    g_blackInstance = instance;
    g_blackOwner = owner;
    WNDCLASSEXW window = {0};
    window.cbSize = sizeof(window);
    window.hInstance = instance;
    window.lpfnWndProc = BlackWndProc;
    window.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    window.lpszClassName = BLACK_CLASS;
    g_blackRegistered = RegisterClassExW(&window) != 0;
    Diagnostics_Log(g_blackRegistered ? "INFO" : "ERROR", "overlay", "initialize registered=%d error=0x%08lX",
                    g_blackRegistered, g_blackRegistered ? ERROR_SUCCESS : GetLastError());
}

static void UpdateBlack(const MonitorList *view, BOOL enabled, BOOL idle,
                        BOOL hasDecisionInput, DWORD decisionInput)
{
    enabled = enabled && !g_blackSessionLocked;
    HMONITOR desired[MAX_MONITORS];
    int count = 0;
    BOOL keepDisplay = FALSE;
    if (view && enabled) {
        for (int i = 0; i < view->count; i++) {
            const BrightMonitor *monitor = &view->monitors[i];
            if (!monitor->idleBlack) continue;
            if (monitor->excludedFromControl || !monitor->hMonitor) {
                Diagnostics_Monitor(monitor, "INFO", "overlay", "SKIP reason=%s",
                    monitor->excludedFromControl ? "monitor-excluded" : "no-Windows-display");
                continue;
            }
            keepDisplay = TRUE;
            if (!idle || !Monitor_SourceAllowsControl(monitor)) {
                if (idle) Diagnostics_Monitor(monitor, "INFO", "overlay", "SKIP reason=%s", Diagnostics_SourceReason(monitor));
                continue;
            }
            BOOL duplicate = FALSE;
            for (int j = 0; j < count; j++) if (desired[j] == monitor->hMonitor) duplicate = TRUE;
            if (!duplicate) desired[count++] = monitor->hMonitor;
        }
    }
    /* A display-only request avoids automatic DPMS, even if Windows's timeout
       is shorter than Lumos's idle timeout. It does not prohibit system sleep. */
    if (keepDisplay && !g_blackPreviousState) {
        g_blackPreviousState = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
        Diagnostics_Log(g_blackPreviousState ? "INFO" : "ERROR", "power",
            "keep-display-awake previousState=0x%08lX error=0x%08lX",
            g_blackPreviousState, g_blackPreviousState ? ERROR_SUCCESS : GetLastError());
        if (g_blackPreviousState & ~ES_CONTINUOUS)
            SetThreadExecutionState(g_blackPreviousState | ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    } else if (!keepDisplay && g_blackPreviousState) {
        ReleaseDisplayRequest();
    }

    for (int i = 0; i < MAX_MONITORS; i++) {
        BlackWindow *black = &g_blackWindows[i];
        if (!black->window) continue;
        BOOL retain = FALSE;
        for (int j = 0; j < count; j++) if (desired[j] == black->monitor) retain = TRUE;
        if (!retain) {
            Diagnostics_Monitor(&black->identity, "INFO", "overlay", "HIDE reason=no-longer-eligible idle=%d enabled=%d sessionLocked=%d",
                                idle, enabled, g_blackSessionLocked);
            if (!DestroyWindow(black->window))
                Diagnostics_Monitor(&black->identity, "ERROR", "overlay", "DestroyWindow FAILED error=0x%08lX", GetLastError());
            ZeroMemory(black, sizeof(*black));
        }
    }
    if (!count || !g_blackRegistered) return;

    BOOL first = !IdleBlack_Active();
    LASTINPUTINFO input = { sizeof(input), 0 };
    if (first || hasDecisionInput) {
        BOOL inputOk = GetLastInputInfo(&input);
        if (!inputOk)
            Diagnostics_Log("ERROR", "overlay", "last-input query FAILED error=0x%08lX", GetLastError());
        if (hasDecisionInput && (!inputOk || input.dwTime != decisionInput ||
                               (!first && input.dwTime != g_blackLastInput))) {
            /* The decision's baseline belongs to the entire idle stretch.
               A newer sample must wake existing covers, never rebase them. */
            NotifyBlackWake(inputOk ? "input-changed-before-cover" : "last-input-query-failed");
            return;
        }
        /* If the input query fails, leave the desktop visible. */
        if (!inputOk) return;
    }
    if (first) g_blackLastInput = hasDecisionInput ? decisionInput : input.dwTime;

    HANDLE oldDpi;
    SetDpiContextProc setDpi = EnterPhysicalDpiContext(&oldDpi);
    for (int j = 0; j < count; j++) {
        MONITORINFO info = {0};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(desired[j], &info)) {
            Diagnostics_Log("ERROR", "overlay", "GetMonitorInfo FAILED hMonitor=%p error=0x%08lX", (void *)desired[j], GetLastError());
            continue;
        }
        int index = -1;
        for (int i = 0; i < MAX_MONITORS; i++)
            if (g_blackWindows[i].window && g_blackWindows[i].monitor == desired[j]) index = i;
        if (index < 0) {
            for (int i = 0; i < MAX_MONITORS; i++)
                if (!g_blackWindows[i].window) { index = i; break; }
        }
        if (index < 0) continue;
        BlackWindow *black = &g_blackWindows[index];
        for (int i = 0; view && i < view->count; i++)
            if (view->monitors[i].hMonitor == desired[j] && view->monitors[i].idleBlack &&
                !view->monitors[i].excludedFromControl && Monitor_SourceAllowsControl(&view->monitors[i])) {
                black->identity = view->monitors[i];
                break;
            }
        int width = info.rcMonitor.right - info.rcMonitor.left;
        int height = info.rcMonitor.bottom - info.rcMonitor.top;
        if (!black->window) {
            black->window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                BLACK_CLASS, L"", WS_POPUP, info.rcMonitor.left, info.rcMonitor.top, width, height,
                g_blackOwner, NULL, g_blackInstance, NULL);
            if (!black->window) {
                Diagnostics_Monitor(&black->identity, "ERROR", "overlay", "CreateWindow FAILED error=0x%08lX", GetLastError());
                continue;
            }
            black->monitor = desired[j];
            if (!SetTimer(black->window, BLACK_INPUT_TIMER, BLACK_INPUT_POLL_MS, NULL)) {
                Diagnostics_Monitor(&black->identity, "ERROR", "overlay", "input timer FAILED error=0x%08lX", GetLastError());
                DestroyWindow(black->window);
                ZeroMemory(black, sizeof(*black));
                continue;
            }
            Diagnostics_Monitor(&black->identity, "INFO", "overlay", "SHOW true-black bounds=%ld,%ld,%ld,%ld window=%p inputPollMs=100",
                info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom, (void *)black->window);
        }
        black->bounds = info.rcMonitor;
        if (!SetWindowPos(black->window, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.top,
                         width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW))
            Diagnostics_Monitor(&black->identity, "ERROR", "overlay", "SetWindowPos FAILED error=0x%08lX", GetLastError());
    }
    if (oldDpi) setDpi(oldDpi);
}

void IdleBlack_Update(const MonitorList *view, BOOL enabled, BOOL idle)
{
    UpdateBlack(view, enabled, idle, FALSE, 0);
}

void IdleBlack_UpdateForInput(const MonitorList *view, BOOL enabled, BOOL idle,
                             DWORD decisionInput)
{
    UpdateBlack(view, enabled, idle, TRUE, decisionInput);
}

void IdleBlack_Shutdown(void)
{
    Diagnostics_Log("INFO", "overlay", "shutdown");
    IdleBlack_Clear();
    ReleaseDisplayRequest();
    if (g_blackRegistered) UnregisterClassW(BLACK_CLASS, g_blackInstance);
    g_blackRegistered = FALSE;
    g_blackSessionLocked = FALSE;
}

void IdleBlack_SetSessionLocked(BOOL locked)
{
    Diagnostics_Log("INFO", "overlay", "session locked=%d", locked);
    g_blackSessionLocked = locked;
    if (locked) IdleBlack_Update(NULL, FALSE, FALSE);
}
