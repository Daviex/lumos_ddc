#include "ui.h"
#include "resource.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windowsx.h>

static HINSTANCE g_hInst;
static const WCHAR POPUP_CLASS[]   = L"LumosPopup";
static const WCHAR OSD_CLASS[]     = L"LumosOSD";
static const WCHAR CTXMENU_CLASS[] = L"LumosCtxMenu";
static const WCHAR SCHED_CLASS[]   = L"LumosSched";
static const WCHAR ABOUT_CLASS[]   = L"LumosAbout";
static const WCHAR SET_CLASS[]     = L"LumosSettings";
static HWND g_osdHwnd = NULL;
static HWND g_ctxHwnd = NULL;
static HWND g_schedHwnd = NULL;
static HWND g_aboutHwnd = NULL;
static HWND g_setHwnd = NULL;
static RECT g_aboutLinkRect = { 0, 0, 0, 0 };  /* hit rect for the repo link */

/* ---- Popup data ---- */

typedef struct {
    MonitorList *ml;
    int activeSlider;
    int masterPercent;
    int dragPercent;
} PopupData;

typedef struct {
    int count;
    int percent[MAX_MONITORS + 1];
    int delta[MAX_MONITORS];
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

/* Forward declarations */
static void GetDeltaRange(MonitorList *ml, int *outMin, int *outMax);
static int MasterTargetToSlider(MonitorList *ml, int target);
static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void RenderAbout(HWND hwnd);

/* ---- Color helpers ---- */

static COLORREF HexToColorRef(DWORD rgb)
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static int GetMonPercent(BrightMonitor *mon)
{
    DWORD range = mon->brightnessMax - mon->brightnessMin;
    if (range == 0) return 0;
    return (int)(((ULONGLONG)(mon->brightnessCur - mon->brightnessMin) * 100) / range);
}

static int GetMasterPercent(MonitorList *ml)
{
    if (g_masterTargetKnown)
        return MasterTargetToSlider(ml, g_masterTarget);
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

/* ---- 32-bit DIB helpers for layered windows ---- */

static HDC CreateAlphaDC(int w, int h, HBITMAP *outBmp, BYTE **outBits)
{
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = CreateCompatibleDC(NULL);
    *outBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)outBits, NULL, 0);
    SelectObject(hdc, *outBmp);
    return hdc;
}

/* Apply rounded corner mask with anti-aliasing and premultiply alpha */
static void ApplyRoundedMask(BYTE *bits, int w, int h, int radius, BYTE baseAlpha)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            BYTE *px = bits + (y * w + x) * 4;
            BYTE alpha = baseAlpha;

            BOOL inCorner = FALSE;
            int cx = 0, cy = 0;
            if      (x < radius && y < radius)              { cx = radius;     cy = radius;     inCorner = TRUE; }
            else if (x >= w - radius && y < radius)          { cx = w - radius; cy = radius;     inCorner = TRUE; }
            else if (x < radius && y >= h - radius)          { cx = radius;     cy = h - radius; inCorner = TRUE; }
            else if (x >= w - radius && y >= h - radius)     { cx = w - radius; cy = h - radius; inCorner = TRUE; }

            if (inCorner) {
                float dx = (float)x - (float)cx + 0.5f;
                float dy = (float)y - (float)cy + 0.5f;
                float dist = sqrtf(dx * dx + dy * dy);
                float r = (float)radius;
                if (dist > r + 0.5f)
                    alpha = 0;
                else if (dist > r - 0.5f)
                    alpha = (BYTE)((r + 0.5f - dist) * baseAlpha);
            }

            /* Premultiply RGB by alpha */
            px[0] = (BYTE)((px[0] * alpha + 127) / 255);
            px[1] = (BYTE)((px[1] * alpha + 127) / 255);
            px[2] = (BYTE)((px[2] * alpha + 127) / 255);
            px[3] = alpha;
        }
    }
}

/* Call UpdateLayeredWindow with a 32-bit surface */
static BOOL CommitLayered(HWND hwnd, HDC memDC, int w, int h)
{
    POINT ptSrc = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    return UpdateLayeredWindow(hwnd, NULL, NULL, &sz, memDC, &ptSrc, 0, &bf, ULW_ALPHA);
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
        cache->background = CreateSolidBrush(HexToColorRef(CLR_BG));
        cache->track = CreateSolidBrush(HexToColorRef(CLR_TRACK));
        cache->accent = CreateSolidBrush(HexToColorRef(CLR_ACCENT));
        cache->surface = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
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
    for (int row = 0; row <= frame->count; row++) {
        if (row == frame->count) {
            frame->percent[row] = pd->masterPercent;
        } else {
            BrightMonitor *mon = &pd->ml->monitors[row];
            frame->percent[row] = GetMonPercent(mon);
            frame->delta[row] = mon->delta;
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
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    RECT rcTitle = { POPUP_PADDING, POPUP_PADDING, w - POPUP_PADDING, POPUP_PADDING + 20 };
    DrawTextW(dc, L"Brightness", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);

    MonitorList *ml = pd->ml;
    int totalRows = ml->count + 1;
    HBRUSH trackBrush = cache->track;
    HBRUSH fillBrush = cache->accent;
    SelectObject(dc, cache->noPen);

    for (int row = 0; row < totalRows; row++) {
        BOOL isMaster = (row == ml->count);
        int pct = frame.percent[row];
        WCHAR label[140];
        WCHAR pctStr[8];

        if (isMaster) {
            wcscpy(label, L"All Monitors");
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
            FillRect(dc, &rcMinus, cache->surface);
            FillRect(dc, &rcPlus, cache->surface);

            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, L"\x2013", -1, &rcMinus, DT_CENTER | DT_VCENTER | DT_SINGLELINE); /* en dash as minus */
            DrawTextW(dc, L"+", -1, &rcPlus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            /* Delta value */
            WCHAR deltaStr[16];
            int d = frame.delta[row];
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
    if (CommitLayered(hwnd, dc, w, h)) {
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
    if (row < 0 || row > ml->count) return;
    BOOL isMaster = (row == ml->count);
    int target = isMaster ? SliderToMasterTarget(ml, percent) : percent;

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
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

        /* Check delta buttons first */
        int deltaRow;
        int deltaDir = HitTestDelta(pd, x, y, &deltaRow);
        if (deltaDir != 0 && deltaRow >= 0) {
            BrightMonitor *mon = &pd->ml->monitors[deltaRow];
            int oldDelta = mon->delta;
            mon->delta += deltaDir;
            if (mon->delta < -40) mon->delta = -40;
            if (mon->delta > 40) mon->delta = 40;
            if (mon->delta == oldDelta) return 0;
            if (g_deltaSaveCb) g_deltaSaveCb();
            pd->masterPercent = GetMasterPercent(pd->ml);
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
            SetCapture(hwnd);
            RenderPopup(hwnd, pd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!pd || pd->activeSlider < 0) break;
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
        if (pd) {
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

/* ---- OSD ---- */

#define OSD_TIMER_FADE   1
#define OSD_TIMER_SHOW   2
#define OSD_W            200
#define OSD_H            56
#define OSD_CORNER       12
#define OSD_BAR_H        6
#define OSD_BASE_ALPHA   210
#define OSD_FADE_STEP    20
#define OSD_FADE_MS      25
#define OSD_SHOW_MS      900

typedef struct {
    int    percent;
    BYTE   alpha;
    BYTE   targetAlpha;
    BOOL   fadingIn;
} OsdData;

static OsdData g_osd;

static void RenderOSD(HWND hwnd)
{
    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(OSD_W, OSD_H, &bmp, &bits);

    /* Background */
    HBRUSH bg = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, OSD_W, OSD_H };
    FillRect(dc, &rcAll, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);

    /* Percentage text */
    WCHAR pctStr[8];
    wsprintfW(pctStr, L"%d%%", g_osd.percent);

    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    HFONT hf = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(dc, hf);
    RECT rcText = { 0, 6, OSD_W, 30 };
    DrawTextW(dc, pctStr, -1, &rcText, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(hf);

    /* Progress bar */
    int barL = 16, barR = OSD_W - 16;
    int barY = 36;
    int barW = barR - barL;
    int fillW = (barW * g_osd.percent) / 100;

    /* Track */
    HBRUSH trackBr = CreateSolidBrush(HexToColorRef(CLR_TRACK));
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, trackBr);
    RoundRect(dc, barL, barY, barR, barY + OSD_BAR_H, OSD_BAR_H, OSD_BAR_H);

    /* Fill */
    HBRUSH fillBr = CreateSolidBrush(HexToColorRef(CLR_ACCENT));
    SelectObject(dc, fillBr);
    if (fillW > 0)
        RoundRect(dc, barL, barY, barL + fillW, barY + OSD_BAR_H, OSD_BAR_H, OSD_BAR_H);

    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(trackBr);
    DeleteObject(fillBr);

    /* Apply rounded mask with current alpha */
    ApplyRoundedMask(bits, OSD_W, OSD_H, OSD_CORNER, g_osd.alpha);
    CommitLayered(hwnd, dc, OSD_W, OSD_H);

    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK OsdWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TIMER:
        if (wParam == OSD_TIMER_SHOW) {
            /* Show period ended, start fade out */
            KillTimer(hwnd, OSD_TIMER_SHOW);
            g_osd.targetAlpha = 0;
            g_osd.fadingIn = FALSE;
            SetTimer(hwnd, OSD_TIMER_FADE, OSD_FADE_MS, NULL);
            return 0;
        }
        if (wParam == OSD_TIMER_FADE) {
            if (g_osd.fadingIn) {
                /* Fade in */
                int next = (int)g_osd.alpha + OSD_FADE_STEP * 2;
                if (next >= g_osd.targetAlpha) {
                    g_osd.alpha = g_osd.targetAlpha;
                    g_osd.fadingIn = FALSE;
                    KillTimer(hwnd, OSD_TIMER_FADE);
                    SetTimer(hwnd, OSD_TIMER_SHOW, OSD_SHOW_MS, NULL);
                } else {
                    g_osd.alpha = (BYTE)next;
                }
            } else {
                /* Fade out */
                if (g_osd.alpha <= OSD_FADE_STEP) {
                    g_osd.alpha = 0;
                    KillTimer(hwnd, OSD_TIMER_FADE);
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                }
                g_osd.alpha -= OSD_FADE_STEP;
            }
            RenderOSD(hwnd);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---- Context Menu ---- */

#define CTX_ITEM_NORMAL    0
#define CTX_ITEM_SEPARATOR 1

typedef struct {
    int   type;
    int   id;
    WCHAR label[80];
    BOOL  checked;
} CtxMenuItem;

#define MAX_CTX_ITEMS 20

typedef struct {
    CtxMenuItem items[MAX_CTX_ITEMS];
    int count;
    int hoverIndex;
    HWND hwndOwner;
} CtxMenuData;

static CtxMenuData g_ctxData;

static void BuildContextMenu(CtxMenuData *d, Settings *s)
{
    d->count = 0;
    d->hoverIndex = -1;

    /* Presets as flat items */
    for (int i = 0; i < s->presetCount && d->count < MAX_CTX_ITEMS; i++) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_PRESET_BASE + i;
        wsprintfW(it->label, L"%s (%u%%)", s->presets[i].name, s->presets[i].brightness);
        it->checked = FALSE;
    }

    /* Separator */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_SEPARATOR;
        it->id = 0;
        it->label[0] = 0;
        it->checked = FALSE;
    }

    /* Re-scan */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_RESCAN;
        wcscpy(it->label, L"Re-scan Monitors");
        it->checked = FALSE;
    }

    /* Settings */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_SETTINGS;
        wcscpy(it->label, L"Settings...");
        it->checked = FALSE;
    }

    /* Autostart */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_AUTOSTART;
        wcscpy(it->label, L"Start with Windows");
        it->checked = Settings_GetAutostart();
    }

    /* Schedule toggle */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_SCHEDULE_TOGGLE;
        wcscpy(it->label, L"Brightness Schedule");
        it->checked = s->scheduleEnabled;
    }

    /* Edit schedule */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_SCHEDULE_EDIT;
        wcscpy(it->label, L"Edit Schedule...");
        it->checked = FALSE;
    }

    /* Idle auto-dim toggle. The level and the timeout live in config.ini. */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_IDLEDIM_TOGGLE;
        wsprintfW(it->label, L"Dim When Idle (%d%%/%dm)",
                  s->idleDimPercent, s->idleDimMinutes);
        it->checked = s->idleDimEnabled;
    }

    /* Separator */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_SEPARATOR;
        it->id = 0;
        it->label[0] = 0;
        it->checked = FALSE;
    }

    /* About */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_ABOUT;
        wcscpy(it->label, L"About " APP_NAME);
        it->checked = FALSE;
    }

    /* Exit */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_EXIT;
        wcscpy(it->label, L"Exit");
        it->checked = FALSE;
    }
}

static int GetCtxMenuHeight(CtxMenuData *d)
{
    int h = CTXMENU_PAD * 2;
    for (int i = 0; i < d->count; i++)
        h += (d->items[i].type == CTX_ITEM_SEPARATOR) ? CTXMENU_SEP_H : CTXMENU_ITEM_H;
    return h;
}

static int CtxMenuHitTest(CtxMenuData *d, int y)
{
    int cy = CTXMENU_PAD;
    for (int i = 0; i < d->count; i++) {
        int ih = (d->items[i].type == CTX_ITEM_SEPARATOR) ? CTXMENU_SEP_H : CTXMENU_ITEM_H;
        if (y >= cy && y < cy + ih) {
            return (d->items[i].type == CTX_ITEM_SEPARATOR) ? -1 : i;
        }
        cy += ih;
    }
    return -1;
}

static void RenderContextMenu(HWND hwnd, CtxMenuData *d)
{
    int w = CTXMENU_WIDTH;
    int h = GetCtxMenuHeight(d);

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    /* Background */
    HBRUSH bgBrush = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(dc, TRANSPARENT);
    HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontCheck = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(dc, hFont);

    int cy = CTXMENU_PAD;
    for (int i = 0; i < d->count; i++) {
        CtxMenuItem *it = &d->items[i];

        if (it->type == CTX_ITEM_SEPARATOR) {
            /* Horizontal line */
            int lineY = cy + CTXMENU_SEP_H / 2;
            HBRUSH sepBrush = CreateSolidBrush(HexToColorRef(CLR_TRACK));
            RECT rcSep = { 12, lineY, w - 12, lineY + 1 };
            FillRect(dc, &rcSep, sepBrush);
            DeleteObject(sepBrush);
            cy += CTXMENU_SEP_H;
            continue;
        }

        /* Hover highlight */
        if (i == d->hoverIndex) {
            HBRUSH hoverBrush = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            HPEN oldPen = (HPEN)SelectObject(dc, noPen);
            HBRUSH oldBr = (HBRUSH)SelectObject(dc, hoverBrush);
            RoundRect(dc, 4, cy + 2, w - 4, cy + CTXMENU_ITEM_H - 2, 8, 8);
            SelectObject(dc, oldBr);
            SelectObject(dc, oldPen);
            DeleteObject(noPen);
            DeleteObject(hoverBrush);
        }

        int textX = 14;

        /* Checkmark */
        if (it->checked) {
            SelectObject(dc, hFontCheck);
            SetTextColor(dc, HexToColorRef(CLR_ACCENT));
            RECT rcCheck = { textX - 2, cy, textX + 14, cy + CTXMENU_ITEM_H };
            DrawTextW(dc, L"\x2713", 1, &rcCheck, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            textX += 18;
        }

        /* Label */
        SelectObject(dc, hFont);
        SetTextColor(dc, HexToColorRef(CLR_TEXT));
        RECT rcLabel = { textX, cy, w - 12, cy + CTXMENU_ITEM_H };
        DrawTextW(dc, it->label, -1, &rcLabel,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        cy += CTXMENU_ITEM_H;
    }

    SelectObject(dc, oldFont);
    DeleteObject(hFont);
    DeleteObject(hFontCheck);

    ApplyRoundedMask(bits, w, h, CTXMENU_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK CtxMenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CtxMenuData *d = &g_ctxData;

    switch (msg) {
    case WM_MOUSEMOVE: {
        int y = (short)HIWORD(lParam);
        int idx = CtxMenuHitTest(d, y);
        if (idx != d->hoverIndex) {
            d->hoverIndex = idx;
            RenderContextMenu(hwnd, d);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (d->hoverIndex != -1) {
            d->hoverIndex = -1;
            RenderContextMenu(hwnd, d);
        }
        return 0;

    case WM_LBUTTONUP: {
        int y = (short)HIWORD(lParam);
        int idx = CtxMenuHitTest(d, y);
        if (idx >= 0 && idx < d->count && d->items[idx].type == CTX_ITEM_NORMAL) {
            int id = d->items[idx].id;
            HWND owner = d->hwndOwner;
            DestroyWindow(hwnd);
            g_ctxHwnd = NULL;
            PostMessageW(owner, WM_COMMAND, (WPARAM)id, 0);
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            DestroyWindow(hwnd);
            g_ctxHwnd = NULL;
        }
        return 0;

    case WM_DESTROY:
        g_ctxHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---- Schedule editor ---- */

enum { SF_HOUR = 0, SF_MIN = 1, SF_BRI = 2, SF_DEL = 3 };  /* field kinds */

typedef struct {
    SchedulePoint pts[MAX_SCHEDULE];
    int      count;
    int      selectedRow;
    Settings *settings;
    HWND     owner;
} SchedEditData;

static SchedEditData g_sched;

static int SchedHeight(SchedEditData *d)
{
    return SCHED_HEADER_H + d->count * SCHED_ROW_H + SCHED_FOOTER_H;
}

/* Field rects for a row. x zones: HH | MM | NN% | [x] */
static void SchedFieldRect(int row, int field, RECT *rc)
{
    int y = SCHED_HEADER_H + row * SCHED_ROW_H;
    rc->top = y + 4;
    rc->bottom = y + SCHED_ROW_H - 4;
    switch (field) {
        case SF_HOUR: rc->left = 16;  rc->right = 56;  break;
        case SF_MIN:  rc->left = 64;  rc->right = 104; break;
        case SF_BRI:  rc->left = 128; rc->right = 196; break;
        case SF_DEL:  rc->left = SCHED_WIDTH - 40; rc->right = SCHED_WIDTH - 16; break;
        default:      rc->left = 0;   rc->right = 0;   break;
    }
}

/* Footer button rects: Add (left), Save (right). */
static void SchedButtonRects(SchedEditData *d, RECT *rcAdd, RECT *rcSave)
{
    int y = SCHED_HEADER_H + d->count * SCHED_ROW_H + 8;
    rcAdd->left = 16;  rcAdd->right = 120;
    rcAdd->top = y;    rcAdd->bottom = y + 28;
    rcSave->right = SCHED_WIDTH - 16; rcSave->left = SCHED_WIDTH - 120;
    rcSave->top = y;   rcSave->bottom = y + 28;
}

/* Returns row index and sets *outField, or -1. */
static int SchedHitField(SchedEditData *d, int x, int y, int *outField)
{
    for (int row = 0; row < d->count; row++) {
        for (int f = SF_HOUR; f <= SF_DEL; f++) {
            RECT rc;
            SchedFieldRect(row, f, &rc);
            if (x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom) {
                *outField = f;
                return row;
            }
        }
    }
    *outField = -1;
    return -1;
}

/* Adjust a field by +/- delta with wrap/clamp. */
static void SchedAdjust(SchedEditData *d, int row, int field, int dir)
{
    if (row < 0 || row >= d->count) return;
    SchedulePoint *p = &d->pts[row];
    int h = p->minutes / 60, m = p->minutes % 60;
    if (field == SF_HOUR) {
        h = (h + dir + 24) % 24;
        p->minutes = h * 60 + m;
    } else if (field == SF_MIN) {
        m += dir * 5;                 /* 5-minute granularity */
        while (m < 0)  { m += 60; }
        while (m >= 60){ m -= 60; }
        p->minutes = h * 60 + m;
    } else if (field == SF_BRI) {
        int b = p->brightness + dir * 5;
        if (b < 0) b = 0;
        if (b > 100) b = 100;
        p->brightness = b;
    }
}

static void RenderSchedEditor(HWND hwnd, SchedEditData *d)
{
    int w = SCHED_WIDTH;
    int h = SchedHeight(d);

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    HBRUSH bg = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bg);
    DeleteObject(bg);

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

    /* Title + hint */
    SelectObject(dc, hFontBold);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    RECT rcTitle = { 16, 12, w - 16, 32 };
    DrawTextW(dc, L"Brightness Schedule", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    RECT rcHint = { 16, 12, w - 16, 32 };
    DrawTextW(dc, L"wheel = adjust", -1, &rcHint, DT_RIGHT | DT_SINGLELINE);

    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);

    for (int row = 0; row < d->count; row++) {
        int y = SCHED_HEADER_H + row * SCHED_ROW_H;

        if (row == d->selectedRow) {
            HBRUSH hb = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
            HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
            RoundRect(dc, 8, y + 2, w - 8, y + SCHED_ROW_H - 2, 8, 8);
            SelectObject(dc, ob);
            DeleteObject(hb);
        }

        SchedulePoint *p = &d->pts[row];
        WCHAR s[16];
        RECT rc;

        SelectObject(dc, hFont);
        SetTextColor(dc, HexToColorRef(CLR_TEXT));
        SchedFieldRect(row, SF_HOUR, &rc);
        wsprintfW(s, L"%02d", p->minutes / 60);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT rcColon = rc; rcColon.left = rc.right; rcColon.right = rc.right + 8;
        DrawTextW(dc, L":", -1, &rcColon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_MIN, &rc);
        wsprintfW(s, L"%02d", p->minutes % 60);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_BRI, &rc);
        SetTextColor(dc, HexToColorRef(CLR_ACCENT));
        wsprintfW(s, L"%d%%", p->brightness);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_DEL, &rc);
        SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
        DrawTextW(dc, L"\x2715", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE); /* x */
    }

    /* Footer buttons */
    RECT rcAdd, rcSave;
    SchedButtonRects(d, &rcAdd, &rcSave);
    HBRUSH btn = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
    HBRUSH acc = CreateSolidBrush(HexToColorRef(CLR_ACCENT));
    HBRUSH ob = (HBRUSH)SelectObject(dc, btn);
    RoundRect(dc, rcAdd.left, rcAdd.top, rcAdd.right, rcAdd.bottom, 8, 8);
    SelectObject(dc, acc);
    RoundRect(dc, rcSave.left, rcSave.top, rcSave.right, rcSave.bottom, 8, 8);
    SelectObject(dc, ob);
    DeleteObject(btn);
    DeleteObject(acc);

    SelectObject(dc, hFont);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    DrawTextW(dc, L"+ Add", -1, &rcAdd, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, HexToColorRef(CLR_BG));
    DrawTextW(dc, L"Save", -1, &rcSave, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(hFont);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

    ApplyRoundedMask(bits, w, h, SCHED_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

static void SchedResize(HWND hwnd, SchedEditData *d)
{
    SetWindowPos(hwnd, NULL, 0, 0, SCHED_WIDTH, SchedHeight(d),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK SchedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SchedEditData *d = &g_sched;

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);

        /* Footer buttons */
        RECT rcAdd, rcSave;
        SchedButtonRects(d, &rcAdd, &rcSave);
        if (x >= rcAdd.left && x <= rcAdd.right && y >= rcAdd.top && y <= rcAdd.bottom) {
            if (d->count < MAX_SCHEDULE) {
                d->pts[d->count].minutes = 12 * 60;  /* default new point 12:00 = 50 */
                d->pts[d->count].brightness = 50;
                d->selectedRow = d->count;
                d->count++;
                SchedResize(hwnd, d);
                RenderSchedEditor(hwnd, d);
            }
            return 0;
        }
        if (x >= rcSave.left && x <= rcSave.right && y >= rcSave.top && y <= rcSave.bottom) {
            Schedule_Sort(d->pts, d->count);
            for (int i = 0; i < d->count; i++)
                d->settings->schedule[i] = d->pts[i];
            d->settings->scheduleCount = d->count;
            HWND owner = d->owner;
            DestroyWindow(hwnd);
            g_schedHwnd = NULL;
            PostMessageW(owner, WM_COMMAND, (WPARAM)IDM_SCHEDULE_SAVED, 0);
            return 0;
        }

        int field;
        int row = SchedHitField(d, x, y, &field);
        if (row >= 0) {
            d->selectedRow = row;
            if (field == SF_DEL) {
                for (int i = row; i < d->count - 1; i++)
                    d->pts[i] = d->pts[i + 1];
                d->count--;
                if (d->selectedRow >= d->count) d->selectedRow = d->count - 1;
                SchedResize(hwnd, d);
            }
            RenderSchedEditor(hwnd, d);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int dir = ((short)HIWORD(wParam) > 0) ? 1 : -1;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        int field;
        int row = SchedHitField(d, pt.x, pt.y, &field);
        if (row >= 0 && field != SF_DEL) {
            d->selectedRow = row;
            SchedAdjust(d, row, field, dir);
            RenderSchedEditor(hwnd, d);
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            /* Dismiss without saving on click-outside. */
            DestroyWindow(hwnd);
            g_schedHwnd = NULL;
        }
        return 0;

    case WM_DESTROY:
        g_schedHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---- Settings window ---- */

/* Rows are data, not code: the table below drives rendering, hit testing and
   editing, so adding a setting later is one BuildSettingsRows line. */
enum { SET_SECTION = 0, SET_TOGGLE, SET_NUMBER };
enum { SET_UNIT_PLAIN = 0, SET_UNIT_PERCENT, SET_UNIT_MINUTES };

/* Hit kinds returned by SetHitTest. */
enum { SETHIT_NONE = 0, SETHIT_MINUS, SETHIT_PLUS, SETHIT_TOGGLE, SETHIT_ROW };

typedef struct {
    int    kind;
    WCHAR  label[MAX_PRESET_NAME + 16];
    int   *ival;      /* SET_NUMBER: the value being edited */
    BOOL  *bval;      /* SET_TOGGLE: the flag being edited */
    int    lo, hi;    /* SET_NUMBER bounds */
    int    step;      /* SET_NUMBER increment (minutes scale instead, see SetStepFor) */
    int    unit;
} SetRow;

#define MAX_SET_ROWS (MAX_PRESETS + 12)

typedef struct {
    /* Working copy. Edits are discarded unless the user hits Save, which is why
       the window never writes into Settings directly. */
    int   step;
    BOOL  autostart;
    BOOL  scheduleEnabled;
    BOOL  idleDimEnabled;
    int   idleDimPercent;
    int   idleDimMinutes;
    int   presetValues[MAX_PRESETS];
    int   presetCount;

    SetRow rows[MAX_SET_ROWS];
    int    rowCount;
    int    hoverRow;
    Settings *settings;
    HWND   owner;
} SetEditData;

static SetEditData g_set;

static SetRow *SetAddRow(SetEditData *d, int kind, const WCHAR *label)
{
    if (d->rowCount >= MAX_SET_ROWS) return NULL;
    SetRow *r = &d->rows[d->rowCount++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    wcsncpy(r->label, label, (sizeof(r->label) / sizeof(WCHAR)) - 1);
    return r;
}

static void SetAddToggle(SetEditData *d, const WCHAR *label, BOOL *val)
{
    SetRow *r = SetAddRow(d, SET_TOGGLE, label);
    if (r) r->bval = val;
}

static void SetAddNumber(SetEditData *d, const WCHAR *label, int *val,
                         int lo, int hi, int step, int unit)
{
    SetRow *r = SetAddRow(d, SET_NUMBER, label);
    if (!r) return;
    r->ival = val;
    r->lo = lo;
    r->hi = hi;
    r->step = step;
    r->unit = unit;
}

static void BuildSettingsRows(SetEditData *d)
{
    d->rowCount = 0;

    SetAddRow(d, SET_SECTION, L"GENERAL");
    SetAddNumber(d, L"Brightness step", &d->step, 1, 50, 1, SET_UNIT_PERCENT);
    SetAddToggle(d, L"Start with Windows", &d->autostart);

    SetAddRow(d, SET_SECTION, L"IDLE DIM");
    SetAddToggle(d, L"Dim when idle", &d->idleDimEnabled);
    SetAddNumber(d, L"Idle level", &d->idleDimPercent, 0, 100, 1, SET_UNIT_PERCENT);
    SetAddNumber(d, L"Dim after", &d->idleDimMinutes, 1, 1440, 5, SET_UNIT_MINUTES);

    SetAddRow(d, SET_SECTION, L"SCHEDULE");
    SetAddToggle(d, L"Brightness schedule", &d->scheduleEnabled);

    if (d->presetCount > 0) {
        SetAddRow(d, SET_SECTION, L"PRESETS");
        for (int i = 0; i < d->presetCount; i++)
            SetAddNumber(d, d->settings->presets[i].name, &d->presetValues[i],
                         0, 100, 5, SET_UNIT_PERCENT);
    }
}

static int SetRowHeight(SetRow *r)
{
    return (r->kind == SET_SECTION) ? SET_SECTION_H : SET_ROW_H;
}

static int SetHeight(SetEditData *d)
{
    int h = SET_HEADER_H + SET_FOOTER_H;
    for (int i = 0; i < d->rowCount; i++)
        h += SetRowHeight(&d->rows[i]);
    return h;
}

/* Number controls sit on the right edge: [-] value [+] */
static void SetControlRects(int top, RECT *rcMinus, RECT *rcValue, RECT *rcPlus)
{
    int t = top + 4, b = top + SET_ROW_H - 4;
    rcMinus->left = SET_WIDTH - 116; rcMinus->right = SET_WIDTH - 92;
    rcValue->left = SET_WIDTH - 90;  rcValue->right = SET_WIDTH - 44;
    rcPlus->left  = SET_WIDTH - 40;  rcPlus->right  = SET_WIDTH - 16;
    rcMinus->top = rcValue->top = rcPlus->top = t;
    rcMinus->bottom = rcValue->bottom = rcPlus->bottom = b;
}

static void SetToggleRect(int top, RECT *rc)
{
    rc->right  = SET_WIDTH - 16;
    rc->left   = rc->right - 36;
    rc->top    = top + (SET_ROW_H - 20) / 2;
    rc->bottom = rc->top + 20;
}

static void SetSaveRect(SetEditData *d, RECT *rc)
{
    int y = SetHeight(d) - SET_FOOTER_H + 10;
    rc->right = SET_WIDTH - 16; rc->left = SET_WIDTH - 112;
    rc->top = y; rc->bottom = y + 28;
}

/* Minutes run from 1 to 1440, so the increment scales with the value: a fixed
   step is either too coarse near 1 or takes hundreds of clicks near 1440. */
static int SetStepFor(SetRow *r, int value)
{
    if (r->unit != SET_UNIT_MINUTES) return r->step;
    if (value < 15) return 1;
    if (value < 60) return 5;
    return 15;
}

static void SetAdjust(SetRow *r, int dir)
{
    if (r->kind != SET_NUMBER || !r->ival) return;
    int v = *r->ival;
    /* Stepping down uses the bucket below the current value, so the same click
       count walks a value back to where it came from. */
    v += (dir > 0) ? SetStepFor(r, v) : -SetStepFor(r, v - 1);
    if (v < r->lo) v = r->lo;
    if (v > r->hi) v = r->hi;
    *r->ival = v;
}

/* Returns the row under (x,y) and sets *outHit, or -1 for none. */
static int SetHitTest(SetEditData *d, int x, int y, int *outHit)
{
    *outHit = SETHIT_NONE;
    int top = SET_HEADER_H;
    for (int i = 0; i < d->rowCount; i++) {
        SetRow *r = &d->rows[i];
        int rh = SetRowHeight(r);
        if (y >= top && y < top + rh) {
            if (r->kind == SET_SECTION) return -1;
            if (r->kind == SET_TOGGLE) {
                RECT rc;
                SetToggleRect(top, &rc);
                *outHit = (x >= rc.left && x <= rc.right) ? SETHIT_TOGGLE : SETHIT_ROW;
                return i;
            }
            RECT rcMinus, rcValue, rcPlus;
            SetControlRects(top, &rcMinus, &rcValue, &rcPlus);
            if (x >= rcMinus.left && x <= rcMinus.right)     *outHit = SETHIT_MINUS;
            else if (x >= rcPlus.left && x <= rcPlus.right)   *outHit = SETHIT_PLUS;
            else                                             *outHit = SETHIT_ROW;
            return i;
        }
        top += rh;
    }
    return -1;
}

static void RenderSettings(HWND hwnd, SetEditData *d)
{
    int w = SET_WIDTH;
    int h = SetHeight(d);

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    HBRUSH bg = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bg);
    DeleteObject(bg);

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

    /* Title + hint */
    RECT rcTitle = { 16, 12, w - 16, 32 };
    SelectObject(dc, hFontBold);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    DrawTextW(dc, L"Settings", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    DrawTextW(dc, L"wheel = adjust", -1, &rcTitle, DT_RIGHT | DT_SINGLELINE);

    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);

    int y = SET_HEADER_H;
    for (int i = 0; i < d->rowCount; i++) {
        SetRow *r = &d->rows[i];

        if (r->kind == SET_SECTION) {
            RECT rc = { 16, y, w - 16, y + SET_SECTION_H };
            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, r->label, -1, &rc, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            y += SET_SECTION_H;
            continue;
        }

        if (i == d->hoverRow) {
            HBRUSH hb = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
            HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
            RoundRect(dc, 8, y + 2, w - 8, y + SET_ROW_H - 2, 8, 8);
            SelectObject(dc, ob);
            DeleteObject(hb);
        }

        RECT rcLabel = { 16, y, w - 124, y + SET_ROW_H };
        SelectObject(dc, hFont);
        SetTextColor(dc, HexToColorRef(CLR_TEXT));
        DrawTextW(dc, r->label, -1, &rcLabel,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (r->kind == SET_TOGGLE) {
            RECT rcT;
            SetToggleRect(y, &rcT);
            BOOL on = (r->bval && *r->bval);
            HBRUSH track = CreateSolidBrush(HexToColorRef(on ? CLR_ACCENT : CLR_TRACK));
            HBRUSH knob  = CreateSolidBrush(HexToColorRef(on ? CLR_BG : CLR_SUBTEXT));
            HBRUSH ob = (HBRUSH)SelectObject(dc, track);
            RoundRect(dc, rcT.left, rcT.top, rcT.right, rcT.bottom, 20, 20);
            SelectObject(dc, knob);
            int kd = (rcT.bottom - rcT.top) - 6;   /* knob diameter, 3px inset */
            int kx = on ? rcT.right - 3 - kd : rcT.left + 3;
            Ellipse(dc, kx, rcT.top + 3, kx + kd, rcT.top + 3 + kd);
            SelectObject(dc, ob);
            DeleteObject(track);
            DeleteObject(knob);
        } else {
            RECT rcMinus, rcValue, rcPlus;
            SetControlRects(y, &rcMinus, &rcValue, &rcPlus);

            HBRUSH btn = CreateSolidBrush(HexToColorRef(CLR_TRACK));
            HBRUSH ob = (HBRUSH)SelectObject(dc, btn);
            RoundRect(dc, rcMinus.left, rcMinus.top, rcMinus.right, rcMinus.bottom, 6, 6);
            RoundRect(dc, rcPlus.left, rcPlus.top, rcPlus.right, rcPlus.bottom, 6, 6);
            SelectObject(dc, ob);
            DeleteObject(btn);

            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, L"\x2013", -1, &rcMinus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, L"+", -1, &rcPlus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            WCHAR val[16];
            int v = r->ival ? *r->ival : 0;
            if (r->unit == SET_UNIT_MINUTES)      wsprintfW(val, L"%dm", v);
            else if (r->unit == SET_UNIT_PERCENT) wsprintfW(val, L"%d%%", v);
            else                                  wsprintfW(val, L"%d", v);
            SelectObject(dc, hFont);
            SetTextColor(dc, HexToColorRef(CLR_ACCENT));
            DrawTextW(dc, val, -1, &rcValue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        y += SET_ROW_H;
    }

    /* Footer: Save on the right, cancel hint on the left */
    RECT rcSave;
    SetSaveRect(d, &rcSave);
    HBRUSH acc = CreateSolidBrush(HexToColorRef(CLR_ACCENT));
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, acc);
    RoundRect(dc, rcSave.left, rcSave.top, rcSave.right, rcSave.bottom, 8, 8);
    SelectObject(dc, oldBr);
    DeleteObject(acc);

    SelectObject(dc, hFont);
    SetTextColor(dc, HexToColorRef(CLR_BG));
    DrawTextW(dc, L"Save", -1, &rcSave, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT rcNote = { 16, rcSave.top, rcSave.left - 8, rcSave.bottom };
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    DrawTextW(dc, L"click outside to cancel", -1, &rcNote,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(hFont);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

    ApplyRoundedMask(bits, w, h, SET_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

/* Copy the working values back into Settings. Only the fields this window owns
   are touched; the schedule points stay with the schedule editor. */
static void SetCommit(SetEditData *d)
{
    Settings *s = d->settings;
    s->step            = d->step;
    s->autostart       = d->autostart;
    s->scheduleEnabled = d->scheduleEnabled;
    s->idleDimEnabled  = d->idleDimEnabled;
    s->idleDimPercent  = d->idleDimPercent;
    s->idleDimMinutes  = d->idleDimMinutes;
    for (int i = 0; i < d->presetCount && i < s->presetCount; i++)
        s->presets[i].brightness = (DWORD)d->presetValues[i];
}

static LRESULT CALLBACK SetWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SetEditData *d = &g_set;

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);

        RECT rcSave;
        SetSaveRect(d, &rcSave);
        if (x >= rcSave.left && x <= rcSave.right && y >= rcSave.top && y <= rcSave.bottom) {
            SetCommit(d);
            HWND owner = d->owner;
            DestroyWindow(hwnd);
            g_setHwnd = NULL;
            PostMessageW(owner, WM_COMMAND, (WPARAM)IDM_SETTINGS_SAVED, 0);
            return 0;
        }

        int hit;
        int row = SetHitTest(d, x, y, &hit);
        if (row >= 0) {
            SetRow *r = &d->rows[row];
            if (hit == SETHIT_TOGGLE && r->bval) *r->bval = !*r->bval;
            else if (hit == SETHIT_MINUS)        SetAdjust(r, -1);
            else if (hit == SETHIT_PLUS)         SetAdjust(r, +1);
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int hit;
        int row = SetHitTest(d, LOWORD(lParam), HIWORD(lParam), &hit);
        if (row != d->hoverRow) {
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int dir = ((short)HIWORD(wParam) > 0) ? 1 : -1;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        int hit;
        int row = SetHitTest(d, pt.x, pt.y, &hit);
        if (row >= 0 && d->rows[row].kind == SET_NUMBER) {
            SetAdjust(&d->rows[row], dir);
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            /* Dismiss without saving on click-outside, like the schedule editor. */
            DestroyWindow(hwnd);
            g_setHwnd = NULL;
        }
        return 0;

    case WM_DESTROY:
        g_setHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---- Public API ---- */

BOOL UI_Init(HINSTANCE hInst)
{
    g_hInst = hInst;

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PopupWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = POPUP_CLASS;
    if (!RegisterClassExW(&wc))
        return FALSE;

    WNDCLASSEXW wcOsd = { 0 };
    wcOsd.cbSize = sizeof(wcOsd);
    wcOsd.lpfnWndProc = OsdWndProc;
    wcOsd.hInstance = hInst;
    wcOsd.lpszClassName = OSD_CLASS;
    RegisterClassExW(&wcOsd);

    WNDCLASSEXW wcCtx = { 0 };
    wcCtx.cbSize = sizeof(wcCtx);
    wcCtx.lpfnWndProc = CtxMenuWndProc;
    wcCtx.hInstance = hInst;
    wcCtx.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcCtx.lpszClassName = CTXMENU_CLASS;
    RegisterClassExW(&wcCtx);

    WNDCLASSEXW wcSched = { 0 };
    wcSched.cbSize = sizeof(wcSched);
    wcSched.lpfnWndProc = SchedWndProc;
    wcSched.hInstance = hInst;
    wcSched.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcSched.lpszClassName = SCHED_CLASS;
    RegisterClassExW(&wcSched);

    WNDCLASSEXW wcSet = { 0 };
    wcSet.cbSize = sizeof(wcSet);
    wcSet.lpfnWndProc = SetWndProc;
    wcSet.hInstance = hInst;
    wcSet.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcSet.lpszClassName = SET_CLASS;
    RegisterClassExW(&wcSet);

    WNDCLASSEXW wcAbout = { 0 };
    wcAbout.cbSize = sizeof(wcAbout);
    wcAbout.lpfnWndProc = AboutWndProc;
    wcAbout.hInstance = hInst;
    wcAbout.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcAbout.lpszClassName = ABOUT_CLASS;
    RegisterClassExW(&wcAbout);

    return TRUE;
}

void UI_Shutdown(void)
{
    ReleasePopupRenderCache();
    if (g_ctxHwnd) {
        DestroyWindow(g_ctxHwnd);
        g_ctxHwnd = NULL;
    }
    if (g_osdHwnd) {
        DestroyWindow(g_osdHwnd);
        g_osdHwnd = NULL;
    }
    if (g_schedHwnd) {
        DestroyWindow(g_schedHwnd);
        g_schedHwnd = NULL;
    }
    if (g_setHwnd) {
        DestroyWindow(g_setHwnd);
        g_setHwnd = NULL;
    }
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
    FinishSliderDrag(hwnd, &g_popupData, -1);
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

void UI_SetMasterTarget(int target)
{
    if (g_masterTargetKnown && g_masterTarget == target) return;
    g_masterTargetKnown = TRUE;
    g_masterTarget = target;
    if (g_popupData.ml)
        g_popupData.masterPercent = MasterTargetToSlider(g_popupData.ml, target);
    g_popupRender.frameValid = FALSE;
}

void UI_RefreshPopup(HWND hwnd, MonitorList *ml)
{
    if (!hwnd || !IsWindowVisible(hwnd)) return;
    g_popupData.ml = ml;
    if (g_popupData.activeSlider != ml->count)
        g_popupData.masterPercent = GetMasterPercent(ml);
    RenderPopup(hwnd, &g_popupData);
}

void UI_ShowOSD(HINSTANCE hInst, HMONITOR hMon, int percent)
{
    if (!hMon) return;

    MONITORINFO mi = { 0 };
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    int cx = (mi.rcWork.left + mi.rcWork.right) / 2 - OSD_W / 2;
    int cy = mi.rcWork.bottom - OSD_H - 80;

    g_osd.percent = percent;

    /* Create OSD window once */
    if (!g_osdHwnd || !IsWindow(g_osdHwnd)) {
        g_osdHwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            OSD_CLASS, L"",
            WS_POPUP,
            cx, cy, OSD_W, OSD_H,
            NULL, NULL, hInst, NULL);
        if (!g_osdHwnd) return;
    }

    SetWindowPos(g_osdHwnd, HWND_TOPMOST, cx, cy, OSD_W, OSD_H, SWP_NOACTIVATE);

    /* Show immediately at full alpha, only fade out later */
    g_osd.alpha = OSD_BASE_ALPHA;
    g_osd.fadingIn = FALSE;
    KillTimer(g_osdHwnd, OSD_TIMER_FADE);
    KillTimer(g_osdHwnd, OSD_TIMER_SHOW);
    RenderOSD(g_osdHwnd);
    ShowWindow(g_osdHwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_osdHwnd, OSD_TIMER_SHOW, OSD_SHOW_MS, NULL);
}

void UI_ShowContextMenu(HWND hwndOwner, Settings *s)
{
    /* Destroy previous if still open */
    if (g_ctxHwnd && IsWindow(g_ctxHwnd)) {
        DestroyWindow(g_ctxHwnd);
        g_ctxHwnd = NULL;
    }

    BuildContextMenu(&g_ctxData, s);
    g_ctxData.hwndOwner = hwndOwner;

    int w = CTXMENU_WIDTH;
    int h = GetCtxMenuHeight(&g_ctxData);

    /* Position at cursor, adjusted to stay on-screen */
    POINT pt;
    GetCursorPos(&pt);

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { 0 };
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    int x = pt.x;
    int y = pt.y - h;  /* prefer above cursor (tray is usually at bottom) */

    /* Adjust if off-screen */
    if (y < mi.rcWork.top)
        y = pt.y;  /* flip below cursor */
    if (x + w > mi.rcWork.right)
        x = mi.rcWork.right - w;
    if (x < mi.rcWork.left)
        x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom)
        y = mi.rcWork.bottom - h;

    g_ctxHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        CTXMENU_CLASS, L"",
        WS_POPUP,
        x, y, w, h,
        NULL, NULL, g_hInst, NULL);

    if (!g_ctxHwnd) return;

    RenderContextMenu(g_ctxHwnd, &g_ctxData);
    ShowWindow(g_ctxHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_ctxHwnd);
}

void UI_ShowScheduleEditor(HWND hwndOwner, Settings *s)
{
    if (g_schedHwnd && IsWindow(g_schedHwnd)) {
        DestroyWindow(g_schedHwnd);
        g_schedHwnd = NULL;
    }

    memset(&g_sched, 0, sizeof(g_sched));
    g_sched.settings = s;
    g_sched.owner = hwndOwner;
    g_sched.count = s->scheduleCount;
    g_sched.selectedRow = (s->scheduleCount > 0) ? 0 : -1;
    for (int i = 0; i < s->scheduleCount; i++)
        g_sched.pts[i] = s->schedule[i];

    int w = SCHED_WIDTH;
    int h = SchedHeight(&g_sched);

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { 0 };
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    int x = pt.x;
    int y = pt.y - h;
    if (y < mi.rcWork.top) y = pt.y;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;

    g_schedHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        SCHED_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g_hInst, NULL);
    if (!g_schedHwnd) return;

    RenderSchedEditor(g_schedHwnd, &g_sched);
    ShowWindow(g_schedHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_schedHwnd);
}

void UI_ShowSettings(HWND hwndOwner, Settings *s)
{
    if (g_setHwnd && IsWindow(g_setHwnd)) {
        DestroyWindow(g_setHwnd);
        g_setHwnd = NULL;
    }

    memset(&g_set, 0, sizeof(g_set));
    g_set.settings = s;
    g_set.owner = hwndOwner;
    g_set.hoverRow = -1;
    g_set.step = s->step;
    g_set.autostart = Settings_GetAutostart();   /* the registry is the truth here */
    g_set.scheduleEnabled = s->scheduleEnabled;
    g_set.idleDimEnabled = s->idleDimEnabled;
    g_set.idleDimPercent = s->idleDimPercent;
    g_set.idleDimMinutes = s->idleDimMinutes;
    g_set.presetCount = s->presetCount;
    for (int i = 0; i < s->presetCount; i++)
        g_set.presetValues[i] = (int)s->presets[i].brightness;
    BuildSettingsRows(&g_set);

    int w = SET_WIDTH;
    int h = SetHeight(&g_set);

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { 0 };
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    int x = pt.x;
    int y = pt.y - h;
    if (y < mi.rcWork.top) y = pt.y;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (y < mi.rcWork.top) y = mi.rcWork.top;   /* taller than the work area */

    g_setHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        SET_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g_hInst, NULL);
    if (!g_setHwnd) return;

    RenderSettings(g_setHwnd, &g_set);
    ShowWindow(g_setHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_setHwnd);
}

/* ---- About window ---- */

static void RenderAbout(HWND hwnd)
{
    int w = ABOUT_WIDTH, h = ABOUT_HEIGHT;

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    HBRUSH bgBrush = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(dc, TRANSPARENT);

    HFONT hTitle = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hBody  = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hLink  = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    RECT rc;

    /* Title */
    SelectObject(dc, hTitle);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    rc = (RECT){ 0, 18, w, 50 };
    DrawTextW(dc, APP_NAME, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Subtitle */
    SelectObject(dc, hSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 54, w, 72 };
    DrawTextW(dc, L"Monitor brightness for DDC/CI + WMI", -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Version */
    SelectObject(dc, hBody);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    rc = (RECT){ 0, 84, w, 104 };
    DrawTextW(dc, L"Version " APP_VERSION, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Author */
    SelectObject(dc, hSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 106, w, 124 };
    DrawTextW(dc, L"by " APP_AUTHOR, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Clickable repo link (measured so the hit rect is exact) */
    SelectObject(dc, hLink);
    SetTextColor(dc, HexToColorRef(CLR_ACCENT));
    const WCHAR *link = APP_REPO_DISPLAY;
    int linkLen = lstrlenW(link);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, link, linkLen, &sz);
    int linkX = (w - sz.cx) / 2;
    int linkY = 138;
    TextOutW(dc, linkX, linkY, link, linkLen);
    g_aboutLinkRect.left   = linkX - 6;
    g_aboutLinkRect.top    = linkY - 3;
    g_aboutLinkRect.right  = linkX + sz.cx + 6;
    g_aboutLinkRect.bottom = linkY + sz.cy + 3;

    /* Hint */
    SelectObject(dc, hSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 160, w, 178 };
    DrawTextW(dc, L"Esc to close", -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Release fonts (deselect first so none is active) */
    SelectObject(dc, (HFONT)GetStockObject(SYSTEM_FONT));
    DeleteObject(hTitle);
    DeleteObject(hBody);
    DeleteObject(hSmall);
    DeleteObject(hLink);

    ApplyRoundedMask(bits, w, h, ABOUT_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        BOOL onLink = PtInRect(&g_aboutLinkRect, pt);
        DestroyWindow(hwnd);
        if (onLink)
            ShellExecuteW(NULL, L"open", APP_REPO_URL, NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        SetCursor(LoadCursor(NULL, PtInRect(&g_aboutLinkRect, pt) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_aboutHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void UI_ShowAbout(HWND hwndOwner)
{
    if (g_aboutHwnd && IsWindow(g_aboutHwnd)) {
        DestroyWindow(g_aboutHwnd);
        g_aboutHwnd = NULL;
    }

    int w = ABOUT_WIDTH, h = ABOUT_HEIGHT;

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { 0 };
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    int x = pt.x - w / 2;
    int y = pt.y - h / 2;
    if (x + w > mi.rcWork.right)  x = mi.rcWork.right - w;
    if (x < mi.rcWork.left)       x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (y < mi.rcWork.top)        y = mi.rcWork.top;

    g_aboutHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        ABOUT_CLASS, L"", WS_POPUP,
        x, y, w, h, hwndOwner, NULL, g_hInst, NULL);
    if (!g_aboutHwnd) return;

    RenderAbout(g_aboutHwnd);
    ShowWindow(g_aboutHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_aboutHwnd);
}
