#include "idle_black.h"
#include "brightness.h"

#define BLACK_INPUT_TIMER 1
#define BLACK_INPUT_POLL_MS 100

typedef struct {
    HWND window;
    HMONITOR monitor;
    RECT bounds;
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

void IdleBlack_Clear(void)
{
    for (int i = 0; i < MAX_MONITORS; i++) {
        if (g_blackWindows[i].window) DestroyWindow(g_blackWindows[i].window);
        ZeroMemory(&g_blackWindows[i], sizeof(g_blackWindows[i]));
    }
}

static void WakeFromBlack(void)
{
    if (!IdleBlack_Active()) return;
    IdleBlack_Clear();
    PostMessageW(g_blackOwner, WM_IDLE_BLACK_WAKE, 0, 0);
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
        WakeFromBlack();
        return 0;
    case WM_TIMER: {
        LASTINPUTINFO input = { sizeof(input), 0 };
        if (wp == BLACK_INPUT_TIMER &&
            (!GetLastInputInfo(&input) || input.dwTime != g_blackLastInput))
            WakeFromBlack();
        return 0;
    }
    case WM_CLOSE:
        WakeFromBlack();
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
}

/* Keep physical monitor coordinates consistent on mixed-DPI desktops without
   changing the awareness of Lumos's other windows. Available since Win10 1607. */
typedef HANDLE (WINAPI *SetDpiContextProc)(HANDLE);

void IdleBlack_Update(const MonitorList *view, BOOL enabled, BOOL idle)
{
    enabled = enabled && !g_blackSessionLocked;
    HMONITOR desired[MAX_MONITORS];
    int count = 0;
    BOOL keepDisplay = FALSE;
    if (view && enabled) {
        for (int i = 0; i < view->count; i++) {
            const BrightMonitor *monitor = &view->monitors[i];
            if (monitor->excludedFromControl || !monitor->idleBlack || !monitor->hMonitor) continue;
            keepDisplay = TRUE;
            if (!idle || !Monitor_SourceAllowsControl(monitor)) continue;
            BOOL duplicate = FALSE;
            for (int j = 0; j < count; j++) if (desired[j] == monitor->hMonitor) duplicate = TRUE;
            if (!duplicate) desired[count++] = monitor->hMonitor;
        }
    }
    /* A display-only request avoids automatic DPMS, even if Windows's timeout
       is shorter than Lumos's idle timeout. It does not prohibit system sleep. */
    if (keepDisplay && !g_blackPreviousState) {
        g_blackPreviousState = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
        if (g_blackPreviousState & ~ES_CONTINUOUS)
            SetThreadExecutionState(g_blackPreviousState | ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    } else if (!keepDisplay && g_blackPreviousState) {
        SetThreadExecutionState(g_blackPreviousState | ES_CONTINUOUS);
        g_blackPreviousState = 0;
    }

    for (int i = 0; i < MAX_MONITORS; i++) {
        BlackWindow *black = &g_blackWindows[i];
        if (!black->window) continue;
        BOOL retain = FALSE;
        for (int j = 0; j < count; j++) if (desired[j] == black->monitor) retain = TRUE;
        if (!retain) {
            DestroyWindow(black->window);
            ZeroMemory(black, sizeof(*black));
        }
    }
    if (!count || !g_blackRegistered) return;

    union { FARPROC generic; SetDpiContextProc setDpi; } dpi;
    dpi.generic = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    SetDpiContextProc setDpi = dpi.setDpi;
    HANDLE oldDpi = setDpi ? setDpi((HANDLE)(LONG_PTR)-4) : NULL;
    if (setDpi && !oldDpi) oldDpi = setDpi((HANDLE)(LONG_PTR)-3);
    BOOL first = !IdleBlack_Active();
    LASTINPUTINFO input = { sizeof(input), 0 };
    /* If the input query fails, leave the desktop visible. */
    if (first && !GetLastInputInfo(&input)) {
        if (oldDpi) setDpi(oldDpi);
        return;
    }
    if (first) g_blackLastInput = input.dwTime;
    for (int j = 0; j < count; j++) {
        MONITORINFO info = {0};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(desired[j], &info)) continue;
        int index = -1;
        for (int i = 0; i < MAX_MONITORS; i++)
            if (g_blackWindows[i].window && g_blackWindows[i].monitor == desired[j]) index = i;
        if (index < 0) {
            for (int i = 0; i < MAX_MONITORS; i++)
                if (!g_blackWindows[i].window) { index = i; break; }
        }
        if (index < 0) continue;
        BlackWindow *black = &g_blackWindows[index];
        int width = info.rcMonitor.right - info.rcMonitor.left;
        int height = info.rcMonitor.bottom - info.rcMonitor.top;
        if (!black->window) {
            black->window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                BLACK_CLASS, L"", WS_POPUP, info.rcMonitor.left, info.rcMonitor.top, width, height,
                g_blackOwner, NULL, g_blackInstance, NULL);
            if (!black->window) continue;
            black->monitor = desired[j];
            if (!SetTimer(black->window, BLACK_INPUT_TIMER, BLACK_INPUT_POLL_MS, NULL)) {
                DestroyWindow(black->window);
                ZeroMemory(black, sizeof(*black));
                continue;
            }
        }
        black->bounds = info.rcMonitor;
        SetWindowPos(black->window, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.top,
                     width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (oldDpi) setDpi(oldDpi);
}

void IdleBlack_Shutdown(void)
{
    IdleBlack_Clear();
    if (g_blackPreviousState) SetThreadExecutionState(g_blackPreviousState | ES_CONTINUOUS);
    g_blackPreviousState = 0;
    if (g_blackRegistered) UnregisterClassW(BLACK_CLASS, g_blackInstance);
    g_blackRegistered = FALSE;
    g_blackSessionLocked = FALSE;
}

void IdleBlack_SetSessionLocked(BOOL locked)
{
    g_blackSessionLocked = locked;
    if (locked) IdleBlack_Update(NULL, FALSE, FALSE);
}
