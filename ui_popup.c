#include "ui_internal.h"
#include <shellapi.h>

static const WCHAR POPUP_CLASS[]   = L"LumosPopup";

/* ---- Popup data ---- */

typedef struct {
    MonitorList *ml;
    int activeSlider;
    int masterPercent;
    int dragPercent;
    DWORD lastApplyTick;   /* throttles hardware writes during slider drag */
} PopupData;

/* Max rate of hardware brightness writes while dragging a slider. The WMI
 * backend (internal panels) does a full COM roundtrip per write, so applying
 * on every WM_MOUSEMOVE would stutter; the visual (RenderPopup) still updates
 * every move and the exact release value is flushed on mouse-up. */
#define DRAG_APPLY_INTERVAL_MS 60

static PopupData g_popupData;

/* Forward declarations */
static void GetDeltaRange(MonitorList *ml, int *outMin, int *outMax);
static int MasterTargetToSlider(MonitorList *ml, int target);

static int GetMonPercent(BrightMonitor *mon)
{
    DWORD range = mon->brightnessMax - mon->brightnessMin;
    if (range == 0) return 0;
    return (int)(((mon->brightnessCur - mon->brightnessMin) * 100) / range);
}

static int GetMasterPercent(MonitorList *ml)
{
    /* Recover base target by subtracting deltas, then map to slider 0-100 */
    int sum = 0, cnt = 0;
    for (int i = 0; i < ml->count; i++) {
        if (ml->monitors[i].controllable) {
            sum += GetMonPercent(&ml->monitors[i]) - ml->monitors[i].delta;
            cnt++;
        }
    }
    int target = cnt > 0 ? sum / cnt : 50;
    return MasterTargetToSlider(ml, target);
}

/* Forward declarations for layout helpers */
static void GetSliderRect(int row, RECT *rc);

/* ---- Callback for saving deltas from UI ---- */

typedef void (*DeltaSaveCallback)(void);
static DeltaSaveCallback g_deltaSaveCb = NULL;

void UI_SetDeltaSaveCallback(DeltaSaveCallback cb)
{
    g_deltaSaveCb = cb;
}

static ManualChangeCallback g_manualChangeCb = NULL;

void UI_SetManualChangeCallback(ManualChangeCallback cb)
{
    g_manualChangeCb = cb;
}

/* ---- Delta button layout ---- */

#define DELTA_BTN_W   22
#define DELTA_BTN_H   16

static void GetDeltaButtonRects(int row, RECT *rcMinus, RECT *rcValue, RECT *rcPlus)
{
    RECT rcSlider;
    GetSliderRect(row, &rcSlider);
    int cy = rcSlider.bottom + 6;
    int centerX = (rcSlider.left + rcSlider.right) / 2;

    rcMinus->left = centerX - 50;
    rcMinus->right = rcMinus->left + DELTA_BTN_W;
    rcMinus->top = cy;
    rcMinus->bottom = cy + DELTA_BTN_H;

    rcPlus->right = centerX + 50;
    rcPlus->left = rcPlus->right - DELTA_BTN_W;
    rcPlus->top = cy;
    rcPlus->bottom = cy + DELTA_BTN_H;

    rcValue->left = rcMinus->right + 2;
    rcValue->right = rcPlus->left - 2;
    rcValue->top = cy;
    rcValue->bottom = cy + DELTA_BTN_H;
}

/* Returns: -1 = minus btn, +1 = plus btn, 0 = none. Sets *outRow. */
static int HitTestDelta(PopupData *pd, int x, int y, int *outRow)
{
    for (int row = 0; row < pd->ml->count; row++) {
        RECT rcMinus, rcValue, rcPlus;
        GetDeltaButtonRects(row, &rcMinus, &rcValue, &rcPlus);
        if (y >= rcMinus.top && y <= rcMinus.bottom) {
            if (x >= rcMinus.left && x <= rcMinus.right) {
                *outRow = row;
                return -1;
            }
            if (x >= rcPlus.left && x <= rcPlus.right) {
                *outRow = row;
                return 1;
            }
        }
    }
    *outRow = -1;
    return 0;
}

/* ---- Popup layout ---- */

static int GetPopupHeight(PopupData *pd)
{
    int rows = pd->ml->count + 1;
    return POPUP_PADDING + 24 + (rows * POPUP_ROW_H) + POPUP_PADDING;
}

static void GetSliderRect(int row, RECT *rc)
{
    int y = POPUP_PADDING + 24 + row * POPUP_ROW_H + 28;
    rc->left = POPUP_PADDING + 4;
    rc->right = POPUP_WIDTH - POPUP_PADDING - 4;
    rc->top = y - SLIDER_TRACK_H / 2;
    rc->bottom = y + SLIDER_TRACK_H / 2;
}

static int PercentFromX(RECT *sliderRect, int x)
{
    int trackLeft = sliderRect->left + SLIDER_THUMB_R;
    int trackRight = sliderRect->right - SLIDER_THUMB_R;
    if (trackRight <= trackLeft) return 0;
    int pct = ((x - trackLeft) * 100) / (trackRight - trackLeft);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static int XFromPercent(RECT *sliderRect, int pct)
{
    int trackLeft = sliderRect->left + SLIDER_THUMB_R;
    int trackRight = sliderRect->right - SLIDER_THUMB_R;
    return trackLeft + (pct * (trackRight - trackLeft)) / 100;
}

/* ---- Popup rendering (UpdateLayeredWindow) ---- */

static void RenderPopup(HWND hwnd, PopupData *pd)
{
    int w = POPUP_WIDTH;
    int h = GetPopupHeight(pd);

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    /* Fill background */
    HBRUSH bgBrush = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(dc, TRANSPARENT);
    HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    /* Title */
    HFONT oldFont = (HFONT)SelectObject(dc, hFontBold);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    RECT rcTitle = { POPUP_PADDING, POPUP_PADDING, w - POPUP_PADDING, POPUP_PADDING + 20 };
    DrawTextW(dc, L"Brightness", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);

    MonitorList *ml = pd->ml;
    int totalRows = ml->count + 1;
    HBRUSH trackBrush = CreateSolidBrush(HexToColorRef(CLR_TRACK));
    HBRUSH fillBrush = CreateSolidBrush(HexToColorRef(CLR_ACCENT));
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);

    for (int row = 0; row < totalRows; row++) {
        BOOL isMaster = (row == ml->count);
        int pct;
        WCHAR label[140];
        WCHAR pctStr[8];

        if (isMaster) {
            pct = pd->masterPercent;
            wcscpy(label, L"All Monitors");
        } else {
            pct = GetMonPercent(&ml->monitors[row]);
            wsprintfW(label, L"%s", ml->monitors[row].name);
        }

        if (pd->activeSlider == row && pd->dragPercent >= 0)
            pct = pd->dragPercent;

        wsprintfW(pctStr, L"%d%%", pct);

        if (isMaster) {
            int sepY = POPUP_PADDING + 24 + row * POPUP_ROW_H + 2;
            HBRUSH sepBrush = CreateSolidBrush(HexToColorRef(CLR_TRACK));
            RECT rcSep = { POPUP_PADDING, sepY, w - POPUP_PADDING, sepY + 1 };
            FillRect(dc, &rcSep, sepBrush);
            DeleteObject(sepBrush);
        }

        int labelY = POPUP_PADDING + 24 + row * POPUP_ROW_H + 6;
        RECT rcLabel = { POPUP_PADDING + 4, labelY, w - POPUP_PADDING - 40, labelY + 18 };
        SelectObject(dc, isMaster ? hFontBold : hFont);
        SetTextColor(dc, HexToColorRef(isMaster ? CLR_ACCENT : CLR_TEXT));
        DrawTextW(dc, label, -1, &rcLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT rcPct = { w - POPUP_PADDING - 40, labelY, w - POPUP_PADDING - 4, labelY + 18 };
        SelectObject(dc, hFontSmall);
        SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
        DrawTextW(dc, pctStr, -1, &rcPct, DT_RIGHT | DT_SINGLELINE);

        RECT rcSlider;
        GetSliderRect(row, &rcSlider);

        HBRUSH oldBr = (HBRUSH)SelectObject(dc, trackBrush);
        RoundRect(dc, rcSlider.left, rcSlider.top, rcSlider.right, rcSlider.bottom,
                  SLIDER_TRACK_H, SLIDER_TRACK_H);

        int thumbX = XFromPercent(&rcSlider, pct);
        SelectObject(dc, fillBrush);
        RoundRect(dc, rcSlider.left, rcSlider.top, thumbX, rcSlider.bottom,
                  SLIDER_TRACK_H, SLIDER_TRACK_H);
        SelectObject(dc, oldBr);

        int cy = (rcSlider.top + rcSlider.bottom) / 2;
        Ellipse(dc, thumbX - SLIDER_THUMB_R, cy - SLIDER_THUMB_R,
                thumbX + SLIDER_THUMB_R, cy + SLIDER_THUMB_R);

        /* Delta controls (skip master row) */
        if (!isMaster) {
            RECT rcMinus, rcValue, rcPlus;
            GetDeltaButtonRects(row, &rcMinus, &rcValue, &rcPlus);

            /* [-] button */
            HBRUSH btnBrush = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
            FillRect(dc, &rcMinus, btnBrush);
            FillRect(dc, &rcPlus, btnBrush);
            DeleteObject(btnBrush);

            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, L"\x2013", -1, &rcMinus, DT_CENTER | DT_VCENTER | DT_SINGLELINE); /* en dash as minus */
            DrawTextW(dc, L"+", -1, &rcPlus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            /* Delta value */
            WCHAR deltaStr[16];
            int d = ml->monitors[row].delta;
            if (d > 0)
                wsprintfW(deltaStr, L"\x25B3+%d", d);
            else if (d < 0)
                wsprintfW(deltaStr, L"\x25B3%d", d);
            else
                wsprintfW(deltaStr, L"\x25B3 0");
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, deltaStr, -1, &rcValue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(trackBrush);
    DeleteObject(fillBrush);
    SelectObject(dc, oldFont);
    DeleteObject(hFont);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

    /* Apply rounded corners with anti-aliased alpha */
    ApplyRoundedMask(bits, w, h, POPUP_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---- Popup hit testing ---- */

static int HitTestSlider(PopupData *pd, int x, int y)
{
    int totalRows = pd->ml->count + 1;
    for (int row = 0; row < totalRows; row++) {
        RECT rc;
        GetSliderRect(row, &rc);
        rc.top -= SLIDER_THUMB_R + 4;
        rc.bottom += SLIDER_THUMB_R + 4;
        if (x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom)
            return row;
    }
    return -1;
}

/* Get delta range across all DDC monitors */
static void GetDeltaRange(MonitorList *ml, int *outMin, int *outMax)
{
    int lo = 0, hi = 0;
    for (int i = 0; i < ml->count; i++) {
        if (!ml->monitors[i].controllable) continue;
        int d = ml->monitors[i].delta;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    *outMin = lo;
    *outMax = hi;
}

/* Map slider 0-100 to extended master target so all monitors can reach full range */
static int SliderToMasterTarget(MonitorList *ml, int sliderPct)
{
    int minD, maxD;
    GetDeltaRange(ml, &minD, &maxD);
    int lo = -maxD;          /* slider 0%   → all monitors at 0 */
    int hi = 100 - minD;     /* slider 100% → all monitors at 100 */
    return lo + (sliderPct * (hi - lo)) / 100;
}

/* Map extended master target back to slider 0-100 */
static int MasterTargetToSlider(MonitorList *ml, int target)
{
    int minD, maxD;
    GetDeltaRange(ml, &minD, &maxD);
    int lo = -maxD;
    int hi = 100 - minD;
    if (hi == lo) return 50;
    int s = ((target - lo) * 100) / (hi - lo);
    if (s < 0) s = 0;
    if (s > 100) s = 100;
    return s;
}

static void ApplySliderValue(PopupData *pd, int row, int percent)
{
    MonitorList *ml = pd->ml;
    BOOL isMaster = (row == ml->count);

    if (isMaster) {
        pd->masterPercent = percent;
        int target = SliderToMasterTarget(ml, percent);
        Monitor_SetAllBrightness(ml, target);
    } else {
        Monitor_SetBrightness(&ml->monitors[row], (DWORD)percent);
    }

    if (g_manualChangeCb) g_manualChangeCb();
}

/* ---- Popup Window Procedure ---- */

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PopupData *pd = (PopupData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        pd = (PopupData *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pd);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!pd) break;
        int x = LOWORD(lParam), y = HIWORD(lParam);

        /* Check delta buttons first */
        int deltaRow;
        int deltaDir = HitTestDelta(pd, x, y, &deltaRow);
        if (deltaDir != 0 && deltaRow >= 0) {
            BrightMonitor *mon = &pd->ml->monitors[deltaRow];
            mon->delta += deltaDir;
            if (mon->delta < -40) mon->delta = -40;
            if (mon->delta > 40) mon->delta = 40;
            if (g_deltaSaveCb) g_deltaSaveCb();
            RenderPopup(hwnd, pd);
            return 0;
        }

        int row = HitTestSlider(pd, x, y);
        if (row >= 0) {
            pd->activeSlider = row;
            RECT rc;
            GetSliderRect(row, &rc);
            int pct = PercentFromX(&rc, x);
            pd->dragPercent = pct;
            ApplySliderValue(pd, row, pct);
            pd->lastApplyTick = GetTickCount();
            SetCapture(hwnd);
            RenderPopup(hwnd, pd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!pd || pd->activeSlider < 0) break;
        if (!(wParam & MK_LBUTTON)) {
            pd->activeSlider = -1;
            pd->dragPercent = -1;
            ReleaseCapture();
            break;
        }
        int x = LOWORD(lParam);
        int row = pd->activeSlider;
        RECT rc;
        GetSliderRect(row, &rc);
        int pct = PercentFromX(&rc, x);
        pd->dragPercent = pct;
        /* Throttle hardware writes; the final value is flushed on mouse-up. */
        DWORD now = GetTickCount();
        if (now - pd->lastApplyTick >= DRAG_APPLY_INTERVAL_MS) {
            pd->lastApplyTick = now;
            ApplySliderValue(pd, row, pct);
        }
        RenderPopup(hwnd, pd);
        return 0;
    }

    case WM_LBUTTONUP:
        if (pd) {
            int row = pd->activeSlider;
            int pct = pd->dragPercent;
            pd->activeSlider = -1;
            pd->dragPercent = -1;
            ReleaseCapture();
            /* Flush the exact release value: intermediate drag writes were
             * throttled, so the last move may not have been applied. */
            if (row >= 0 && pct >= 0)
                ApplySliderValue(pd, row, pct);
            Monitor_RefreshBrightness(pd->ml);
            pd->masterPercent = GetMasterPercent(pd->ml);
            RenderPopup(hwnd, pd);
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && pd)
            UI_HidePopup(hwnd);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND UI_CreatePopup(HINSTANCE hInst, MonitorList *ml)
{
    memset(&g_popupData, 0, sizeof(g_popupData));
    g_popupData.ml = ml;
    g_popupData.activeSlider = -1;
    g_popupData.dragPercent = -1;
    g_popupData.masterPercent = GetMasterPercent(ml);

    int h = GetPopupHeight(&g_popupData);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        POPUP_CLASS, L"",
        WS_POPUP,
        0, 0, POPUP_WIDTH, h,
        NULL, NULL, hInst, &g_popupData);

    return hwnd;
}

void UI_ShowPopup(HWND hwnd, MonitorList *ml)
{
    if (!hwnd) return;

    Monitor_RefreshBrightness(ml);
    g_popupData.ml = ml;
    g_popupData.masterPercent = GetMasterPercent(ml);

    int h = GetPopupHeight(&g_popupData);
    SetWindowPos(hwnd, NULL, 0, 0, POPUP_WIDTH, h, SWP_NOMOVE | SWP_NOZORDER);

    /* Position near cursor (tray icon), adjusted to stay on-screen */
    POINT pt;
    GetCursorPos(&pt);

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    APPBARDATA abd = { sizeof(abd) };
    SHAppBarMessage(ABM_GETTASKBARPOS, &abd);

    int x, y;

    /* Horizontal: center on cursor, clamp to work area */
    x = pt.x - POPUP_WIDTH / 2;
    if (x + POPUP_WIDTH > mi.rcWork.right)
        x = mi.rcWork.right - POPUP_WIDTH - 4;
    if (x < mi.rcWork.left)
        x = mi.rcWork.left + 4;

    /* Vertical: place above or below taskbar depending on edge */
    if (abd.uEdge == ABE_TOP) {
        y = abd.rc.bottom + 8;
    } else if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) {
        y = pt.y - h;
        if (y < mi.rcWork.top) y = pt.y;
    } else {
        /* Bottom taskbar (default) — place above cursor */
        y = pt.y - h - 8;
        if (y < mi.rcWork.top)
            y = pt.y + 8;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE);
    RenderPopup(hwnd, &g_popupData);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);
}

void UI_HidePopup(HWND hwnd)
{
    if (!hwnd) return;
    ShowWindow(hwnd, SW_HIDE);
}

void UI_TogglePopup(HWND hwnd, MonitorList *ml)
{
    if (IsWindowVisible(hwnd))
        UI_HidePopup(hwnd);
    else
        UI_ShowPopup(hwnd, ml);
}

BOOL UI_IsPopupVisible(HWND hwnd)
{
    return hwnd && IsWindowVisible(hwnd);
}

void UI_RefreshPopup(HWND hwnd, MonitorList *ml)
{
    if (!hwnd || !IsWindowVisible(hwnd)) return;
    g_popupData.ml = ml;
    g_popupData.masterPercent = GetMasterPercent(ml);
    RenderPopup(hwnd, &g_popupData);
}

/* ---- Class registration ---- */

BOOL UiPopup_Init(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PopupWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = POPUP_CLASS;
    return RegisterClassExW(&wc) != 0;
}
