#define COBJMACROS
#include "wmibright.h"
#include "diagnostics.h"
#include <wbemidl.h>
#include <oleauto.h>
#include <objbase.h>

/*
 * All COM/WMI plumbing lives here so monitor.c stays a clean DDC module.
 * Each public entry point performs its own COM init/teardown, so callers
 * need no global COM lifecycle management.
 */

/* Connect to root\WMI. On success fills *ppSvc and returns TRUE.
 * *pDidInit tells the caller whether it must CoUninitialize afterwards. */
static BOOL WmiConnect(IWbemServices **ppSvc, IWbemLocator **ppLoc, BOOL *pDidInit)
{
    *ppSvc = NULL;
    *ppLoc = NULL;
    *pDidInit = FALSE;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* RPC_E_CHANGED_MODE means COM is already up as STA on this thread; use
     * it, but do NOT uninit (we did not initialize it). */
    if (SUCCEEDED(hr))
        *pDidInit = TRUE;
    else if (hr != RPC_E_CHANGED_MODE) {
        Diagnostics_Log("ERROR", "wmi", "CoInitializeEx FAILED hr=0x%08lX", (DWORD)hr);
        return FALSE;
    }

    /* Process-wide; harmless if already set (RPC_E_TOO_LATE). Ignore result. */
    CoInitializeSecurity(NULL, -1, NULL, NULL,
                         RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE,
                         NULL, EOAC_NONE, NULL);

    IWbemLocator *pLoc = NULL;
    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (LPVOID *)&pLoc);
    if (FAILED(hr) || !pLoc)
        goto fail;

    IWbemServices *pSvc = NULL;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    hr = IWbemLocator_ConnectServer(pLoc, ns, NULL, NULL, NULL, 0, NULL, NULL, &pSvc);
    SysFreeString(ns);
    if (FAILED(hr) || !pSvc) {
        IWbemLocator_Release(pLoc);
        goto fail;
    }

    hr = CoSetProxyBlanket((IUnknown *)pSvc,
                           RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           NULL, EOAC_NONE);
    if (FAILED(hr)) {
        IWbemServices_Release(pSvc);
        IWbemLocator_Release(pLoc);
        goto fail;
    }

    *ppLoc = pLoc;
    *ppSvc = pSvc;
    return TRUE;

fail:
    Diagnostics_Log("ERROR", "wmi", "connection FAILED hr=0x%08lX", (DWORD)hr);
    if (*pDidInit) {
        CoUninitialize();
        *pDidInit = FALSE;
    }
    return FALSE;
}

static void WmiDisconnect(IWbemServices *pSvc, IWbemLocator *pLoc, BOOL didInit)
{
    if (pSvc) IWbemServices_Release(pSvc);
    if (pLoc) IWbemLocator_Release(pLoc);
    if (didInit) CoUninitialize();
}

/* Read a VARIANT property as DWORD, coercing numeric types. */
static BOOL GetPropDword(IWbemClassObject *obj, const WCHAR *name, DWORD *out)
{
    VARIANT v;
    VariantInit(&v);
    HRESULT hr = IWbemClassObject_Get(obj, name, 0, &v, NULL, NULL);
    BOOL ok = FALSE;
    if (SUCCEEDED(hr) && v.vt != VT_NULL && v.vt != VT_EMPTY) {
        VARIANT c;
        VariantInit(&c);
        if (SUCCEEDED(VariantChangeType(&c, &v, 0, VT_I4))) {
            *out = (DWORD)c.lVal;
            ok = TRUE;
        }
        VariantClear(&c);
    }
    VariantClear(&v);
    return ok;
}

/* Read a VARIANT string property into a fixed buffer. */
static BOOL GetPropString(IWbemClassObject *obj, const WCHAR *name, WCHAR *out, int outLen)
{
    VARIANT v;
    VariantInit(&v);
    HRESULT hr = IWbemClassObject_Get(obj, name, 0, &v, NULL, NULL);
    BOOL ok = FALSE;
    if (SUCCEEDED(hr) && v.vt == VT_BSTR && v.bstrVal) {
        wcsncpy(out, v.bstrVal, outLen - 1);
        out[outLen - 1] = L'\0';
        ok = TRUE;
    }
    VariantClear(&v);
    return ok;
}

int Wmi_QueryPanels(WmiPanel *out, int max)
{
    Diagnostics_Log("INFO", "wmi", "panel enumeration START");
    if (max <= 0) return 0;

    IWbemServices *pSvc = NULL;
    IWbemLocator *pLoc = NULL;
    BOOL didInit = FALSE;
    if (!WmiConnect(&pSvc, &pLoc, &didInit))
        return 0;

    int found = 0;
    IEnumWbemClassObject *pEnum = NULL;
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM WmiMonitorBrightness");

    HRESULT hr = IWbemServices_ExecQuery(pSvc, lang, query,
                     WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                     NULL, &pEnum);
    SysFreeString(lang);
    SysFreeString(query);

    if (SUCCEEDED(hr) && pEnum) {
        for (;;) {
            IWbemClassObject *obj = NULL;
            ULONG ret = 0;
            hr = IEnumWbemClassObject_Next(pEnum, WBEM_INFINITE, 1, &obj, &ret);
            if (hr != S_OK || ret == 0 || !obj)
                break;

            if (found < max) {
                WmiPanel *p = &out[found];
                p->instanceName[0] = L'\0';
                p->currentBrightness = 0;
                if (GetPropString(obj, L"InstanceName", p->instanceName, WMI_MAX_INSTANCE)) {
                    GetPropDword(obj, L"CurrentBrightness", &p->currentBrightness);
                    found++;
                }
            }
            IWbemClassObject_Release(obj);
        }
        IEnumWbemClassObject_Release(pEnum);
    }

    WmiDisconnect(pSvc, pLoc, didInit);
    Diagnostics_Log(FAILED(hr) ? "WARN" : "INFO", "wmi", "panel enumeration END count=%d lastHr=0x%08lX", found, (DWORD)hr);
    return found;
}

BOOL Wmi_GetBrightness(const WCHAR *instanceName, DWORD *outPercent)
{
    if (!instanceName || !outPercent) return FALSE;

    IWbemServices *pSvc = NULL;
    IWbemLocator *pLoc = NULL;
    BOOL didInit = FALSE;
    if (!WmiConnect(&pSvc, &pLoc, &didInit)) {
        Diagnostics_Log("ERROR", "wmi", "brightness read FAILED id=\"WMI:%ls\" reason=connection", instanceName);
        return FALSE;
    }

    BOOL ok = FALSE;
    IEnumWbemClassObject *pEnum = NULL;
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM WmiMonitorBrightness");

    HRESULT hr = IWbemServices_ExecQuery(pSvc, lang, query,
                     WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                     NULL, &pEnum);
    SysFreeString(lang);
    SysFreeString(query);

    if (SUCCEEDED(hr) && pEnum) {
        for (;;) {
            IWbemClassObject *obj = NULL;
            ULONG ret = 0;
            hr = IEnumWbemClassObject_Next(pEnum, WBEM_INFINITE, 1, &obj, &ret);
            if (hr != S_OK || ret == 0 || !obj)
                break;

            WCHAR inst[WMI_MAX_INSTANCE];
            if (GetPropString(obj, L"InstanceName", inst, WMI_MAX_INSTANCE)
                && _wcsicmp(inst, instanceName) == 0) {
                ok = GetPropDword(obj, L"CurrentBrightness", outPercent);
                IWbemClassObject_Release(obj);
                break;
            }
            IWbemClassObject_Release(obj);
        }
        IEnumWbemClassObject_Release(pEnum);
    }

    WmiDisconnect(pSvc, pLoc, didInit);
    Diagnostics_Log(ok ? "INFO" : "ERROR", "wmi", "brightness read %s id=\"WMI:%ls\" lastHr=0x%08lX",
                    ok ? "OK" : "FAILED", instanceName, (DWORD)hr);
    return ok;
}

BOOL Wmi_SetBrightness(const WCHAR *instanceName, DWORD percent)
{
    if (!instanceName) return FALSE;
    if (percent > 100) percent = 100;

    IWbemServices *pSvc = NULL;
    IWbemLocator *pLoc = NULL;
    BOOL didInit = FALSE;
    if (!WmiConnect(&pSvc, &pLoc, &didInit)) {
        Diagnostics_Log("ERROR", "wmi", "brightness write FAILED id=\"WMI:%ls\" percent=%lu reason=connection", instanceName, percent);
        return FALSE;
    }

    BOOL ok = FALSE;

    /* Locate the matching WmiMonitorBrightnessMethods instance and use its
     * own __PATH for ExecMethod. That avoids hand-escaping backslashes in
     * the instance key value. */
    IEnumWbemClassObject *pEnum = NULL;
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");

    HRESULT hr = IWbemServices_ExecQuery(pSvc, lang, query,
                     WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                     NULL, &pEnum);
    SysFreeString(lang);
    SysFreeString(query);

    if (SUCCEEDED(hr) && pEnum) {
        for (;;) {
            IWbemClassObject *obj = NULL;
            ULONG ret = 0;
            hr = IEnumWbemClassObject_Next(pEnum, WBEM_INFINITE, 1, &obj, &ret);
            if (hr != S_OK || ret == 0 || !obj)
                break;

            WCHAR inst[WMI_MAX_INSTANCE];
            if (GetPropString(obj, L"InstanceName", inst, WMI_MAX_INSTANCE)
                && _wcsicmp(inst, instanceName) == 0) {

                /* __PATH = \\HOST\ROOT\WMI:Class.InstanceName="..." with the
                 * instance backslashes doubled, so leave generous headroom. */
                WCHAR objPath[512];
                if (GetPropString(obj, L"__PATH", objPath, 512)) {
                    /* Build in-params for WmiSetBrightness(Timeout, Brightness) */
                    IWbemClassObject *pClass = NULL;
                    BSTR bClass = SysAllocString(L"WmiMonitorBrightnessMethods");
                    hr = IWbemServices_GetObject(pSvc, bClass, 0, NULL, &pClass, NULL);
                    SysFreeString(bClass);

                    if (SUCCEEDED(hr) && pClass) {
                        IWbemClassObject *pInDef = NULL;
                        hr = IWbemClassObject_GetMethod(pClass, L"WmiSetBrightness", 0, &pInDef, NULL);
                        if (SUCCEEDED(hr) && pInDef) {
                            IWbemClassObject *pIn = NULL;
                            hr = IWbemClassObject_SpawnInstance(pInDef, 0, &pIn);
                            if (SUCCEEDED(hr) && pIn) {
                                VARIANT vt, vb;
                                VariantInit(&vt);
                                VariantInit(&vb);
                                vt.vt = VT_I4; vt.lVal = 0;              /* Timeout (s) */
                                vb.vt = VT_I4; vb.lVal = (LONG)percent;  /* Brightness  */
                                IWbemClassObject_Put(pIn, L"Timeout", 0, &vt, 0);
                                IWbemClassObject_Put(pIn, L"Brightness", 0, &vb, 0);
                                VariantClear(&vt);
                                VariantClear(&vb);

                                BSTR bPath = SysAllocString(objPath);
                                BSTR bMeth = SysAllocString(L"WmiSetBrightness");
                                hr = IWbemServices_ExecMethod(pSvc, bPath, bMeth, 0, NULL,
                                                              pIn, NULL, NULL);
                                SysFreeString(bPath);
                                SysFreeString(bMeth);
                                ok = SUCCEEDED(hr);
                                IWbemClassObject_Release(pIn);
                            }
                            IWbemClassObject_Release(pInDef);
                        }
                        IWbemClassObject_Release(pClass);
                    }
                }
                IWbemClassObject_Release(obj);
                break;
            }
            IWbemClassObject_Release(obj);
        }
        IEnumWbemClassObject_Release(pEnum);
    }

    WmiDisconnect(pSvc, pLoc, didInit);
    Diagnostics_Log(ok ? "INFO" : "ERROR", "wmi", "brightness write %s id=\"WMI:%ls\" percent=%lu lastHr=0x%08lX",
                    ok ? "APPLIED" : "FAILED", instanceName, percent, (DWORD)hr);
    return ok;
}
