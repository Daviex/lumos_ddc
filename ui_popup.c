#include "ui_internal.h"
#include "brightmap.h"
#include "ui_graphics.h"
#include "brightness.h"
#include <shellapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>

static const WCHAR POPUP_CLASS[] = L"LumosPopup";

/* ---- Popup data ---- */

typedef struct {
    MonitorList *ml;
    int activeSlider;
    int masterPercent;
    int dragPercent;
    BOOL keyDrag;
    int focusItem;
    BOOL focusVisible;
} PopupData;

typedef struct {
    int count;
    BOOL selectedOnly;
    BOOL enabled[MAX_MONITORS + 1];
    BOOL excluded[MAX_MONITORS];
    BOOL sourceBlocked[MAX_MONITORS];
    BOOL sourceUnknown[MAX_MONITORS];
    int percent[MAX_MONITORS + 1];
    int rangeHi[MAX_MONITORS];
    int focusItem;
    BOOL focusVisible;
    WCHAR name[MAX_MONITORS][128];
} PopupFrame;

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ originalBitmap;
    BYTE *bits;
    BYTE *alpha;
    int width;
    int height;
    HFONT font;
    HFONT fontBold;
    HFONT fontSmall;
    HBRUSH background;
    HBRUSH track;
    HBRUSH accent;
    HBRUSH surface;
    HPEN noPen;
    PopupFrame frame;
    BOOL frameValid;
} PopupRenderCache;

static PopupData g_popupData;
static PopupRenderCache g_popupRender;
static BOOL g_masterTargetKnown;
static int g_masterTarget;

static int GetMasterPercent(MonitorList *ml)
{
    return g_masterTargetKnown ? g_masterTarget : Monitor_MasterFromSnapshot(ml);
}

static BOOL CanAdjustSlider(const PopupData *pd, int row)
{
    if (!pd || !pd->ml || row < 0 || row > pd->ml->count) return FALSE;
    return row == pd->ml->count ? Monitor_HasSelected(pd->ml)
        : Monitor_CanControl(&pd->ml->monitors[row]) &&
          Monitor_SourceAllowsControl(&pd->ml->monitors[row]);
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
        if (!CanAdjustSlider(pd, row)) continue;
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
    return Brightness_GetPercent(&pd->ml->monitors[row]);
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
    if (!CanAdjustSlider(pd, row))
        out->state |= STATE_SYSTEM_UNAVAILABLE;

    if (ItemIsOffset(pd, index)) {
        out->role = ROLE_SYSTEM_SPINBUTTON;
        _snwprintf(out->name, 159, L"%s maximum", mon->name);
        _snwprintf(out->value, 63, L"%d%%", mon->rangeHi);
    } else {
        out->role = ROLE_SYSTEM_SLIDER;
        if (isMaster)
            wcscpy(out->name, pd->ml->selectedOnly ? L"Selected monitors" : L"All monitors");
        else
            _snwprintf(out->name, 159, L"%s", mon->name);
        if (!CanAdjustSlider(pd, row))
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

static void ReleasePopupSurface(void)
{
    PopupRenderCache *cache = &g_popupRender;
    if (cache->dc) {
        if (cache->originalBitmap)
            SelectObject(cache->dc, cache->originalBitmap);
        DeleteDC(cache->dc);
    }
    if (cache->bitmap) DeleteObject(cache->bitmap);
    free(cache->alpha);
    cache->dc = NULL;
    cache->bitmap = NULL;
    cache->originalBitmap = NULL;
    cache->bits = NULL;
    cache->alpha = NULL;
    cache->width = 0;
    cache->height = 0;
    cache->frameValid = FALSE;
}

static void ReleasePopupRenderCache(void)
{
    PopupRenderCache *cache = &g_popupRender;
    ReleasePopupSurface();
    if (cache->font) DeleteObject(cache->font);
    if (cache->fontBold) DeleteObject(cache->fontBold);
    if (cache->fontSmall) DeleteObject(cache->fontSmall);
    if (cache->background) DeleteObject(cache->background);
    if (cache->track) DeleteObject(cache->track);
    if (cache->accent) DeleteObject(cache->accent);
    if (cache->surface) DeleteObject(cache->surface);
    if (cache->noPen) DeleteObject(cache->noPen);
    memset(cache, 0, sizeof(*cache));
}

static HFONT CreatePopupFont(int height, int weight)
{
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

/* Keep all GDI objects across frames. Recreate the backing bitmap and alpha
   mask only when the monitor count changes the popup dimensions. */
static BOOL EnsurePopupRenderCache(int w, int h)
{
    PopupRenderCache *cache = &g_popupRender;
    if (!cache->font) {
        cache->font = CreatePopupFont(-13, FW_NORMAL);
        cache->fontBold = CreatePopupFont(-14, FW_SEMIBOLD);
        cache->fontSmall = CreatePopupFont(-11, FW_NORMAL);
        cache->background = CreateSolidBrush(UI_ColorRef(CLR_BG));
        cache->track = CreateSolidBrush(UI_ColorRef(CLR_TRACK));
        cache->accent = CreateSolidBrush(UI_ColorRef(CLR_ACCENT));
        cache->surface = CreateSolidBrush(UI_ColorRef(CLR_SURFACE));
        cache->noPen = CreatePen(PS_NULL, 0, 0);
        if (!cache->font || !cache->fontBold || !cache->fontSmall ||
            !cache->background || !cache->track || !cache->accent ||
            !cache->surface || !cache->noPen) {
            ReleasePopupRenderCache();
            return FALSE;
        }
    }

    if (cache->dc && cache->width == w && cache->height == h)
        return TRUE;

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(NULL);
    BYTE *bits = NULL;
    BYTE *alpha = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ originalBitmap = NULL;
    if (dc) bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bitmap && bits) {
        originalBitmap = SelectObject(dc, bitmap);
        if (originalBitmap && originalBitmap != HGDI_ERROR)
            alpha = (BYTE *)malloc((size_t)w * (size_t)h);
    }
    if (!alpha) {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        return FALSE;
    }

    /* The rounded edge does not move while sliding, so its expensive distance
       calculation is done once instead of on every mouse movement. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            BYTE a = 245;
            int cx = x < POPUP_CORNER ? POPUP_CORNER : w - POPUP_CORNER;
            int cy = y < POPUP_CORNER ? POPUP_CORNER : h - POPUP_CORNER;
            if ((x < POPUP_CORNER || x >= w - POPUP_CORNER) &&
                (y < POPUP_CORNER || y >= h - POPUP_CORNER)) {
                float dx = (float)(x - cx) + 0.5f;
                float dy = (float)(y - cy) + 0.5f;
                float distance = sqrtf(dx * dx + dy * dy);
                if (distance > POPUP_CORNER + 0.5f)
                    a = 0;
                else if (distance > POPUP_CORNER - 0.5f)
                    a = (BYTE)((POPUP_CORNER + 0.5f - distance) * 245);
            }
            alpha[y * w + x] = a;
        }
    }

    ReleasePopupSurface();
    cache->dc = dc;
    cache->bitmap = bitmap;
    cache->originalBitmap = originalBitmap;
    cache->bits = bits;
    cache->alpha = alpha;
    cache->width = w;
    cache->height = h;
    return TRUE;
}

static void GetPopupFrame(PopupData *pd, PopupFrame *frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->count = pd->ml->count;
    frame->selectedOnly = pd->ml->selectedOnly;
    frame->focusItem = pd->focusItem;
    frame->focusVisible = pd->focusVisible;
    for (int row = 0; row <= frame->count; row++) {
        frame->enabled[row] = CanAdjustSlider(pd, row);
        if (row == frame->count) {
            frame->percent[row] = pd->masterPercent;
        } else {
            BrightMonitor *mon = &pd->ml->monitors[row];
            frame->percent[row] = Brightness_GetPercent(mon);
            frame->rangeHi[row] = mon->rangeHi;
            frame->excluded[row] = mon->excludedFromControl;
            frame->sourceBlocked[row] = !Monitor_SourceAllowsControl(mon);
            frame->sourceUnknown[row] = !mon->sourceKnown || !mon->expectedInput;
            wcsncpy(frame->name[row], mon->name, 127);
        }
        if (pd->activeSlider == row && pd->dragPercent >= 0)
            frame->percent[row] = pd->dragPercent;
    }
}

static void RenderPopup(HWND hwnd, PopupData *pd)
{
    if (!pd || !pd->ml) return;
    int w = POPUP_WIDTH;
    int h = GetPopupHeight(pd);
    if (!EnsurePopupRenderCache(w, h)) return;

    PopupRenderCache *cache = &g_popupRender;
    PopupFrame frame;
    GetPopupFrame(pd, &frame);
    if (cache->frameValid && memcmp(&cache->frame, &frame, sizeof(frame)) == 0)
        return;

    HDC dc = cache->dc;
    int savedDC = SaveDC(dc);
    if (!savedDC) return;

    /* Fill background */
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, cache->background);

    SetBkMode(dc, TRANSPARENT);
    HFONT hFont = cache->font;
    HFONT hFontBold = cache->fontBold;
    HFONT hFontSmall = cache->fontSmall;

    /* Title */
    SelectObject(dc, hFontBold);
    SetTextColor(dc, UI_ColorRef(CLR_TEXT));
    RECT rcTitle = { POPUP_PADDING, POPUP_PADDING, w - POPUP_PADDING, POPUP_PADDING + 20 };
    DrawTextW(dc, L"Brightness", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);

    MonitorList *ml = pd->ml;
    int totalRows = ml->count + 1;
    HBRUSH trackBrush = cache->track;
    HBRUSH fillBrush = cache->accent;
    SelectObject(dc, cache->noPen);

    for (int row = 0; row < totalRows; row++) {
        BOOL isMaster = (row == ml->count);
        BOOL enabled = frame.enabled[row];
        int pct = frame.percent[row];
        WCHAR label[180];
        WCHAR pctStr[8];

        if (isMaster) {
            wcscpy(label, frame.selectedOnly ? L"Selected Monitors" : L"All Monitors");
        } else if (frame.excluded[row]) {
            wsprintfW(label, L"%s (excluded)", frame.name[row]);
        } else if (frame.sourceBlocked[row]) {
            wsprintfW(label, frame.sourceUnknown[row] ? L"%s (input unverified)" :
                                                       L"%s (other input)", frame.name[row]);
        } else {
            wcscpy(label, frame.name[row]);
        }

        wsprintfW(pctStr, L"%d%%", pct);

        if (isMaster) {
            int sepY = POPUP_PADDING + 24 + row * POPUP_ROW_H + 2;
            RECT rcSep = { POPUP_PADDING, sepY, w - POPUP_PADDING, sepY + 1 };
            FillRect(dc, &rcSep, trackBrush);
        }

        int labelY = POPUP_PADDING + 24 + row * POPUP_ROW_H + 6;
        RECT rcLabel = { POPUP_PADDING + 4, labelY, w - POPUP_PADDING - 40, labelY + 18 };
        SelectObject(dc, isMaster ? hFontBold : hFont);
        SetTextColor(dc, UI_ColorRef(!enabled ? CLR_SUBTEXT :
                                    isMaster ? CLR_ACCENT : CLR_TEXT));
        DrawTextW(dc, label, -1, &rcLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT rcPct = { w - POPUP_PADDING - 40, labelY, w - POPUP_PADDING - 4, labelY + 18 };
        SelectObject(dc, hFontSmall);
        SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
        DrawTextW(dc, pctStr, -1, &rcPct, DT_RIGHT | DT_SINGLELINE);

        RECT rcSlider;
        GetSliderRect(row, &rcSlider);

        HBRUSH oldBr = (HBRUSH)SelectObject(dc, trackBrush);
        RoundRect(dc, rcSlider.left, rcSlider.top, rcSlider.right, rcSlider.bottom,
                  SLIDER_TRACK_H, SLIDER_TRACK_H);

        int thumbX = XFromPercent(&rcSlider, pct);
        SelectObject(dc, enabled ? fillBrush : trackBrush);
        RoundRect(dc, rcSlider.left, rcSlider.top, thumbX, rcSlider.bottom,
                  SLIDER_TRACK_H, SLIDER_TRACK_H);
        SelectObject(dc, enabled ? oldBr : trackBrush);

        int cy = (rcSlider.top + rcSlider.bottom) / 2;
        Ellipse(dc, thumbX - SLIDER_THUMB_R, cy - SLIDER_THUMB_R,
                thumbX + SLIDER_THUMB_R, cy + SLIDER_THUMB_R);
        SelectObject(dc, oldBr);

        /* Delta controls (skip master row) */
        if (!isMaster) {
            RECT rcMinus, rcValue, rcPlus;
            GetDeltaButtonRects(row, &rcMinus, &rcValue, &rcPlus);

            /* [-] button */
            FillRect(dc, &rcMinus, cache->surface);
            FillRect(dc, &rcPlus, cache->surface);

            SelectObject(dc, hFontSmall);
            SetTextColor(dc, UI_ColorRef(enabled ? CLR_SUBTEXT : CLR_TRACK));
            DrawTextW(dc, L"\x2013", -1, &rcMinus, DT_CENTER | DT_VCENTER | DT_SINGLELINE); /* en dash as minus */
            DrawTextW(dc, L"+", -1, &rcPlus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            /* Upper end of this monitor's brightness range. */
            WCHAR deltaStr[16];
            wsprintfW(deltaStr, L"Max %d%%", frame.rangeHi[row]);
            SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
            DrawTextW(dc, deltaStr, -1, &rcValue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    if (pd->focusVisible && pd->focusItem >= 0 && pd->focusItem < ItemCount(pd)) {
        RECT focus;
        GetItemFocusRect(pd, pd->focusItem, &focus);
        DrawFocusRing(dc, &focus, 8);
    }

    RestoreDC(dc, savedDC);
    GdiFlush();

    /* Premultiply freshly drawn RGB using the cached corner mask. */
    for (int i = 0; i < w * h; i++) {
        BYTE *pixel = cache->bits + i * 4;
        BYTE alpha = cache->alpha[i];
        pixel[0] = (BYTE)((pixel[0] * alpha + 127) / 255);
        pixel[1] = (BYTE)((pixel[1] * alpha + 127) / 255);
        pixel[2] = (BYTE)((pixel[2] * alpha + 127) / 255);
        pixel[3] = alpha;
    }
    if (UI_CommitLayered(hwnd, dc, w, h)) {
        cache->frame = frame;
        cache->frameValid = TRUE;
    } else {
        cache->frameValid = FALSE;
    }
}

/* ---- Popup hit testing ---- */

static int HitTestSlider(PopupData *pd, int x, int y)
{
    int totalRows = pd->ml->count + 1;
    for (int row = 0; row < totalRows; row++) {
        if (!CanAdjustSlider(pd, row)) continue;
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
    if (!CanAdjustSlider(pd, row)) return;
    BOOL isMaster = (row == ml->count);
    int target = percent;

    if (isMaster) {
        pd->masterPercent = percent;
        Monitor_SetAllBrightness(ml, target);
    } else {
        Monitor_SetBrightness(&ml->monitors[row], (DWORD)percent);
        pd->masterPercent = GetMasterPercent(ml);
    }

    if (g_manualChangeCb) g_manualChangeCb(isMaster ? -1 : row, target);
}

/* Capture can be lost without receiving button-up (for example on Alt-Tab).
   All changed values are queued while moving; closing the drag also submits
   its final target and clears state before releasing capture reenters us. */
static void FinishSliderDrag(HWND hwnd, PopupData *pd, int releasePercent)
{
    if (!pd || pd->activeSlider < 0) return;
    int row = pd->activeSlider;
    int percent = releasePercent >= 0 ? releasePercent : pd->dragPercent;
    pd->activeSlider = -1;
    pd->dragPercent = -1;
    if (percent >= 0)
        ApplySliderValue(pd, row, percent);
    if (GetCapture() == hwnd)
        ReleaseCapture();
    Monitor_RefreshBrightness(pd->ml);
    if (row != pd->ml->count)
        pd->masterPercent = GetMasterPercent(pd->ml);
    RenderPopup(hwnd, pd);
}

/* ---- Keyboard ---- */

static void SetFocusItem(HWND hwnd, PopupData *pd, int item)
{
    int n = ItemCount(pd);
    pd->focusItem = (item % n + n) % n;   /* wrap both ways */
    RenderPopup(hwnd, pd);
    A11y_NotifyFocus(hwnd, pd->focusItem);
}

/* The worker coalesces targets while the UI follows each key repeat. */
static void KeyAdjustSlider(HWND hwnd, PopupData *pd, int row, int pct)
{
    if (!CanAdjustSlider(pd, row)) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == RowPercent(pd, row)) return;
    pd->activeSlider = row;
    pd->keyDrag = TRUE;
    pd->dragPercent = pct;
    ApplySliderValue(pd, row, pct);
    RenderPopup(hwnd, pd);
    A11y_NotifyValue(hwnd, pd->focusItem);
}

static void EndKeyDrag(HWND hwnd, PopupData *pd)
{
    if (!pd || !pd->keyDrag) return;
    pd->keyDrag = FALSE;
    FinishSliderDrag(hwnd, pd, -1);
}

/* Set a monitor's level at All Monitors 100%. The low end comes from
   Settings, and the range keeps its minimum width. */
static void AdjustMax(HWND hwnd, PopupData *pd, int row, int value)
{
    if (!CanAdjustSlider(pd, row) || row >= pd->ml->count) return;
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

    if (!CanAdjustSlider(pd, row))
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
        LRESULT result;
        if (A11y_HandleGetObject(hwnd, wParam, lParam, &result)) return result;
        break;
    }

    case WM_KEYDOWN:
        if (pd) PopupKeyDown(hwnd, pd, wParam);
        return 0;

    case WM_KEYUP:
        if (pd) EndKeyDrag(hwnd, pd);
        return 0;

    case WM_CLOSE:
        UI_HidePopup(hwnd);
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
            SetCapture(hwnd);
            RenderPopup(hwnd, pd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!pd || pd->activeSlider < 0 || pd->keyDrag) break;
        if (!(wParam & MK_LBUTTON)) {
            FinishSliderDrag(hwnd, pd, -1);
            return 0;
        }
        int x = GET_X_LPARAM(lParam);
        int row = pd->activeSlider;
        RECT rc;
        GetSliderRect(row, &rc);
        int pct = PercentFromX(&rc, x);
        if (pct == pd->dragPercent) return 0;
        pd->dragPercent = pct;
        ApplySliderValue(pd, row, pct);
        RenderPopup(hwnd, pd);
        return 0;
    }

    case WM_LBUTTONUP:
        if (pd && pd->activeSlider >= 0) {
            RECT rc;
            GetSliderRect(pd->activeSlider, &rc);
            FinishSliderDrag(hwnd, pd, PercentFromX(&rc, GET_X_LPARAM(lParam)));
        }
        return 0;

    case WM_CAPTURECHANGED:
        if ((HWND)lParam != hwnd)
            FinishSliderDrag(hwnd, pd, -1);
        return 0;

    case WM_CANCELMODE:
        FinishSliderDrag(hwnd, pd, -1);
        return 0;

    case WM_DESTROY:
        A11y_Detach(hwnd);
        if (pd) {
            pd->keyDrag = FALSE;
            pd->activeSlider = -1;
            pd->dragPercent = -1;
        }
        ReleasePopupRenderCache();
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

/* ---- Popup lifecycle and public API ---- */

BOOL UI_PopupInit(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PopupWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = POPUP_CLASS;
    if (!RegisterClassExW(&wc))
        return FALSE;

    return TRUE;
}

void UI_PopupShutdown(void)
{
    ReleasePopupRenderCache();
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
    g_popupData.focusItem = MasterItem(&g_popupData);

    int h = GetPopupHeight(&g_popupData);
    SetWindowPos(hwnd, NULL, 0, 0, POPUP_WIDTH, h, SWP_NOMOVE | SWP_NOZORDER);

    g_popupData.focusVisible = fromKeyboard;

    /* Position near the tray anchor or cursor, adjusted to stay on-screen. */
    POINT pt;
    if (anchor) pt = *anchor;
    else GetCursorPos(&pt);

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { 0 };
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    APPBARDATA abd = { 0 };
    abd.cbSize = sizeof(abd);
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
        /* Bottom taskbar (default) â€” place above cursor */
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
    EndKeyDrag(hwnd, &g_popupData);
    FinishSliderDrag(hwnd, &g_popupData, -1);
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

void UI_SetMasterTarget(int target)
{
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    if (g_masterTargetKnown && g_masterTarget == target) return;
    g_masterTargetKnown = TRUE;
    g_masterTarget = target;
    if (g_popupData.ml)
        g_popupData.masterPercent = target;
    g_popupRender.frameValid = FALSE;
}

void UI_RefreshPopup(HWND hwnd, MonitorList *ml)
{
    if (!hwnd || !IsWindowVisible(hwnd)) return;
    int oldItem = g_popupData.focusItem;
    int oldRow = ItemRow(oldItem);
    int before = g_popupRender.frameValid && oldRow >= 0 && oldRow <= g_popupRender.frame.count
        ? g_popupRender.frame.percent[oldRow] : -1;
    g_popupData.ml = ml;
    if (g_popupData.focusItem >= ItemCount(&g_popupData))
        g_popupData.focusItem = MasterItem(&g_popupData);
    if (g_popupData.activeSlider >= 0 && !CanAdjustSlider(&g_popupData, g_popupData.activeSlider)) {
        g_popupData.keyDrag = FALSE;
        FinishSliderDrag(hwnd, &g_popupData, -1);
    }
    if (g_popupData.activeSlider != ml->count)
        g_popupData.masterPercent = GetMasterPercent(ml);
    RenderPopup(hwnd, &g_popupData);
    if (oldItem == g_popupData.focusItem && !ItemIsOffset(&g_popupData, oldItem) &&
        RowPercent(&g_popupData, ItemRow(oldItem)) != before)
        A11y_NotifyValue(hwnd, oldItem);
}

BOOL UiPopup_Init(HINSTANCE hInst)
{
    return UI_PopupInit(hInst);
}
