#include "a11y.h"
#include <oleauto.h>

/* The model pointer is kept in a window property rather than inside the COM
   object. A screen reader can hold an object long after the window is gone;
   looking the model up on every call means such a stale object answers
   CO_E_OBJNOTCONNECTED instead of reading state that no longer exists. */
static const WCHAR A11Y_PROP[] = L"LumosA11yModel";

/* The interface ids are defined here rather than taken from the libraries.
   MinGW's liboleacc.a exports IID_IAccessible as an import thunk (code), and
   linking against it silently gave us the bytes of a jmp instruction instead
   of the GUID. LresultFromObject then asked COM to marshal an interface that
   does not exist, and every screen reader got REGDB_E_IIDNOTREG. */
static const IID kIID_IAccessible =
    { 0x618736e0, 0x3c3d, 0x11cf, { 0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const IID kIID_IDispatch =
    { 0x00020400, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID kIID_IUnknown =
    { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

typedef struct {
    IAccessible iface;   /* first member, so IAccessible* == A11yObject* */
    LONG refs;
    HWND hwnd;
} A11yObject;

static const A11yModel *ModelOf(A11yObject *o)
{
    if (!IsWindow(o->hwnd))
        return NULL;
    return (const A11yModel *)GetPropW(o->hwnd, A11Y_PROP);
}

/* VARIANT child id -> item index (-1 = the window). */
static HRESULT ChildIndex(const A11yModel *m, VARIANT v, int *outIndex)
{
    if (v.vt != VT_I4)
        return E_INVALIDARG;
    if (v.lVal == CHILDID_SELF) {
        *outIndex = -1;
        return S_OK;
    }
    if (v.lVal < 1 || v.lVal > m->count(m->ctx))
        return E_INVALIDARG;
    *outIndex = v.lVal - 1;
    return S_OK;
}

/* Look up the model and the item a call refers to, in one step. */
static HRESULT Resolve(A11yObject *o, VARIANT v, const A11yModel **outModel,
                       int *outIndex, A11yItem *outItem)
{
    const A11yModel *m = ModelOf(o);
    if (!m)
        return CO_E_OBJNOTCONNECTED;
    HRESULT hr = ChildIndex(m, v, outIndex);
    if (FAILED(hr))
        return hr;
    if (outItem) {
        memset(outItem, 0, sizeof(*outItem));
        m->describe(m->ctx, *outIndex, outItem);
    }
    *outModel = m;
    return S_OK;
}

static HRESULT ReturnString(const WCHAR *s, BSTR *out)
{
    if (!out)
        return E_POINTER;
    if (!s || !s[0]) {
        *out = NULL;
        return S_FALSE;
    }
    *out = SysAllocString(s);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static BOOL IsForeground(HWND hwnd)
{
    return GetForegroundWindow() == hwnd;
}

/* ---- IUnknown / IDispatch ---- */

static HRESULT STDMETHODCALLTYPE Acc_QueryInterface(IAccessible *self, REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, &kIID_IUnknown) || IsEqualIID(riid, &kIID_IDispatch) ||
        IsEqualIID(riid, &kIID_IAccessible)) {
        *ppv = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Acc_AddRef(IAccessible *self)
{
    return (ULONG)InterlockedIncrement(&((A11yObject *)self)->refs);
}

static ULONG STDMETHODCALLTYPE Acc_Release(IAccessible *self)
{
    LONG r = InterlockedDecrement(&((A11yObject *)self)->refs);
    if (r == 0)
        HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE Acc_GetTypeInfoCount(IAccessible *self, UINT *pctinfo)
{
    (void)self;
    if (!pctinfo) return E_POINTER;
    *pctinfo = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_GetTypeInfo(IAccessible *self, UINT i, LCID lcid, ITypeInfo **pp)
{
    (void)self; (void)i; (void)lcid;
    if (pp) *pp = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Acc_GetIDsOfNames(IAccessible *self, REFIID riid, LPOLESTR *names,
                                                   UINT n, LCID lcid, DISPID *ids)
{
    (void)self; (void)riid; (void)names; (void)n; (void)lcid; (void)ids;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Acc_Invoke(IAccessible *self, DISPID id, REFIID riid, LCID lcid,
                                            WORD flags, DISPPARAMS *params, VARIANT *result,
                                            EXCEPINFO *excep, UINT *argErr)
{
    (void)self; (void)id; (void)riid; (void)lcid; (void)flags;
    (void)params; (void)result; (void)excep; (void)argErr;
    return E_NOTIMPL;
}

/* ---- IAccessible ---- */

static HRESULT STDMETHODCALLTYPE Acc_get_accParent(IAccessible *self, IDispatch **ppdisp)
{
    A11yObject *o = (A11yObject *)self;
    if (!ppdisp) return E_POINTER;
    *ppdisp = NULL;
    if (!ModelOf(o)) return CO_E_OBJNOTCONNECTED;
    return AccessibleObjectFromWindow(o->hwnd, (DWORD)OBJID_WINDOW, &kIID_IDispatch, (void **)ppdisp);
}

static HRESULT STDMETHODCALLTYPE Acc_get_accChildCount(IAccessible *self, long *pcount)
{
    const A11yModel *m = ModelOf((A11yObject *)self);
    if (!pcount) return E_POINTER;
    if (!m) return CO_E_OBJNOTCONNECTED;
    *pcount = m->count(m->ctx);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_get_accChild(IAccessible *self, VARIANT v, IDispatch **ppdisp)
{
    const A11yModel *m;
    int index;
    if (!ppdisp) return E_POINTER;
    *ppdisp = NULL;
    HRESULT hr = Resolve((A11yObject *)self, v, &m, &index, NULL);
    if (FAILED(hr)) return hr;
    if (index < 0)
        return Acc_QueryInterface(self, &kIID_IDispatch, (void **)ppdisp);
    return S_FALSE;   /* items are simple elements, not objects of their own */
}

static HRESULT STDMETHODCALLTYPE Acc_get_accName(IAccessible *self, VARIANT v, BSTR *out)
{
    const A11yModel *m;
    int index;
    A11yItem it;
    if (out) *out = NULL;
    HRESULT hr = Resolve((A11yObject *)self, v, &m, &index, &it);
    if (FAILED(hr)) return hr;
    return ReturnString(it.name, out);
}

static HRESULT STDMETHODCALLTYPE Acc_get_accValue(IAccessible *self, VARIANT v, BSTR *out)
{
    const A11yModel *m;
    int index;
    A11yItem it;
    if (out) *out = NULL;
    HRESULT hr = Resolve((A11yObject *)self, v, &m, &index, &it);
    if (FAILED(hr)) return hr;
    return ReturnString(it.value, out);
}

static HRESULT STDMETHODCALLTYPE Acc_get_accDescription(IAccessible *self, VARIANT v, BSTR *out)
{
    (void)self; (void)v;
    return ReturnString(NULL, out);
}

static HRESULT STDMETHODCALLTYPE Acc_get_accRole(IAccessible *self, VARIANT v, VARIANT *out)
{
    const A11yModel *m;
    int index;
    A11yItem it;
    if (!out) return E_POINTER;
    VariantInit(out);
    HRESULT hr = Resolve((A11yObject *)self, v, &m, &index, &it);
    if (FAILED(hr)) return hr;
    out->vt = VT_I4;
    out->lVal = it.role;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_get_accState(IAccessible *self, VARIANT v, VARIANT *out)
{
    A11yObject *o = (A11yObject *)self;
    const A11yModel *m;
    int index;
    A11yItem it;
    if (!out) return E_POINTER;
    VariantInit(out);
    HRESULT hr = Resolve(o, v, &m, &index, &it);
    if (FAILED(hr)) return hr;

    DWORD state = it.state;
    if (!IsWindowVisible(o->hwnd))
        state |= STATE_SYSTEM_INVISIBLE;
    if (IsForeground(o->hwnd) && m->focused(m->ctx) == index)
        state |= STATE_SYSTEM_FOCUSED;
    out->vt = VT_I4;
    out->lVal = (LONG)state;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_get_accHelp(IAccessible *self, VARIANT v, BSTR *out)
{
    (void)self; (void)v;
    return ReturnString(NULL, out);
}

static HRESULT STDMETHODCALLTYPE Acc_get_accHelpTopic(IAccessible *self, BSTR *file, VARIANT v, long *topic)
{
    (void)self; (void)v;
    if (file) *file = NULL;
    if (topic) *topic = 0;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE Acc_get_accKeyboardShortcut(IAccessible *self, VARIANT v, BSTR *out)
{
    (void)self; (void)v;
    return ReturnString(NULL, out);
}

static HRESULT STDMETHODCALLTYPE Acc_get_accFocus(IAccessible *self, VARIANT *out)
{
    A11yObject *o = (A11yObject *)self;
    const A11yModel *m = ModelOf(o);
    if (!out) return E_POINTER;
    VariantInit(out);
    if (!m) return CO_E_OBJNOTCONNECTED;
    if (!IsForeground(o->hwnd))
        return S_FALSE;   /* VT_EMPTY: nothing here has the focus */
    int f = m->focused(m->ctx);
    out->vt = VT_I4;
    out->lVal = (f >= 0) ? f + 1 : CHILDID_SELF;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_get_accSelection(IAccessible *self, VARIANT *out)
{
    (void)self;
    if (!out) return E_POINTER;
    VariantInit(out);
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE Acc_get_accDefaultAction(IAccessible *self, VARIANT v, BSTR *out)
{
    const A11yModel *m;
    int index;
    A11yItem it;
    if (out) *out = NULL;
    HRESULT hr = Resolve((A11yObject *)self, v, &m, &index, &it);
    if (FAILED(hr)) return hr;
    return ReturnString(it.action, out);
}

static HRESULT STDMETHODCALLTYPE Acc_accSelect(IAccessible *self, long flags, VARIANT v)
{
    (void)self; (void)flags; (void)v;
    return DISP_E_MEMBERNOTFOUND;
}

static HRESULT STDMETHODCALLTYPE Acc_accLocation(IAccessible *self, long *x, long *y,
                                                 long *w, long *h, VARIANT v)
{
    A11yObject *o = (A11yObject *)self;
    const A11yModel *m;
    int index;
    A11yItem it;
    if (!x || !y || !w || !h) return E_POINTER;
    HRESULT hr = Resolve(o, v, &m, &index, &it);
    if (FAILED(hr)) return hr;

    RECT rc;
    if (index < 0) {
        GetWindowRect(o->hwnd, &rc);
    } else {
        rc = it.rect;
        MapWindowPoints(o->hwnd, NULL, (POINT *)&rc, 2);
    }
    *x = rc.left;
    *y = rc.top;
    *w = rc.right - rc.left;
    *h = rc.bottom - rc.top;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_accNavigate(IAccessible *self, long dir, VARIANT start, VARIANT *out)
{
    const A11yModel *m;
    int index;
    if (!out) return E_POINTER;
    VariantInit(out);
    HRESULT hr = Resolve((A11yObject *)self, start, &m, &index, NULL);
    if (FAILED(hr)) return hr;

    int count = m->count(m->ctx);
    int target = -2;   /* -2 = no such element */
    switch (dir) {
    case NAVDIR_FIRSTCHILD:
        if (index < 0 && count > 0) target = 0;
        break;
    case NAVDIR_LASTCHILD:
        if (index < 0 && count > 0) target = count - 1;
        break;
    case NAVDIR_NEXT:
    case NAVDIR_DOWN:
        if (index >= 0 && index + 1 < count) target = index + 1;
        break;
    case NAVDIR_PREVIOUS:
    case NAVDIR_UP:
        if (index > 0) target = index - 1;
        break;
    default:
        break;
    }
    if (target < 0)
        return S_FALSE;
    out->vt = VT_I4;
    out->lVal = target + 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_accHitTest(IAccessible *self, long x, long y, VARIANT *out)
{
    A11yObject *o = (A11yObject *)self;
    const A11yModel *m = ModelOf(o);
    if (!out) return E_POINTER;
    VariantInit(out);
    if (!m) return CO_E_OBJNOTCONNECTED;

    POINT pt = { x, y };
    RECT rcWin;
    GetWindowRect(o->hwnd, &rcWin);
    if (!PtInRect(&rcWin, pt))
        return S_FALSE;

    ScreenToClient(o->hwnd, &pt);
    int count = m->count(m->ctx);
    for (int i = 0; i < count; i++) {
        A11yItem it;
        memset(&it, 0, sizeof(it));
        m->describe(m->ctx, i, &it);
        if (PtInRect(&it.rect, pt)) {
            out->vt = VT_I4;
            out->lVal = i + 1;
            return S_OK;
        }
    }
    out->vt = VT_I4;
    out->lVal = CHILDID_SELF;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_accDoDefaultAction(IAccessible *self, VARIANT v)
{
    const A11yModel *m;
    int index;
    HRESULT hr = Resolve((A11yObject *)self, v, &m, &index, NULL);
    if (FAILED(hr)) return hr;
    if (!m->invoke || !m->invoke(m->ctx, index))
        return DISP_E_MEMBERNOTFOUND;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Acc_put_accName(IAccessible *self, VARIANT v, BSTR name)
{
    (void)self; (void)v; (void)name;
    return E_NOTIMPL;   /* deprecated in MSAA */
}

static HRESULT STDMETHODCALLTYPE Acc_put_accValue(IAccessible *self, VARIANT v, BSTR value)
{
    (void)self; (void)v; (void)value;
    return DISP_E_MEMBERNOTFOUND;
}

static IAccessibleVtbl g_accVtbl = {
    Acc_QueryInterface, Acc_AddRef, Acc_Release,
    Acc_GetTypeInfoCount, Acc_GetTypeInfo, Acc_GetIDsOfNames, Acc_Invoke,
    Acc_get_accParent, Acc_get_accChildCount, Acc_get_accChild,
    Acc_get_accName, Acc_get_accValue, Acc_get_accDescription,
    Acc_get_accRole, Acc_get_accState, Acc_get_accHelp, Acc_get_accHelpTopic,
    Acc_get_accKeyboardShortcut, Acc_get_accFocus, Acc_get_accSelection,
    Acc_get_accDefaultAction, Acc_accSelect, Acc_accLocation, Acc_accNavigate,
    Acc_accHitTest, Acc_accDoDefaultAction, Acc_put_accName, Acc_put_accValue
};

/* ---- Public API ---- */

void A11y_Attach(HWND hwnd, const A11yModel *model)
{
    SetPropW(hwnd, A11Y_PROP, (HANDLE)model);
}

void A11y_Detach(HWND hwnd)
{
    RemovePropW(hwnd, A11Y_PROP);
}

BOOL A11y_HandleGetObject(HWND hwnd, WPARAM wParam, LPARAM lParam, LRESULT *result)
{
    if ((LONG)lParam != OBJID_CLIENT || !GetPropW(hwnd, A11Y_PROP))
        return FALSE;

    A11yObject *o = (A11yObject *)HeapAlloc(GetProcessHeap(), 0, sizeof(A11yObject));
    if (!o)
        return FALSE;
    o->iface.lpVtbl = &g_accVtbl;
    o->refs = 1;
    o->hwnd = hwnd;
    /* LresultFromObject takes its own reference; ours is dropped right after. */
    *result = LresultFromObject(&kIID_IAccessible, wParam, (IUnknown *)&o->iface);
    Acc_Release(&o->iface);
    return TRUE;
}

static LONG ChildId(int index)
{
    return (index >= 0) ? index + 1 : CHILDID_SELF;
}

void A11y_NotifyFocus(HWND hwnd, int index)
{
    NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, ChildId(index));
}

void A11y_NotifyValue(HWND hwnd, int index)
{
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd, OBJID_CLIENT, ChildId(index));
}

void A11y_NotifyName(HWND hwnd, int index)
{
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_CLIENT, ChildId(index));
}

void A11y_NotifyState(HWND hwnd, int index)
{
    NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd, OBJID_CLIENT, ChildId(index));
}

void A11y_NotifyMenuPopup(HWND hwnd, BOOL start)
{
    NotifyWinEvent(start ? EVENT_SYSTEM_MENUPOPUPSTART : EVENT_SYSTEM_MENUPOPUPEND,
                   hwnd, OBJID_CLIENT, CHILDID_SELF);
}

/* ---- Announcements ----
   A hotkey change is spoken as a live region on the OSD. NVDA accepts the
   WinEvent from a WS_EX_TOPMOST window even while another application has the
   focus, and speaks the window's name.

   Narrator does not speak it while another application has the focus. A UIA
   LiveRegionChanged and a UIA Notification from a native provider on the OSD
   were both delivered to UIA clients (measured with docs/a11y-events.ps1) and
   Narrator still said nothing; it ignores announcements from background
   applications. The provider was removed again, because it also moved NVDA
   off the MSAA path that its source shows to work here. */

/* Dynamic Annotation sets the UIA LiveSetting that MSAA cannot express, so
   the UIA view of the window is a proper live region. Ids are defined here for
   the same reason as the IIDs above. */
static const CLSID kCLSID_AccPropServices =
    { 0xb5f8350b, 0x0548, 0x48b1, { 0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7 } };
static const IID kIID_IAccPropServices =
    { 0x6e26e776, 0x04f0, 0x495d, { 0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69 } };
static const GUID kLiveSettingProperty =   /* LiveSetting_Property_GUID */
    { 0xc12bcd8e, 0x2a8e, 0x4950, { 0x8a, 0xe7, 0x36, 0x25, 0x11, 0x1d, 0x58, 0xeb } };
#define LIVE_SETTING_ASSERTIVE 2   /* LiveSetting: Off 0, Polite 1, Assertive 2 */

static IAccPropServices *AccPropServices(void)
{
    IAccPropServices *svc = NULL;
    if (FAILED(CoCreateInstance(&kCLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER,
                                &kIID_IAccPropServices, (void **)&svc)))
        return NULL;
    return svc;
}

void A11y_MarkLiveRegion(HWND hwnd)
{
    IAccPropServices *svc = AccPropServices();
    if (!svc)
        return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = LIVE_SETTING_ASSERTIVE;
    svc->lpVtbl->SetHwndProp(svc, hwnd, (DWORD)OBJID_CLIENT, CHILDID_SELF,
                             kLiveSettingProperty, v);
    svc->lpVtbl->Release(svc);
}

void A11y_UnmarkLiveRegion(HWND hwnd)
{
    IAccPropServices *svc = AccPropServices();
    if (!svc)
        return;
    svc->lpVtbl->ClearHwndProps(svc, hwnd, (DWORD)OBJID_CLIENT, CHILDID_SELF,
                                &kLiveSettingProperty, 1);
    svc->lpVtbl->Release(svc);
}

void A11y_Announce(HWND hwnd)
{
    if (hwnd)
        NotifyWinEvent(EVENT_OBJECT_LIVEREGIONCHANGED, hwnd, OBJID_CLIENT, CHILDID_SELF);
}
