#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

/* Shared by the ui_*.c window modules only. Outside code uses ui.h. */

#include "ui.h"
#include "resource.h"
#include "a11y.h"
#include <stdio.h>
#include <stdlib.h>

/* Module instance, set once by UI_Init. */
extern HINSTANCE g_uiInst;

/* ---- Drawing helpers (ui_draw.c) ---- */

COLORREF HexToColorRef(DWORD rgb);

/* 32-bit top-down DIB selected into a new memory DC. */
HDC CreateAlphaDC(int w, int h, HBITMAP *outBmp, BYTE **outBits);

/* Apply rounded corner mask with anti-aliasing and premultiply alpha */
void ApplyRoundedMask(BYTE *bits, int w, int h, int radius, BYTE baseAlpha);

/* Call UpdateLayeredWindow with a 32-bit surface */
void CommitLayered(HWND hwnd, HDC memDC, int w, int h);

/* 2px accent outline around the keyboard focus. */
void DrawFocusRing(HDC dc, const RECT *rc, int radius);

/* Move a caption-less window by dragging any spot that is not a control. */
void BeginWindowDrag(HWND hwnd);

/* Dialog footer: Cancel and Save on the right edge, the same in every dialog. */
#define DLG_BTN_W    84
#define DLG_BTN_H    28
#define DLG_BTN_GAP  8
#define DLG_MARGIN   16
void DialogButtonRects(int w, int y, RECT *rcCancel, RECT *rcSave);
void DrawDialogButton(HDC dc, const RECT *rc, const WCHAR *label, BOOL primary, HFONT font);

/* TRUE while the key is held, for reading modifiers inside WM_KEYDOWN. */
#define KEY_DOWN(vk) ((GetKeyState(vk) & 0x8000) != 0)

/* ---- Per-window class registration and teardown ---- */

BOOL UiPopup_Init(HINSTANCE hInst);
void UiOsd_Init(HINSTANCE hInst);
void UiOsd_Shutdown(void);
void UiMenu_Init(HINSTANCE hInst);
void UiMenu_Shutdown(void);
void UiSched_Init(HINSTANCE hInst);
void UiSched_Shutdown(void);
void UiSettings_Init(HINSTANCE hInst);
void UiSettings_Shutdown(void);
void UiAbout_Init(HINSTANCE hInst);

#endif /* UI_INTERNAL_H */
