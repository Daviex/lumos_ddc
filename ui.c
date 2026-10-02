#include "ui.h"
#include "ui_graphics.h"
#include "ui_monitor_selection.h"
#include "resource.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <windowsx.h>

static HINSTANCE g_hInst;
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

static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void RenderAbout(HWND hwnd);

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
    HDC dc = UI_CreateAlphaDC(OSD_W, OSD_H, &bmp, &bits);

    /* Background */
    HBRUSH bg = CreateSolidBrush(UI_ColorRef(CLR_BG));
    RECT rcAll = { 0, 0, OSD_W, OSD_H };
    FillRect(dc, &rcAll, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);

    /* Percentage text */
    WCHAR pctStr[8];
    wsprintfW(pctStr, L"%d%%", g_osd.percent);

    SetTextColor(dc, UI_ColorRef(CLR_TEXT));
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
    HBRUSH trackBr = CreateSolidBrush(UI_ColorRef(CLR_TRACK));
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, trackBr);
    RoundRect(dc, barL, barY, barR, barY + OSD_BAR_H, OSD_BAR_H, OSD_BAR_H);

    /* Fill */
    HBRUSH fillBr = CreateSolidBrush(UI_ColorRef(CLR_ACCENT));
    SelectObject(dc, fillBr);
    if (fillW > 0)
        RoundRect(dc, barL, barY, barL + fillW, barY + OSD_BAR_H, OSD_BAR_H, OSD_BAR_H);

    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(trackBr);
    DeleteObject(fillBr);

    /* Apply rounded mask with current alpha */
    UI_ApplyRoundedMask(bits, OSD_W, OSD_H, OSD_CORNER, g_osd.alpha);
    UI_CommitLayered(hwnd, dc, OSD_W, OSD_H);

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
    HDC dc = UI_CreateAlphaDC(w, h, &bmp, &bits);

    /* Background */
    HBRUSH bgBrush = CreateSolidBrush(UI_ColorRef(CLR_BG));
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
            HBRUSH sepBrush = CreateSolidBrush(UI_ColorRef(CLR_TRACK));
            RECT rcSep = { 12, lineY, w - 12, lineY + 1 };
            FillRect(dc, &rcSep, sepBrush);
            DeleteObject(sepBrush);
            cy += CTXMENU_SEP_H;
            continue;
        }

        /* Hover highlight */
        if (i == d->hoverIndex) {
            HBRUSH hoverBrush = CreateSolidBrush(UI_ColorRef(CLR_SURFACE));
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
            SetTextColor(dc, UI_ColorRef(CLR_ACCENT));
            RECT rcCheck = { textX - 2, cy, textX + 14, cy + CTXMENU_ITEM_H };
            DrawTextW(dc, L"\x2713", 1, &rcCheck, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            textX += 18;
        }

        /* Label */
        SelectObject(dc, hFont);
        SetTextColor(dc, UI_ColorRef(CLR_TEXT));
        RECT rcLabel = { textX, cy, w - 12, cy + CTXMENU_ITEM_H };
        DrawTextW(dc, it->label, -1, &rcLabel,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        cy += CTXMENU_ITEM_H;
    }

    SelectObject(dc, oldFont);
    DeleteObject(hFont);
    DeleteObject(hFontCheck);

    UI_ApplyRoundedMask(bits, w, h, CTXMENU_CORNER, 245);
    UI_CommitLayered(hwnd, dc, w, h);

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
    HDC dc = UI_CreateAlphaDC(w, h, &bmp, &bits);

    HBRUSH bg = CreateSolidBrush(UI_ColorRef(CLR_BG));
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
    SetTextColor(dc, UI_ColorRef(CLR_TEXT));
    RECT rcTitle = { 16, 12, w - 16, 32 };
    DrawTextW(dc, L"Brightness Schedule", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
    RECT rcHint = { 16, 12, w - 16, 32 };
    DrawTextW(dc, L"wheel = adjust", -1, &rcHint, DT_RIGHT | DT_SINGLELINE);

    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);

    for (int row = 0; row < d->count; row++) {
        int y = SCHED_HEADER_H + row * SCHED_ROW_H;

        if (row == d->selectedRow) {
            HBRUSH hb = CreateSolidBrush(UI_ColorRef(CLR_SURFACE));
            HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
            RoundRect(dc, 8, y + 2, w - 8, y + SCHED_ROW_H - 2, 8, 8);
            SelectObject(dc, ob);
            DeleteObject(hb);
        }

        SchedulePoint *p = &d->pts[row];
        WCHAR s[16];
        RECT rc;

        SelectObject(dc, hFont);
        SetTextColor(dc, UI_ColorRef(CLR_TEXT));
        SchedFieldRect(row, SF_HOUR, &rc);
        wsprintfW(s, L"%02d", p->minutes / 60);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT rcColon = rc; rcColon.left = rc.right; rcColon.right = rc.right + 8;
        DrawTextW(dc, L":", -1, &rcColon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_MIN, &rc);
        wsprintfW(s, L"%02d", p->minutes % 60);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_BRI, &rc);
        SetTextColor(dc, UI_ColorRef(CLR_ACCENT));
        wsprintfW(s, L"%d%%", p->brightness);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_DEL, &rc);
        SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
        DrawTextW(dc, L"\x2715", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE); /* x */
    }

    /* Footer buttons */
    RECT rcAdd, rcSave;
    SchedButtonRects(d, &rcAdd, &rcSave);
    HBRUSH btn = CreateSolidBrush(UI_ColorRef(CLR_SURFACE));
    HBRUSH acc = CreateSolidBrush(UI_ColorRef(CLR_ACCENT));
    HBRUSH ob = (HBRUSH)SelectObject(dc, btn);
    RoundRect(dc, rcAdd.left, rcAdd.top, rcAdd.right, rcAdd.bottom, 8, 8);
    SelectObject(dc, acc);
    RoundRect(dc, rcSave.left, rcSave.top, rcSave.right, rcSave.bottom, 8, 8);
    SelectObject(dc, ob);
    DeleteObject(btn);
    DeleteObject(acc);

    SelectObject(dc, hFont);
    SetTextColor(dc, UI_ColorRef(CLR_TEXT));
    DrawTextW(dc, L"+ Add", -1, &rcAdd, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, UI_ColorRef(CLR_BG));
    DrawTextW(dc, L"Save", -1, &rcSave, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(hFont);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

    UI_ApplyRoundedMask(bits, w, h, SCHED_CORNER, 245);
    UI_CommitLayered(hwnd, dc, w, h);

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
enum { SET_SECTION = 0, SET_TOGGLE, SET_NUMBER, SET_MONITORS, SET_SLIDER };
enum { SET_UNIT_PLAIN = 0, SET_UNIT_PERCENT, SET_UNIT_MINUTES, SET_UNIT_SECONDS };
#define SET_SLIDER_H 56

/* Hit kinds returned by SetHitTest. */
enum { SETHIT_NONE = 0, SETHIT_MINUS, SETHIT_PLUS, SETHIT_TOGGLE, SETHIT_ROW, SETHIT_MONITORS, SETHIT_SLIDER };

typedef struct {
    int    kind;
    WCHAR  label[MAX_PRESET_NAME + 16];
    int   *ival;      /* SET_NUMBER / SET_SLIDER: the value being edited */
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
    int   sourcePollSeconds;
    int   presetValues[MAX_PRESETS];
    int   presetCount;
    MonitorSelection monitorSelection;
    MonitorList *monitors;
    BOOL selectionOpen;

    SetRow rows[MAX_SET_ROWS];
    int    rowCount;
    int    hoverRow;
    int    keyboardRow; /* rowCount denotes Save, -1 means mouse navigation */
    int    activeSliderRow; /* -1 unless a slider owns mouse capture */
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

static SetRow *SetAddNumber(SetEditData *d, const WCHAR *label, int *val,
                         int lo, int hi, int step, int unit)
{
    SetRow *r = SetAddRow(d, SET_NUMBER, label);
    if (!r) return NULL;
    r->ival = val;
    r->lo = lo;
    r->hi = hi;
    r->step = step;
    r->unit = unit;
    return r;
}

static void BuildSettingsRows(SetEditData *d)
{
    d->rowCount = 0;

    SetAddRow(d, SET_SECTION, L"GENERAL");
    SetAddNumber(d, L"Brightness step", &d->step, 1, 50, 1, SET_UNIT_PERCENT);
    SetAddToggle(d, L"Start with Windows", &d->autostart);
    SetAddRow(d, SET_MONITORS, L"Choose Monitors");
    SetRow *sourceInterval = SetAddNumber(d, L"Source check interval", &d->sourcePollSeconds,
                 MIN_SOURCE_POLL_SECONDS, MAX_SOURCE_POLL_SECONDS, 1, SET_UNIT_SECONDS);
    if (sourceInterval) sourceInterval->kind = SET_SLIDER;

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

static int SetRowHeight(const SetRow *r)
{
    if (r->kind == SET_SECTION) return SET_SECTION_H;
    return r->kind == SET_SLIDER ? SET_SLIDER_H : SET_ROW_H;
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

static int SetRowTop(const SetEditData *d, int row)
{
    int top = SET_HEADER_H;
    for (int i = 0; i < row; i++) top += SetRowHeight(&d->rows[i]);
    return top;
}

static void SetSliderRect(int top, RECT *track)
{
    track->left = 22;
    track->right = SET_WIDTH - 22;
    track->top = top + 35;
    track->bottom = top + 41;
}

static BOOL SetSliderValue(SetEditData *d, int row, int x)
{
    if (row < 0 || row >= d->rowCount) return FALSE;
    SetRow *r = &d->rows[row];
    if (r->kind != SET_SLIDER || !r->ival) return FALSE;
    RECT track;
    SetSliderRect(SetRowTop(d, row), &track);
    if (x < track.left) x = track.left;
    if (x > track.right) x = track.right;
    int width = track.right - track.left;
    int value = r->lo + ((x - track.left) * (r->hi - r->lo) + width / 2) / width;
    if (*r->ival == value) return FALSE;
    *r->ival = value;
    return TRUE;
}

static void SetEndSliderDrag(HWND hwnd, SetEditData *d)
{
    d->activeSliderRow = -1; /* clear before ReleaseCapture sends another message */
    if (GetCapture() == hwnd) ReleaseCapture();
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
    if ((r->kind != SET_NUMBER && r->kind != SET_SLIDER) || !r->ival) return;
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
            if (r->kind == SET_MONITORS) {
                if (x < 12 || x > SET_WIDTH - 12) return -1;
                *outHit = SETHIT_MONITORS;
                return i;
            }
            if (r->kind == SET_TOGGLE) {
                RECT rc;
                SetToggleRect(top, &rc);
                *outHit = (x >= rc.left && x <= rc.right) ? SETHIT_TOGGLE : SETHIT_ROW;
                return i;
            }
            if (r->kind == SET_SLIDER) {
                RECT track;
                SetSliderRect(top, &track);
                *outHit = x >= track.left - 7 && x <= track.right + 7 &&
                          y >= top + 26 && y < top + rh - 4 ? SETHIT_SLIDER : SETHIT_ROW;
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

static BOOL SetCanSave(const SetEditData *d)
{
    return !d->monitorSelection.selectedOnly ||
           (d->monitorSelection.count > 0 && d->monitorSelection.count <= MAX_MONITORS);
}

static void RenderSettings(HWND hwnd, SetEditData *d)
{
    int w = SET_WIDTH;
    int h = SetHeight(d);

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = UI_CreateAlphaDC(w, h, &bmp, &bits);
    if (!dc) return;

    HBRUSH bg = CreateSolidBrush(UI_ColorRef(CLR_BG));
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
    HFONT oldFont = (HFONT)SelectObject(dc, hFontBold);
    SetTextColor(dc, UI_ColorRef(CLR_TEXT));
    DrawTextW(dc, L"Settings", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
    DrawTextW(dc, L"Tab / wheel", -1, &rcTitle, DT_RIGHT | DT_SINGLELINE);

    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);

    int y = SET_HEADER_H;
    for (int i = 0; i < d->rowCount; i++) {
        SetRow *r = &d->rows[i];
        int rowHeight = SetRowHeight(r);

        if (r->kind == SET_SECTION) {
            RECT rc = { 16, y, w - 16, y + SET_SECTION_H };
            SelectObject(dc, hFontSmall);
            SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
            DrawTextW(dc, r->label, -1, &rc, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            y += SET_SECTION_H;
            continue;
        }

        if (i == d->hoverRow || i == d->keyboardRow || r->kind == SET_MONITORS) {
            HBRUSH hb = CreateSolidBrush(UI_ColorRef(CLR_SURFACE));
            HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
            RoundRect(dc, 8, y + 2, w - 8, y + rowHeight - 2, 8, 8);
            SelectObject(dc, ob);
            DeleteObject(hb);
        }

        if (r->kind == SET_MONITORS) {
            RECT label = { 16, y, w - 112, y + SET_ROW_H };
            RECT summary = { w - 116, y, w - 16, y + SET_ROW_H };
            WCHAR text[32];
            if (d->monitorSelection.selectedOnly)
                wsprintfW(text, L"%d selected", d->monitorSelection.count);
            else
                lstrcpyW(text, L"All Monitors");
            SelectObject(dc, hFont);
            SetTextColor(dc, UI_ColorRef(CLR_TEXT));
            DrawTextW(dc, r->label, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, hFontSmall);
            SetTextColor(dc, UI_ColorRef(CLR_ACCENT));
            DrawTextW(dc, text, -1, &summary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            y += SET_ROW_H;
            continue;
        }

        if (r->kind == SET_SLIDER) {
            RECT label = { 16, y, w - 66, y + 28 };
            RECT value = { w - 66, y, w - 16, y + 28 };
            WCHAR text[16];
            int v = r->ival ? *r->ival : r->lo;
            if (v < r->lo) v = r->lo;
            if (v > r->hi) v = r->hi;
            wsprintfW(text, L"%d s", v);
            SelectObject(dc, hFont);
            SetTextColor(dc, UI_ColorRef(CLR_TEXT));
            DrawTextW(dc, r->label, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(dc, UI_ColorRef(CLR_ACCENT));
            DrawTextW(dc, text, -1, &value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            RECT track;
            SetSliderRect(y, &track);
            int x = track.left + (v - r->lo) * (track.right - track.left) / (r->hi - r->lo);
            HBRUSH background = CreateSolidBrush(UI_ColorRef(CLR_TRACK));
            HBRUSH fill = CreateSolidBrush(UI_ColorRef(CLR_ACCENT));
            HBRUSH knob = CreateSolidBrush(UI_ColorRef(CLR_TEXT));
            HBRUSH oldBrush = (HBRUSH)SelectObject(dc, background);
            RoundRect(dc, track.left, track.top, track.right, track.bottom, 6, 6);
            SelectObject(dc, fill);
            if (x > track.left) RoundRect(dc, track.left, track.top, x, track.bottom, 6, 6);
            SelectObject(dc, knob);
            int cy = (track.top + track.bottom) / 2;
            Ellipse(dc, x - 7, cy - 7, x + 7, cy + 7);
            SelectObject(dc, oldBrush);
            DeleteObject(background);
            DeleteObject(fill);
            DeleteObject(knob);
            y += rowHeight;
            continue;
        }

        RECT rcLabel = { 16, y, w - 124, y + SET_ROW_H };
        SelectObject(dc, hFont);
        SetTextColor(dc, UI_ColorRef(CLR_TEXT));
        DrawTextW(dc, r->label, -1, &rcLabel,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (r->kind == SET_TOGGLE) {
            RECT rcT;
            SetToggleRect(y, &rcT);
            BOOL on = (r->bval && *r->bval);
            HBRUSH track = CreateSolidBrush(UI_ColorRef(on ? CLR_ACCENT : CLR_TRACK));
            HBRUSH knob  = CreateSolidBrush(UI_ColorRef(on ? CLR_BG : CLR_SUBTEXT));
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

            HBRUSH btn = CreateSolidBrush(UI_ColorRef(CLR_TRACK));
            HBRUSH ob = (HBRUSH)SelectObject(dc, btn);
            RoundRect(dc, rcMinus.left, rcMinus.top, rcMinus.right, rcMinus.bottom, 6, 6);
            RoundRect(dc, rcPlus.left, rcPlus.top, rcPlus.right, rcPlus.bottom, 6, 6);
            SelectObject(dc, ob);
            DeleteObject(btn);

            SelectObject(dc, hFontSmall);
            SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
            DrawTextW(dc, L"\x2013", -1, &rcMinus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, L"+", -1, &rcPlus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            WCHAR val[16];
            int v = r->ival ? *r->ival : 0;
            if (r->unit == SET_UNIT_MINUTES)      wsprintfW(val, L"%dm", v);
            else if (r->unit == SET_UNIT_PERCENT) wsprintfW(val, L"%d%%", v);
            else                                  wsprintfW(val, L"%d", v);
            SelectObject(dc, hFont);
            SetTextColor(dc, UI_ColorRef(CLR_ACCENT));
            DrawTextW(dc, val, -1, &rcValue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        y += SET_ROW_H;
    }

    /* Footer: Save on the right, cancel hint on the left */
    RECT rcSave;
    SetSaveRect(d, &rcSave);
    BOOL canSave = SetCanSave(d);
    HBRUSH acc = CreateSolidBrush(UI_ColorRef(canSave ? CLR_ACCENT : CLR_TRACK));
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, acc);
    RoundRect(dc, rcSave.left, rcSave.top, rcSave.right, rcSave.bottom, 8, 8);
    SelectObject(dc, oldBr);
    DeleteObject(acc);

    SelectObject(dc, hFont);
    SetTextColor(dc, UI_ColorRef(canSave ? CLR_BG : CLR_SUBTEXT));
    DrawTextW(dc, L"Save", -1, &rcSave, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (d->keyboardRow == d->rowCount) {
        RECT focus = { rcSave.left + 4, rcSave.top + 4, rcSave.right - 4, rcSave.bottom - 4 };
        DrawFocusRect(dc, &focus);
    }

    RECT rcNote = { 16, rcSave.top, rcSave.left - 8, rcSave.bottom };
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
    DrawTextW(dc, canSave ? L"Esc to cancel" : L"Select at least one", -1, &rcNote,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldPen);
    SelectObject(dc, oldFont);
    DeleteObject(noPen);
    DeleteObject(hFont);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

    UI_ApplyRoundedMask(bits, w, h, SET_CORNER, 245);
    UI_CommitLayered(hwnd, dc, w, h);

    DeleteDC(dc);
    DeleteObject(bmp);
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
    s->sourcePollSeconds = Settings_ClampSourcePollSeconds(d->sourcePollSeconds);
    s->monitorSelection = d->monitorSelection;
    for (int i = 0; i < d->presetCount && i < s->presetCount; i++)
        s->presets[i].brightness = (DWORD)d->presetValues[i];
}

static void SetSelectionClosed(BOOL applied)
{
    (void)applied;
    g_set.selectionOpen = FALSE;
    if (g_setHwnd) RenderSettings(g_setHwnd, &g_set);
}

static void SetChooseMonitors(HWND hwnd, SetEditData *d)
{
    d->selectionOpen = TRUE; /* activation changes while the child is being created */
    if (!UI_ShowMonitorSelection(hwnd, &d->monitorSelection, d->monitors, SetSelectionClosed)) {
        d->selectionOpen = FALSE;
        RenderSettings(hwnd, d);
    }
}

static void SetSave(HWND hwnd, SetEditData *d)
{
    if (!SetCanSave(d)) return;
    SetCommit(d);
    HWND owner = d->owner;
    DestroyWindow(hwnd);
    PostMessageW(owner, WM_COMMAND, (WPARAM)IDM_SETTINGS_SAVED, 0);
}

static void SetMoveKeyboard(SetEditData *d, int direction)
{
    int row = d->keyboardRow;
    if (row < 0) row = direction > 0 ? -1 : 0;
    do {
        row = (row + direction + d->rowCount + 1) % (d->rowCount + 1);
    } while (row < d->rowCount && d->rows[row].kind == SET_SECTION);
    d->keyboardRow = row;
}

static LRESULT CALLBACK SetWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SetEditData *d = &g_set;

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

        RECT rcSave;
        SetSaveRect(d, &rcSave);
        if (x >= rcSave.left && x <= rcSave.right && y >= rcSave.top && y <= rcSave.bottom) {
            SetSave(hwnd, d);
            return 0;
        }

        int hit;
        int row = SetHitTest(d, x, y, &hit);
        if (row >= 0) {
            d->keyboardRow = -1;
            SetRow *r = &d->rows[row];
            if (hit == SETHIT_MONITORS) { SetChooseMonitors(hwnd, d); return 0; }
            if (hit == SETHIT_SLIDER) {
                d->activeSliderRow = row;
                SetSliderValue(d, row, x);
                SetCapture(hwnd);
            } else if (hit == SETHIT_TOGGLE && r->bval) *r->bval = !*r->bval;
            else if (hit == SETHIT_MINUS)        SetAdjust(r, -1);
            else if (hit == SETHIT_PLUS)         SetAdjust(r, +1);
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (d->activeSliderRow >= 0) {
            BOOL changed = SetSliderValue(d, d->activeSliderRow, GET_X_LPARAM(lParam));
            if (!(wParam & MK_LBUTTON)) SetEndSliderDrag(hwnd, d);
            if (changed) RenderSettings(hwnd, d);
            return 0;
        }
        int hit;
        int row = SetHitTest(d, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &hit);
        if (row != d->hoverRow) {
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (d->activeSliderRow >= 0) {
            SetSliderValue(d, d->activeSliderRow, GET_X_LPARAM(lParam));
            SetEndSliderDrag(hwnd, d);
            RenderSettings(hwnd, d);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if ((HWND)lParam != hwnd) d->activeSliderRow = -1;
        return 0;

    case WM_CANCELMODE:
        SetEndSliderDrag(hwnd, d);
        return 0;

    case WM_MOUSEWHEEL: {
        int dir = ((short)HIWORD(wParam) > 0) ? 1 : -1;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        int hit;
        int row = SetHitTest(d, pt.x, pt.y, &hit);
        if (row >= 0 && (d->rows[row].kind == SET_NUMBER || d->rows[row].kind == SET_SLIDER)) {
            SetAdjust(&d->rows[row], dir);
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        if (wParam == VK_TAB) {
            SetMoveKeyboard(d, GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            RenderSettings(hwnd, d);
            return 0;
        }
        if (d->keyboardRow == d->rowCount && (wParam == VK_RETURN || wParam == VK_SPACE)) {
            SetSave(hwnd, d);
            return 0;
        }
        if (d->keyboardRow >= 0 && d->keyboardRow < d->rowCount) {
            SetRow *r = &d->rows[d->keyboardRow];
            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                if (r->kind == SET_MONITORS) { SetChooseMonitors(hwnd, d); return 0; }
                if (r->kind == SET_TOGGLE && r->bval) *r->bval = !*r->bval;
            } else if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                SetAdjust(r, wParam == VK_RIGHT ? 1 : -1);
            } else if (r->kind == SET_SLIDER && r->ival && (wParam == VK_HOME || wParam == VK_END)) {
                *r->ival = wParam == VK_HOME ? r->lo : r->hi;
            }
            RenderSettings(hwnd, d);
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && !d->selectionOpen) {
            /* Dismiss without saving on click-outside, like the schedule editor. */
            DestroyWindow(hwnd);
            g_setHwnd = NULL;
        }
        return 0;

    case WM_DESTROY:
        SetEndSliderDrag(hwnd, d);
        g_setHwnd = NULL;
        UI_CloseMonitorSelection();
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

    if (!UI_PopupInit(hInst)) return FALSE;
    if (!UI_MonitorSelectionInit(hInst)) return FALSE;

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
    UI_CloseMonitorSelection();
    UI_PopupShutdown();
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

void UI_ShowSettings(HWND hwndOwner, Settings *s, MonitorList *monitors)
{
    if (g_setHwnd && IsWindow(g_setHwnd)) {
        DestroyWindow(g_setHwnd);
        g_setHwnd = NULL;
    }

    memset(&g_set, 0, sizeof(g_set));
    g_set.settings = s;
    g_set.owner = hwndOwner;
    g_set.hoverRow = -1;
    g_set.keyboardRow = -1;
    g_set.monitors = monitors;
    g_set.monitorSelection = s->monitorSelection;
    g_set.step = s->step;
    g_set.autostart = Settings_GetAutostart();   /* the registry is the truth here */
    g_set.scheduleEnabled = s->scheduleEnabled;
    g_set.idleDimEnabled = s->idleDimEnabled;
    g_set.idleDimPercent = s->idleDimPercent;
    g_set.idleDimMinutes = s->idleDimMinutes;
    g_set.sourcePollSeconds = Settings_ClampSourcePollSeconds(s->sourcePollSeconds);
    g_set.activeSliderRow = -1;
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

BOOL UI_HandleDialogMessage(MSG *message)
{
    return UI_MonitorSelectionMessage(message);
}

/* ---- About window ---- */

static void RenderAbout(HWND hwnd)
{
    int w = ABOUT_WIDTH, h = ABOUT_HEIGHT;

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = UI_CreateAlphaDC(w, h, &bmp, &bits);

    HBRUSH bgBrush = CreateSolidBrush(UI_ColorRef(CLR_BG));
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
    SetTextColor(dc, UI_ColorRef(CLR_TEXT));
    rc = (RECT){ 0, 18, w, 50 };
    DrawTextW(dc, APP_NAME, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Subtitle */
    SelectObject(dc, hSmall);
    SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 54, w, 72 };
    DrawTextW(dc, L"Monitor brightness for DDC/CI + WMI", -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Version */
    SelectObject(dc, hBody);
    SetTextColor(dc, UI_ColorRef(CLR_TEXT));
    rc = (RECT){ 0, 84, w, 104 };
    DrawTextW(dc, L"Version " APP_VERSION, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Author */
    SelectObject(dc, hSmall);
    SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 106, w, 124 };
    DrawTextW(dc, L"by " APP_AUTHOR, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Clickable repo link (measured so the hit rect is exact) */
    SelectObject(dc, hLink);
    SetTextColor(dc, UI_ColorRef(CLR_ACCENT));
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
    SetTextColor(dc, UI_ColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 160, w, 178 };
    DrawTextW(dc, L"Esc to close", -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Release fonts (deselect first so none is active) */
    SelectObject(dc, (HFONT)GetStockObject(SYSTEM_FONT));
    DeleteObject(hTitle);
    DeleteObject(hBody);
    DeleteObject(hSmall);
    DeleteObject(hLink);

    UI_ApplyRoundedMask(bits, w, h, ABOUT_CORNER, 245);
    UI_CommitLayered(hwnd, dc, w, h);

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
