#include "ui_internal.h"

static const WCHAR OSD_CLASS[]     = L"LumosOSD";
static HWND g_osdHwnd = NULL;

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

/* The OSD has no items. Its own name is the spoken text, which is what NVDA
   reads on the live region event that A11y_Announce raises. */
static int OsdA11yCount(void *ctx) { (void)ctx; return 0; }
static int OsdA11yFocused(void *ctx) { (void)ctx; return -1; }

static void OsdA11yDescribe(void *ctx, int index, A11yItem *out)
{
    (void)ctx;
    if (index >= 0)
        return;
    out->role = ROLE_SYSTEM_STATICTEXT;
    out->state = STATE_SYSTEM_READONLY;
    _snwprintf(out->name, 159, L"Brightness %d%%", g_osd.percent);
}

static const A11yModel g_osdModel = {
    OsdA11yCount, OsdA11yDescribe, OsdA11yFocused, NULL, NULL
};

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
    case WM_GETOBJECT: {
        LRESULT r;
        if (A11y_HandleGetObject(hwnd, wParam, lParam, &r))
            return r;
        break;
    }

    case WM_DESTROY:
        A11y_UnmarkLiveRegion(hwnd);
        A11y_Detach(hwnd);
        return 0;

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

void UI_ShowOSD(HINSTANCE hInst, HMONITOR hMon, int percent, BOOL announce)
{
    if (!hMon) return;

    MONITORINFO mi = { sizeof(mi) };
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
        A11y_Attach(g_osdHwnd, &g_osdModel);
        A11y_MarkLiveRegion(g_osdHwnd);
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

    if (announce)
        A11y_Announce(g_osdHwnd);   /* reads "Brightness N%" from the model */
}

/* ---- Class registration ---- */

void UiOsd_Init(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OsdWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = OSD_CLASS;
    RegisterClassExW(&wc);
}

void UiOsd_Shutdown(void)
{
    if (g_osdHwnd) {
        DestroyWindow(g_osdHwnd);
        g_osdHwnd = NULL;
    }
}
