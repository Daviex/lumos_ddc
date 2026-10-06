#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

/* Shared by the ui_*.c window modules only. Outside code uses ui.h. */

#include "ui.h"
#include "resource.h"
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
