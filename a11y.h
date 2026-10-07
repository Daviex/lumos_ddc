#ifndef A11Y_H
#define A11Y_H

/* Screen reader support for the custom-drawn layered windows.

   None of the Lumos windows has child controls, so Windows has nothing to
   expose on its own. Each window describes its focusable items through an
   A11yModel, and this module turns that description into an MSAA IAccessible
   object (window = CHILDID_SELF, items = child ids 1..count). */

#include <windows.h>
#include <oleacc.h>

/* One item, or the window itself when the index is -1. */
typedef struct {
    LONG  role;          /* ROLE_SYSTEM_* */
    DWORD state;         /* STATE_SYSTEM_* flags. FOCUSED is added by this module. */
    RECT  rect;          /* client coordinates; ignored for the window itself */
    WCHAR name[160];
    WCHAR value[64];     /* empty when the item has no value */
    WCHAR action[32];    /* default action name, empty when there is none */
} A11yItem;

typedef struct {
    int  (*count)(void *ctx);
    void (*describe)(void *ctx, int index, A11yItem *out);   /* index -1 = window */
    int  (*focused)(void *ctx);                              /* -1 = the window itself */
    BOOL (*invoke)(void *ctx, int index);                    /* may be NULL */
    void *ctx;
} A11yModel;

/* Bind a model to a window for its whole life. The model must outlive the
   window; call A11y_Detach from WM_DESTROY so clients holding an object get
   a disconnected error instead of reading freed state. */
void A11y_Attach(HWND hwnd, const A11yModel *model);
void A11y_Detach(HWND hwnd);

/* Call from the window procedure on WM_GETOBJECT. Returns TRUE and sets
   *result when the request was for this window's client object. */
BOOL A11y_HandleGetObject(HWND hwnd, WPARAM wParam, LPARAM lParam, LRESULT *result);

/* WinEvents for an item (index -1 = the window itself). */
void A11y_NotifyFocus(HWND hwnd, int index);
void A11y_NotifyValue(HWND hwnd, int index);
void A11y_NotifyName(HWND hwnd, int index);
void A11y_NotifyState(HWND hwnd, int index);

/* EVENT_SYSTEM_MENUPOPUPSTART / END, so screen readers enter and leave menu mode. */
void A11y_NotifyMenuPopup(HWND hwnd, BOOL start);

/* Make a window a live region (assertive: a new value cuts off the previous
   one) through a LiveSetting annotation. Call once after creating the window,
   and Unmark from WM_DESTROY, since the annotation outlives the window. */
void A11y_MarkLiveRegion(HWND hwnd);
void A11y_UnmarkLiveRegion(HWND hwnd);

/* Speak the current name (describe(-1)) of a WS_EX_TOPMOST live region window.
   NVDA reads it even while another application has the focus. Narrator does
   not: it ignores announcements from background applications. */
void A11y_Announce(HWND hwnd);

#endif /* A11Y_H */
