/* Exercise the real popup renderer on offscreen DIBs and simulate mouse
   messages. All window, capture and monitor operations are mocked:
   this test creates no window and cannot change brightness or the registry. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ui.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static const HWND testWindow = (HWND)(UINT_PTR)1;
static HWND testCapture;
static LONG_PTR testUserData;
static BOOL testVisible = TRUE;
static int commits, setCalls, allCalls, refreshCalls, manualCalls, deltaSaves;
static int lastRow, lastTarget;
static BOOL failNextDC, failNextBrush, failNextCommit;
static BOOL sawAllLabel, sawSelectedLabel, sawExcludedLabel;
static MonitorList testMonitors;

static BOOL WINAPI MockUpdateLayeredWindow(HWND, HDC, const POINT *, const SIZE *,
                                          HDC, const POINT *, COLORREF,
                                          const BLENDFUNCTION *, DWORD);
static LONG_PTR WINAPI MockGetWindowLongPtrW(HWND, int);
static LONG_PTR WINAPI MockSetWindowLongPtrW(HWND, int, LONG_PTR);
static HWND WINAPI MockGetCapture(void);
static HWND WINAPI MockSetCapture(HWND);
static BOOL WINAPI MockReleaseCapture(void);
static BOOL WINAPI MockShowWindow(HWND, int);
static BOOL WINAPI MockIsWindowVisible(HWND);
static HDC WINAPI MockCreateCompatibleDC(HDC);
static HBRUSH WINAPI MockCreateSolidBrush(COLORREF);
static BOOL WINAPI MockDeleteDC(HDC);
static BOOL WINAPI MockDeleteObject(HGDIOBJ);
static int WINAPI MockDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);

#undef GetWindowLongPtrW
#undef SetWindowLongPtrW
#define UpdateLayeredWindow MockUpdateLayeredWindow
#define GetWindowLongPtrW MockGetWindowLongPtrW
#define SetWindowLongPtrW MockSetWindowLongPtrW
#define GetCapture MockGetCapture
#define SetCapture MockSetCapture
#define ReleaseCapture MockReleaseCapture
#define ShowWindow MockShowWindow
#define IsWindowVisible MockIsWindowVisible
#define CreateCompatibleDC MockCreateCompatibleDC
#define CreateSolidBrush MockCreateSolidBrush
#define DeleteDC MockDeleteDC
#define DeleteObject MockDeleteObject
#define DrawTextW MockDrawTextW
#include "../ui_graphics.c"
#include "../ui_popup.c"
#undef UpdateLayeredWindow
#undef GetWindowLongPtrW
#undef SetWindowLongPtrW
#undef GetCapture
#undef SetCapture
#undef ReleaseCapture
#undef ShowWindow
#undef IsWindowVisible
#undef CreateCompatibleDC
#undef CreateSolidBrush
#undef DeleteDC
#undef DeleteObject
#undef DrawTextW

static BOOL WINAPI MockUpdateLayeredWindow(HWND hwnd, HDC destination,
                                          const POINT *position, const SIZE *size,
                                          HDC source, const POINT *origin,
                                          COLORREF key, const BLENDFUNCTION *blend,
                                          DWORD flags)
{
    (void)destination; (void)position; (void)key;
    CHECK(hwnd == testWindow && source == g_popupRender.dc);
    CHECK(size->cx == POPUP_WIDTH && size->cy == g_popupRender.height);
    CHECK(origin->x == 0 && origin->y == 0);
    CHECK(blend->AlphaFormat == AC_SRC_ALPHA && flags == ULW_ALPHA);
    commits++;
    if (failNextCommit) { failNextCommit = FALSE; return FALSE; }
    return TRUE;
}

static LONG_PTR WINAPI MockGetWindowLongPtrW(HWND hwnd, int index)
{
    CHECK(hwnd == testWindow && index == GWLP_USERDATA);
    return testUserData;
}

static LONG_PTR WINAPI MockSetWindowLongPtrW(HWND hwnd, int index, LONG_PTR value)
{
    LONG_PTR old = testUserData;
    CHECK(hwnd == testWindow && index == GWLP_USERDATA);
    testUserData = value;
    return old;
}

static HWND WINAPI MockGetCapture(void) { return testCapture; }
static HWND WINAPI MockSetCapture(HWND hwnd)
{
    HWND old = testCapture;
    CHECK(hwnd == testWindow);
    testCapture = hwnd;
    return old;
}
static BOOL WINAPI MockReleaseCapture(void)
{
    HWND old = testCapture;
    testCapture = NULL;
    if (old) PopupWndProc(old, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}
static BOOL WINAPI MockShowWindow(HWND hwnd, int command)
{
    CHECK(hwnd == testWindow);
    testVisible = command != SW_HIDE;
    return TRUE;
}
static BOOL WINAPI MockIsWindowVisible(HWND hwnd)
{
    CHECK(hwnd == testWindow);
    return testVisible;
}
static HDC WINAPI MockCreateCompatibleDC(HDC dc)
{
    if (failNextDC) { failNextDC = FALSE; return NULL; }
    return CreateCompatibleDC(dc);
}
static HBRUSH WINAPI MockCreateSolidBrush(COLORREF color)
{
    if (failNextBrush) { failNextBrush = FALSE; return NULL; }
    return CreateSolidBrush(color);
}
static BOOL WINAPI MockDeleteDC(HDC dc)
{
    BOOL result = DeleteDC(dc);
    CHECK(result);
    return result;
}
static BOOL WINAPI MockDeleteObject(HGDIOBJ object)
{
    BOOL result = DeleteObject(object);
    CHECK(result);
    return result;
}

static int WINAPI MockDrawTextW(HDC dc, LPCWSTR text, int length,
                               LPRECT rectangle, UINT format)
{
    if (length == -1) {
        if (wcscmp(text, L"All Monitors") == 0) sawAllLabel = TRUE;
        if (wcscmp(text, L"Selected Monitors") == 0) sawSelectedLabel = TRUE;
        if (wcsstr(text, L"(excluded)")) sawExcludedLabel = TRUE;
    }
    return DrawTextW(dc, text, length, rectangle, format);
}

BOOL Monitor_SetBrightness(BrightMonitor *monitor, DWORD percent)
{
    CHECK(monitor >= testMonitors.monitors &&
          monitor < testMonitors.monitors + testMonitors.count);
    CHECK(percent <= 100);
    CHECK(Monitor_CanControl(monitor));
    setCalls++;
    monitor->brightnessCur = monitor->brightnessMin +
        ((monitor->brightnessMax - monitor->brightnessMin) * percent) / 100;
    return TRUE;
}

void Monitor_SetAllBrightness(MonitorList *view, int target)
{
    CHECK(view == &testMonitors);
    allCalls++;
    for (int i = 0; i < view->count; i++) {
        if (!Monitor_CanControl(&view->monitors[i])) continue;
        int percent = target + view->monitors[i].delta;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        Monitor_SetBrightness(&view->monitors[i], (DWORD)percent);
    }
}

void Monitor_RefreshBrightness(MonitorList *view)
{
    CHECK(view == &testMonitors);
    refreshCalls++;
}

static void ManualChange(int row, int target)
{
    manualCalls++;
    lastRow = row;
    lastTarget = target;
    /* Match the application callback's authoritative master bookkeeping. */
    UI_SetMasterTarget(row == -1 ? target :
                       (int)testMonitors.monitors[row].brightnessCur -
                       testMonitors.monitors[row].delta);
}

static void SaveDelta(void) { deltaSaves++; }

static void ResetPopup(int count)
{
    ReleasePopupRenderCache();
    ZeroMemory(&testMonitors, sizeof(testMonitors));
    testMonitors.count = count;
    for (int i = 0; i < count; i++) {
        BrightMonitor *monitor = &testMonitors.monitors[i];
        monitor->brightnessMax = 100;
        monitor->brightnessCur = 50;
        monitor->controllable = TRUE;
        monitor->backend = BACKEND_DDC;
        wsprintfW(monitor->name, L"Simulated display %d", i + 1);
    }
    ZeroMemory(&g_popupData, sizeof(g_popupData));
    g_popupData.ml = &testMonitors;
    g_popupData.activeSlider = -1;
    g_popupData.dragPercent = -1;
    g_masterTargetKnown = FALSE;
    g_popupData.masterPercent = GetMasterPercent(&testMonitors);
    testUserData = (LONG_PTR)&g_popupData;
    testCapture = NULL;
    testVisible = TRUE;
    commits = setCalls = allCalls = refreshCalls = manualCalls = deltaSaves = 0;
    sawAllLabel = sawSelectedLabel = sawExcludedLabel = FALSE;
    failNextDC = failNextBrush = failNextCommit = FALSE;
    UI_SetManualChangeCallback(ManualChange);
    UI_SetDeltaSaveCallback(SaveDelta);
}

static LPARAM SliderPosition(int row, int percent)
{
    RECT slider;
    GetSliderRect(row, &slider);
    return MAKELPARAM(XFromPercent(&slider, percent),
                     (slider.top + slider.bottom) / 2);
}

static DWORD GdiObjectCount(void)
{
    /* DeleteObject may be batched; count only after those calls reach GDI. */
    GdiFlush();
    return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

static void TestRenderCache(void)
{
    ResetPopup(1);
    /* Warm the GDI/font engine's process caches before measuring lifecycle
       leaks. Deleted handles may remain cached by Windows for later reuse. */
    RenderPopup(testWindow, &g_popupData);
    ReleasePopupRenderCache();
    DWORD baseline = GdiObjectCount();
    commits = 0;
    RenderPopup(testWindow, &g_popupData);
    CHECK(commits == 1 && g_popupRender.frameValid);
    CHECK(g_popupRender.bits && g_popupRender.alpha);
    HDC dc = g_popupRender.dc;
    HBITMAP bitmap = g_popupRender.bitmap;
    HFONT font = g_popupRender.font;
    HBRUSH brush = g_popupRender.accent;
    DWORD cachedObjects = GdiObjectCount();
    RenderPopup(testWindow, &g_popupData);
    CHECK(commits == 1);
    for (int i = 0; i < 200; i++) {
        testMonitors.monitors[0].brightnessCur = (DWORD)(i % 101);
        g_popupData.masterPercent = GetMasterPercent(&testMonitors);
        RenderPopup(testWindow, &g_popupData);
        CHECK(g_popupRender.dc == dc && g_popupRender.bitmap == bitmap);
        CHECK(g_popupRender.font == font && g_popupRender.accent == brush);
    }
    CHECK(GdiObjectCount() == cachedObjects);

    testMonitors.monitors[0].brightnessCur = 73;
    g_popupData.masterPercent = 73;
    RenderPopup(testWindow, &g_popupData);
    size_t byteCount = (size_t)g_popupRender.width * g_popupRender.height * 4;
    BYTE *reusedPixels = (BYTE *)malloc(byteCount);
    CHECK(reusedPixels != NULL);
    if (reusedPixels) {
        memcpy(reusedPixels, g_popupRender.bits, byteCount);
        ReleasePopupRenderCache();
        RenderPopup(testWindow, &g_popupData);
        CHECK(memcmp(reusedPixels, g_popupRender.bits, byteCount) == 0);
        free(reusedPixels);
    }

    /* Retain the previous surface if allocating its replacement fails. */
    dc = g_popupRender.dc;
    bitmap = g_popupRender.bitmap;
    testMonitors.count = 2;
    testMonitors.monitors[1] = testMonitors.monitors[0];
    wcscpy(testMonitors.monitors[1].name, L"Second display");
    int oldCommits = commits;
    failNextDC = TRUE;
    RenderPopup(testWindow, &g_popupData);
    CHECK(commits == oldCommits && g_popupRender.dc == dc);
    CHECK(g_popupRender.bitmap == bitmap);
    RenderPopup(testWindow, &g_popupData);
    CHECK(commits == oldCommits + 1 && g_popupRender.dc != dc);
    CHECK(g_popupRender.bitmap != bitmap && g_popupRender.frame.count == 2);
    ReleasePopupRenderCache();
    printf("GDI object counts: baseline=%lu, cached=%lu, after cleanup=%lu\n",
           (unsigned long)baseline, (unsigned long)cachedObjects,
           (unsigned long)GdiObjectCount());
    CHECK(GdiObjectCount() == baseline);
    failNextBrush = TRUE;
    RenderPopup(testWindow, &g_popupData);
    CHECK(g_popupRender.dc == NULL && g_popupRender.font == NULL);
    CHECK(GdiObjectCount() == baseline);
    RenderPopup(testWindow, &g_popupData);
    CHECK(g_popupRender.frameValid);
    testMonitors.monitors[0].brightnessCur++;
    failNextCommit = TRUE;
    RenderPopup(testWindow, &g_popupData);
    CHECK(!g_popupRender.frameValid);
    oldCommits = commits;
    RenderPopup(testWindow, &g_popupData);
    CHECK(commits == oldCommits + 1 && g_popupRender.frameValid);
    ReleasePopupRenderCache();
    CHECK(GdiObjectCount() == baseline);
    for (int i = 0; i < 10; i++) {
        RenderPopup(testWindow, &g_popupData);
        CHECK(GdiObjectCount() == cachedObjects);
        ReleasePopupRenderCache();
        CHECK(GdiObjectCount() == baseline);
    }
    puts("PASS popup cache, pixel reuse, resize, allocation and commit failures");
}

static void TestDragFinalValue(void)
{
    ResetPopup(1);
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(0, 0));
    CHECK(g_popupData.activeSlider == 0 && testCapture == testWindow);
    CHECK(lastRow == 0 && lastTarget == 0);
    int oldSets = setCalls, oldCommits = commits;
    PopupWndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, SliderPosition(0, 0));
    CHECK(setCalls == oldSets && commits == oldCommits);
    PopupWndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, SliderPosition(0, 40));
    /* The release position, even without a final mousemove, must be applied. */
    PopupWndProc(testWindow, WM_LBUTTONUP, 0, SliderPosition(0, 100));
    CHECK(lastRow == 0 && lastTarget == 100);
    CHECK(testMonitors.monitors[0].brightnessCur == 100);
    CHECK(g_popupData.activeSlider == -1 && g_popupData.dragPercent == -1);
    CHECK(testCapture == NULL && refreshCalls == 1);
    CHECK(manualCalls == 3); /* down, changed move, exact release */

    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(0, 100));
    /* Signed coordinates left of the window must clamp to 0, not wrap to 100. */
    PopupWndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-20, 68));
    CHECK(lastTarget == 0 && g_popupData.dragPercent == 0);
    PopupWndProc(testWindow, WM_CANCELMODE, 0, 0);
    CHECK(g_popupData.activeSlider == -1 && testCapture == NULL);

    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(0, 0));
    PopupWndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, SliderPosition(0, 100));
    testCapture = NULL; /* The operating system already transferred capture. */
    PopupWndProc(testWindow, WM_CAPTURECHANGED, 0, (LPARAM)(UINT_PTR)2);
    CHECK(lastTarget == 100 && g_popupData.activeSlider == -1);
    CHECK(g_popupData.dragPercent == -1);

    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(0, 100));
    UI_HidePopup(testWindow);
    CHECK(lastTarget == 100 && g_popupData.activeSlider == -1);
    CHECK(testCapture == NULL && !testVisible);

    testVisible = TRUE;
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(0, 100));
    PopupWndProc(testWindow, WM_MOUSEMOVE, 0, SliderPosition(0, 100));
    CHECK(lastTarget == 100 && g_popupData.activeSlider == -1);
    CHECK(testCapture == NULL);
    PopupWndProc(testWindow, WM_DESTROY, 0, 0);
    CHECK(g_popupRender.dc == NULL);
    puts("PASS drag exact release, signed coordinates, capture loss, cancel and hide");
}

static void TestMasterTarget(void)
{
    ResetPopup(1);
    testMonitors.monitors[0].delta = 40;
    CHECK(!g_masterTargetKnown);
    CHECK(GetMasterPercent(&testMonitors) == 35); /* infer (50 - 40) */
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(1, 100));
    PopupWndProc(testWindow, WM_LBUTTONUP, 0, SliderPosition(1, 100));
    CHECK(lastRow == -1 && lastTarget == 100);
    CHECK(testMonitors.monitors[0].brightnessCur == 100);
    UI_RefreshPopup(testWindow, &testMonitors);
    CHECK(g_popupData.masterPercent == 100 && GetMasterPercent(&testMonitors) == 100);

    UI_SetMasterTarget(-40);
    CHECK(!g_popupRender.frameValid);
    UI_RefreshPopup(testWindow, &testMonitors);
    CHECK(g_popupData.masterPercent == 0);
    CHECK(g_popupRender.frame.percent[1] == 0);
    int oldCommits = commits;
    UI_SetMasterTarget(-40);
    UI_RefreshPopup(testWindow, &testMonitors);
    CHECK(commits == oldCommits);
    ReleasePopupRenderCache();
    puts("PASS master target stays authoritative with delta and clamping");
}

static LPARAM DeltaPosition(int row, BOOL plus)
{
    RECT minus, value, positive;
    GetDeltaButtonRects(row, &minus, &value, &positive);
    RECT button = plus ? positive : minus;
    return MAKELPARAM((button.left + button.right) / 2,
                     (button.top + button.bottom) / 2);
}

static void TestMonitorSelection(void)
{
    ResetPopup(2);
    RenderPopup(testWindow, &g_popupData);
    CHECK(sawAllLabel && !sawSelectedLabel && !sawExcludedLabel);
    DWORD objects = GdiObjectCount();
    int oldCommits = commits;
    testMonitors.selectedOnly = TRUE;
    testMonitors.monitors[1].excludedFromControl = TRUE;
    UI_RefreshPopup(testWindow, &testMonitors);
    CHECK(commits == oldCommits + 1 && GdiObjectCount() == objects);
    CHECK(sawSelectedLabel && sawExcludedLabel);
    CHECK(g_popupRender.frame.selectedOnly);
    CHECK(g_popupRender.frame.enabled[0] && !g_popupRender.frame.enabled[1]);
    CHECK(g_popupRender.frame.enabled[2] && g_popupRender.frame.excluded[1]);

    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(1, 100));
    CHECK(g_popupData.activeSlider == -1 && testCapture == NULL);
    CHECK(setCalls == 0 && allCalls == 0 && manualCalls == 0);
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, DeltaPosition(1, TRUE));
    CHECK(testMonitors.monitors[1].delta == 0 && deltaSaves == 0);
    ApplySliderValue(&g_popupData, 1, 0); /* Direct callers are guarded as well. */
    CHECK(setCalls == 0 && manualCalls == 0 && refreshCalls == 0);

    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(2, 100));
    PopupWndProc(testWindow, WM_LBUTTONUP, 0, SliderPosition(2, 100));
    CHECK(lastRow == -1 && lastTarget == 100 && allCalls == 2 && setCalls == 2);
    CHECK(testMonitors.monitors[0].brightnessCur == 100);
    CHECK(testMonitors.monitors[1].brightnessCur == 50 && manualCalls == 2);
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, DeltaPosition(0, TRUE));
    CHECK(testMonitors.monitors[0].delta == 1 && deltaSaves == 1);
    CHECK(manualCalls == 2); /* Calibration retains its existing callback policy. */

    testMonitors.monitors[1].excludedFromControl = FALSE;
    UI_RefreshPopup(testWindow, &testMonitors);
    CHECK(g_popupRender.frame.enabled[1] && g_popupRender.frame.selectedOnly);
    sawAllLabel = FALSE;
    oldCommits = commits;
    testMonitors.selectedOnly = FALSE;
    UI_RefreshPopup(testWindow, &testMonitors);
    CHECK(commits == oldCommits + 1 && sawAllLabel);
    ReleasePopupRenderCache();
    puts("PASS selected popup labels, disabled exclusions and selected master writes");
}

static void TestNoEligibleControls(void)
{
    ResetPopup(2);
    testMonitors.selectedOnly = TRUE;
    testMonitors.monitors[0].excludedFromControl = TRUE;
    testMonitors.monitors[1].excludedFromControl = TRUE;
    UI_RefreshPopup(testWindow, &testMonitors);
    CHECK(!g_popupRender.frame.enabled[0] && !g_popupRender.frame.enabled[1]);
    CHECK(!g_popupRender.frame.enabled[2]);
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(2, 100));
    ApplySliderValue(&g_popupData, 2, 100);
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, DeltaPosition(0, FALSE));
    CHECK(g_popupData.activeSlider == -1 && testCapture == NULL);
    CHECK(setCalls == 0 && allCalls == 0 && manualCalls == 0 && deltaSaves == 0);

    ResetPopup(1);
    testMonitors.monitors[0].controllable = FALSE;
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(0, 100));
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(1, 100));
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, DeltaPosition(0, TRUE));
    CHECK(setCalls == 0 && allCalls == 0 && manualCalls == 0 && deltaSaves == 0);

    ResetPopup(0);
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(0, 100));
    CHECK(setCalls == 0 && allCalls == 0 && manualCalls == 0 && testCapture == NULL);

    ResetPopup(1);
    PopupWndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, SliderPosition(0, 0));
    int oldSets = setCalls, oldManual = manualCalls;
    testMonitors.monitors[0].excludedFromControl = TRUE;
    PopupWndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, SliderPosition(0, 100));
    PopupWndProc(testWindow, WM_LBUTTONUP, 0, SliderPosition(0, 100));
    CHECK(setCalls == oldSets && manualCalls == oldManual);
    CHECK(g_popupData.activeSlider == -1 && testCapture == NULL);
    ReleasePopupRenderCache();
    puts("PASS no eligible monitor means no slider/delta requests or manual callbacks");
}

int main(void)
{
    TestRenderCache();
    TestDragFinalValue();
    TestMasterTarget();
    TestMonitorSelection();
    TestNoEligibleControls();
    ReleasePopupRenderCache();
    printf("UI tests: %s\n", failures ? "FAIL" : "ALL PASS");
    return failures ? 1 : 0;
}
