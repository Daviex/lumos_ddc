#include "ui_internal.h"
#include "brightmap.h"
#include <shellapi.h>
#include <windowsx.h>

static const WCHAR POPUP_CLASS[]   = L"LumosPopup";

/* ---- Popup data ---- */

typedef struct {
    MonitorList *ml;
    int activeSlider;
    int masterPercent;
    int dragPercent;
    DWORD lastApplyTick;   /* throttles hardware writes during slider drag */
    BOOL keyDrag;          /* the drag above is driven by a held arrow key */
    int  focusItem;        /* keyboard focus, see "Keyboard items" below */
    BOOL focusVisible;     /* draw the ring: set once the keyboard is in use */
} PopupData;

/* Writes a key-driven value that arrived inside the throttle interval. */
#define POPUP_FLUSH_TIMER 1

/* Max rate of hardware brightness writes while dragging a slider. The WMI
 * backend (internal panels) does a full COM roundtrip per write, so applying
 * on every WM_MOUSEMOVE would stutter; the visual (RenderPopup) still updates
 * every move and the exact release value is flushed on mouse-up. */
#define DRAG_APPLY_INTERVAL_MS 60

static PopupData g_popupData;

static int GetMonPercent(BrightMonitor *mon)
{
    return Monitor_GetPercent(mon);
}

static int GetMasterPercent(MonitorList *ml)
{
    /* Map each reading back through its range and average the results */
    int sum = 0, cnt = 0;
    for (int i = 0; i < ml->count; i++) {
        if (ml->monitors[i].controllable) {
            BrightMonitor *mon = &ml->monitors[i];
            sum += BrightMap_Master(GetMonPercent(mon), mon->rangeLo, mon->rangeHi);
            cnt++;
        }
    }
    return cnt > 0 ? sum / cnt : 50;
}

/* Forward declarations for layout helpers */
static void GetSliderRect(int row, RECT *rc);

/* ---- Callback for range changes from the UI ---- */

static RangeChangeCallback g_rangeChangeCb = NULL;

void UI_SetRangeChangeCallback(RangeChangeCallback cb)
{
    g_rangeChangeCb = cb;
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

/* ---- Keyboard items ----
   Focus order follows the layout: each monitor's slider, then its offset
   control, then "All Monitors". Item i belongs to row i / 2, which also gives
   the master row (ml->count) for the last item. */

static int ItemCount(PopupData *pd)  { return pd->ml->count * 2 + 1; }
static int MasterItem(PopupData *pd) { return pd->ml->count * 2; }
static int ItemRow(int item)         { return item / 2; }

static BOOL ItemIsOffset(PopupData *pd, int item)
{
    return item < MasterItem(pd) && (item % 2) == 1;
}

/* The percentage a row shows right now, including a drag in progress. */
static int RowPercent(PopupData *pd, int row)
{
    if (pd->activeSlider == row && pd->dragPercent >= 0)
        return pd->dragPercent;
    if (row == pd->ml->count)
        return pd->masterPercent;
    return GetMonPercent(&pd->ml->monitors[row]);
}

/* Outline drawn around the focused item. */
static void GetItemFocusRect(PopupData *pd, int item, RECT *rc)
{
    int row = ItemRow(item);
    if (ItemIsOffset(pd, item)) {
        RECT rcMinus, rcValue, rcPlus;
        GetDeltaButtonRects(row, &rcMinus, &rcValue, &rcPlus);
        SetRect(rc, rcMinus.left - 3, rcMinus.top - 3, rcPlus.right + 3, rcPlus.bottom + 3);
    } else {
        GetSliderRect(row, rc);
        InflateRect(rc, 4, SLIDER_THUMB_R + 3);
    }
}

static int PopupA11yCount(void *ctx)
{
    return ItemCount((PopupData *)ctx);
}

static void PopupA11yDescribe(void *ctx, int index, A11yItem *out)
{
    PopupData *pd = (PopupData *)ctx;
    if (index < 0) {
        out->role = ROLE_SYSTEM_DIALOG;
        out->state = STATE_SYSTEM_FOCUSABLE;
        wcscpy(out->name, APP_NAME L" brightness");
        return;
    }
    int row = ItemRow(index);
    BOOL isMaster = (index == MasterItem(pd));
    BrightMonitor *mon = isMaster ? NULL : &pd->ml->monitors[row];
    GetItemFocusRect(pd, index, &out->rect);
    out->state = STATE_SYSTEM_FOCUSABLE;
    if (mon && !mon->controllable)
        out->state |= STATE_SYSTEM_UNAVAILABLE;

    if (ItemIsOffset(pd, index)) {
        out->role = ROLE_SYSTEM_SPINBUTTON;
        _snwprintf(out->name, 159, L"%s maximum", mon->name);
        _snwprintf(out->value, 63, L"%d%%", mon->rangeHi);
    } else {
        out->role = ROLE_SYSTEM_SLIDER;
        if (isMaster)
            wcscpy(out->name, L"All monitors");
        else
            _snwprintf(out->name, 159, L"%s", mon->name);
        if (mon && !mon->controllable)
            wcscpy(out->value, L"Unavailable");
        else
            _snwprintf(out->value, 63, L"%d%%", RowPercent(pd, row));
    }
}

static int PopupA11yFocused(void *ctx)
{
    return ((PopupData *)ctx)->focusItem;
}

static const A11yModel g_popupModel = {
    PopupA11yCount, PopupA11yDescribe, PopupA11yFocused, NULL, &g_popupData
};

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
        /* A monitor that does not answer shows no level and no thumb: the last
           value read from it may be long out of date. */
        BOOL unavailable = !isMaster && !ml->monitors[row].controllable;
        int pct;
        WCHAR label[140];
        WCHAR pctStr[16];

        if (isMaster) {
            pct = pd->masterPercent;
            wcscpy(label, L"All Monitors");
        } else {
            pct = GetMonPercent(&ml->monitors[row]);
            wsprintfW(label, L"%s", ml->monitors[row].name);
        }

        if (pd->activeSlider == row && pd->dragPercent >= 0)
            pct = pd->dragPercent;

        if (unavailable)
            wcscpy(pctStr, L"Unavailable");
        else
            wsprintfW(pctStr, L"%d%%", pct);
        int pctW = unavailable ? 80 : 40;

        if (isMaster) {
            int sepY = POPUP_PADDING + 24 + row * POPUP_ROW_H + 2;
            HBRUSH sepBrush = CreateSolidBrush(HexToColorRef(CLR_TRACK));
            RECT rcSep = { POPUP_PADDING, sepY, w - POPUP_PADDING, sepY + 1 };
            FillRect(dc, &rcSep, sepBrush);
            DeleteObject(sepBrush);
        }

        int labelY = POPUP_PADDING + 24 + row * POPUP_ROW_H + 6;
        RECT rcLabel = { POPUP_PADDING + 4, labelY, w - POPUP_PADDING - pctW, labelY + 18 };
        SelectObject(dc, isMaster ? hFontBold : hFont);
        SetTextColor(dc, HexToColorRef(isMaster ? CLR_ACCENT
                                       : unavailable ? CLR_SUBTEXT : CLR_TEXT));
        /* DT_NOPREFIX: monitor names contain "&" ("Wide viewing angle & High
           density"), which DrawText would otherwise turn into an underline. */
        DrawTextW(dc, label, -1, &rcLabel,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        RECT rcPct = { w - POPUP_PADDING - pctW, labelY, w - POPUP_PADDING - 4, labelY + 18 };
        SelectObject(dc, hFontSmall);
        SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
        DrawTextW(dc, pctStr, -1, &rcPct, DT_RIGHT | DT_SINGLELINE);

        RECT rcSlider;
        GetSliderRect(row, &rcSlider);

        HBRUSH oldBr = (HBRUSH)SelectObject(dc, trackBrush);
        RoundRect(dc, rcSlider.left, rcSlider.top, rcSlider.right, rcSlider.bottom,
                  SLIDER_TRACK_H, SLIDER_TRACK_H);

        if (unavailable) {
            SelectObject(dc, oldBr);
        } else {
            int thumbX = XFromPercent(&rcSlider, pct);
            SelectObject(dc, fillBrush);
            RoundRect(dc, rcSlider.left, rcSlider.top, thumbX, rcSlider.bottom,
                      SLIDER_TRACK_H, SLIDER_TRACK_H);
            SelectObject(dc, oldBr);

            int cy = (rcSlider.top + rcSlider.bottom) / 2;
            Ellipse(dc, thumbX - SLIDER_THUMB_R, cy - SLIDER_THUMB_R,
                    thumbX + SLIDER_THUMB_R, cy + SLIDER_THUMB_R);
        }

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

            /* The monitor's level at All Monitors 100% */
            WCHAR maxStr[16];
            wsprintfW(maxStr, L"Max %d%%", ml->monitors[row].rangeHi);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, maxStr, -1, &rcValue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(trackBrush);
    DeleteObject(fillBrush);

    if (pd->focusVisible && pd->focusItem >= 0 && pd->focusItem < ItemCount(pd)) {
        RECT rcFocus;
        GetItemFocusRect(pd, pd->focusItem, &rcFocus);
        DrawFocusRing(dc, &rcFocus, 10);
    }
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
        if (row < pd->ml->count && !pd->ml->monitors[row].controllable)
            continue;   /* no slider to drag on a monitor that does not answer */
        RECT rc;
        GetSliderRect(row, &rc);
        rc.top -= SLIDER_THUMB_R + 4;
        rc.bottom += SLIDER_THUMB_R + 4;
        if (x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom)
            return row;
    }
    return -1;
}

static void ApplySliderValue(PopupData *pd, int row, int percent)
{
    MonitorList *ml = pd->ml;
    BOOL isMaster = (row == ml->count);

    if (isMaster) {
        pd->masterPercent = percent;
        Monitor_SetAllBrightness(ml, percent);
    } else {
        Monitor_SetBrightness(&ml->monitors[row], (DWORD)percent);
    }

    if (g_manualChangeCb) g_manualChangeCb(isMaster ? percent : -1);
}

/* ---- Keyboard ---- */

static void SetFocusItem(HWND hwnd, PopupData *pd, int item)
{
    int n = ItemCount(pd);
    pd->focusItem = (item % n + n) % n;   /* wrap both ways */
    RenderPopup(hwnd, pd);
    A11y_NotifyFocus(hwnd, pd->focusItem);
}

/* Apply the pending key value now. */
static void FlushKeyValue(HWND hwnd, PopupData *pd)
{
    KillTimer(hwnd, POPUP_FLUSH_TIMER);
    if (pd->keyDrag && pd->activeSlider >= 0 && pd->dragPercent >= 0) {
        ApplySliderValue(pd, pd->activeSlider, pd->dragPercent);
        pd->lastApplyTick = GetTickCount();
    }
}

/* A held arrow key works like a mouse drag: the display follows every repeat,
   the hardware gets at most one write per DRAG_APPLY_INTERVAL_MS, and the
   timer delivers the last value if the repeats stop inside an interval. */
static void KeyAdjustSlider(HWND hwnd, PopupData *pd, int row, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == RowPercent(pd, row))
        return;
    pd->activeSlider = row;
    pd->keyDrag = TRUE;
    pd->dragPercent = pct;
    if (GetTickCount() - pd->lastApplyTick >= DRAG_APPLY_INTERVAL_MS)
        FlushKeyValue(hwnd, pd);
    else
        SetTimer(hwnd, POPUP_FLUSH_TIMER, DRAG_APPLY_INTERVAL_MS, NULL);
    RenderPopup(hwnd, pd);
    A11y_NotifyValue(hwnd, pd->focusItem);
}

/* Key released: write the final value and leave drag mode. Monitor_SetBrightness
   updates the cached level, so there is no DDC read-back here. */
static void EndKeyDrag(HWND hwnd, PopupData *pd)
{
    if (!pd->keyDrag)
        return;
    FlushKeyValue(hwnd, pd);
    pd->keyDrag = FALSE;
    pd->activeSlider = -1;
    pd->dragPercent = -1;
    RenderPopup(hwnd, pd);
}

/* Set a monitor's level at All Monitors 100%. The low end comes from
   Settings, and the range keeps its minimum width. */
static void AdjustMax(HWND hwnd, PopupData *pd, int row, int value)
{
    BrightMonitor *mon = &pd->ml->monitors[row];
    int lo = mon->rangeLo;
    if (value < lo + BRIGHTMAP_MIN_SPAN) value = lo + BRIGHTMAP_MIN_SPAN;
    if (value > 100) value = 100;
    if (value == mon->rangeHi)
        return;
    mon->rangeHi = value;
    if (g_rangeChangeCb) g_rangeChangeCb(pd->masterPercent);
    RenderPopup(hwnd, pd);
    A11y_NotifyValue(hwnd, pd->focusItem);
}

/* Standard slider keys: Tab and Shift+Tab move between controls, the arrows
   change the value by 1 (Up and Right raise it), Page Up and Page Down by 10,
   Home and End go to the limits. Escape closes. */
static void PopupKeyDown(HWND hwnd, PopupData *pd, WPARAM vk)
{
    int item = pd->focusItem;
    int row = ItemRow(item);

    if (!pd->focusVisible) {
        pd->focusVisible = TRUE;
        RenderPopup(hwnd, pd);
    }

    if (vk == VK_ESCAPE) {
        UI_HidePopup(hwnd);
        return;
    }
    if (vk == VK_TAB) {
        EndKeyDrag(hwnd, pd);
        SetFocusItem(hwnd, pd, item + (KEY_DOWN(VK_SHIFT) ? -1 : 1));
        return;
    }

    int dir = 0, page = 0, limit = 0;
    switch (vk) {
    case VK_RIGHT: case VK_UP:   dir = 1;  break;
    case VK_LEFT:  case VK_DOWN: dir = -1; break;
    case VK_PRIOR: page = 1;  break;
    case VK_NEXT:  page = -1; break;
    case VK_HOME:  limit = -1; break;
    case VK_END:   limit = 1;  break;
    default: return;
    }

    if (ItemIsOffset(pd, item)) {
        int v = pd->ml->monitors[row].rangeHi;
        if (limit)
            v = (limit > 0) ? 100 : 0;   /* AdjustMax raises 0 to the lowest allowed */
        else
            v += dir + page * 5;
        AdjustMax(hwnd, pd, row, v);
        return;
    }

    if (row < pd->ml->count && !pd->ml->monitors[row].controllable)
        return;
    int pct = RowPercent(pd, row);
    if (limit)
        pct = (limit > 0) ? 100 : 0;
    else
        pct += dir + page * 10;
    KeyAdjustSlider(hwnd, pd, row, pct);
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
        A11y_Attach(hwnd, &g_popupModel);
        return 0;
    }

    case WM_GETOBJECT: {
        LRESULT r;
        if (A11y_HandleGetObject(hwnd, wParam, lParam, &r))
            return r;
        break;
    }

    case WM_KEYDOWN:
        if (pd) PopupKeyDown(hwnd, pd, wParam);
        return 0;

    case WM_KEYUP:
        if (pd) EndKeyDrag(hwnd, pd);
        return 0;

    case WM_TIMER:
        if (pd && wParam == POPUP_FLUSH_TIMER)
            FlushKeyValue(hwnd, pd);
        return 0;

    case WM_CAPTURECHANGED:
        /* Another window took the mouse mid-drag and will get the button-up.
           Finish the drag here, or the next mouse move keeps dragging. */
        if (pd && pd->activeSlider >= 0 && !pd->keyDrag) {
            if (pd->dragPercent >= 0)
                ApplySliderValue(pd, pd->activeSlider, pd->dragPercent);
            pd->activeSlider = -1;
            pd->dragPercent = -1;
            RenderPopup(hwnd, pd);
        }
        return 0;

    case WM_CLOSE:
        /* Alt+F4 hides the popup. It is created once and reused, and lumos.c
           keeps its handle, so destroying it here would leave that dangling. */
        UI_HidePopup(hwnd);
        return 0;

    case WM_DESTROY:
        A11y_Detach(hwnd);
        return 0;

    case WM_LBUTTONDOWN: {
        if (!pd) break;
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

        /* Check delta buttons first */
        int deltaRow;
        int deltaDir = HitTestDelta(pd, x, y, &deltaRow);
        if (deltaDir != 0 && deltaRow >= 0) {
            EndKeyDrag(hwnd, pd);
            pd->focusItem = deltaRow * 2 + 1;
            AdjustMax(hwnd, pd, deltaRow, pd->ml->monitors[deltaRow].rangeHi + deltaDir);
            return 0;
        }

        int row = HitTestSlider(pd, x, y);
        if (row >= 0) {
            EndKeyDrag(hwnd, pd);
            pd->focusItem = row * 2;
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
        if (!pd || pd->activeSlider < 0 || pd->keyDrag) break;
        if (!(wParam & MK_LBUTTON)) {
            pd->activeSlider = -1;
            pd->dragPercent = -1;
            ReleaseCapture();
            break;
        }
        /* Signed: with the mouse captured, a pointer left of the popup has a
           negative x, which LOWORD would turn into ~65535 and so 100%. */
        int x = GET_X_LPARAM(lParam);
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
        if (LOWORD(wParam) == WA_INACTIVE && pd) {
            EndKeyDrag(hwnd, pd);
            UI_HidePopup(hwnd);
        }
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
    g_popupData.focusItem = MasterItem(&g_popupData);

    int h = GetPopupHeight(&g_popupData);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        POPUP_CLASS, L"",
        WS_POPUP,
        0, 0, POPUP_WIDTH, h,
        NULL, NULL, hInst, &g_popupData);

    return hwnd;
}

void UI_ShowPopup(HWND hwnd, MonitorList *ml, const POINT *anchor, BOOL fromKeyboard)
{
    if (!hwnd) return;

    Monitor_RefreshBrightness(ml);
    g_popupData.ml = ml;
    g_popupData.masterPercent = GetMasterPercent(ml);
    /* Every opening starts on "All Monitors", the control most people want. */
    g_popupData.focusItem = MasterItem(&g_popupData);
    g_popupData.focusVisible = fromKeyboard;

    int h = GetPopupHeight(&g_popupData);
    SetWindowPos(hwnd, NULL, 0, 0, POPUP_WIDTH, h, SWP_NOMOVE | SWP_NOZORDER);

    /* Position near the anchor (the tray icon) or the cursor, adjusted to stay on-screen */
    POINT pt;
    if (anchor)
        pt = *anchor;
    else
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
    A11y_NotifyFocus(hwnd, g_popupData.focusItem);
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
        UI_ShowPopup(hwnd, ml, NULL, FALSE);
}

BOOL UI_IsPopupVisible(HWND hwnd)
{
    return hwnd && IsWindowVisible(hwnd);
}

void UI_RefreshPopup(HWND hwnd, MonitorList *ml)
{
    if (!hwnd || !IsWindowVisible(hwnd)) return;
    PopupData *pd = &g_popupData;
    int row = ItemRow(pd->focusItem);
    int before = ItemIsOffset(pd, pd->focusItem) ? 0 : RowPercent(pd, row);
    pd->ml = ml;
    pd->masterPercent = GetMasterPercent(ml);
    RenderPopup(hwnd, pd);
    /* A hotkey or the schedule changed the level under the focused slider:
       the screen reader hears it as the slider's new value. */
    if (!ItemIsOffset(pd, pd->focusItem) && RowPercent(pd, row) != before)
        A11y_NotifyValue(hwnd, pd->focusItem);
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
