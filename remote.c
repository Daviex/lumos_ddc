#include "remote.h"
#include "ipc.h"
#include <wchar.h>
#include <stdarg.h>

static const AppControl *g_app = NULL;

void Remote_Init(const AppControl *app)
{
    g_app = app;
}

/* ---- Reply text ---- */

typedef struct {
    WCHAR text[IPC_REPLY_MAX];
    int   len;
} Reply;

static void Say(Reply *r, const WCHAR *fmt, ...)
{
    if (r->len >= IPC_REPLY_MAX - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(r->text + r->len, IPC_REPLY_MAX - 1 - r->len, fmt, ap);
    va_end(ap);
    r->len = (n < 0) ? IPC_REPLY_MAX - 1 : r->len + n;
    r->text[r->len] = L'\0';
}

static void SayMonitor(Reply *r, MonitorList *ml, int i)
{
    BrightMonitor *mon = &ml->monitors[i];
    if (mon->controllable)
        Say(r, L"%d. %ls: %d%%\n", i + 1, mon->name, Monitor_GetPercent(mon));
    else
        Say(r, L"%d. %ls: unavailable\n", i + 1, mon->name);
}

static void SayLevels(Reply *r, MonitorList *ml)
{
    Say(r, L"All monitors: %d%%\n", g_app->masterLevel());
    for (int i = 0; i < ml->count; i++)
        SayMonitor(r, ml, i);
}

static void SayList(Reply *r, MonitorList *ml)
{
    if (ml->count == 0) {
        Say(r, L"No monitors found.\n");
        return;
    }
    for (int i = 0; i < ml->count; i++) {
        BrightMonitor *mon = &ml->monitors[i];
        if (!mon->controllable) {
            Say(r, L"%d. %ls (unavailable)\n", i + 1, mon->name);
            continue;
        }
        Say(r, L"%d. %ls (%ls, %d%%, offset %+d)\n", i + 1, mon->name,
            mon->backend == BACKEND_WMI ? L"WMI" : L"DDC/CI",
            Monitor_GetPercent(mon), mon->delta);
    }
}

/* ---- Lookup ---- */

static BOOL ContainsNoCase(const WCHAR *hay, const WCHAR *needle)
{
    size_t n = wcslen(needle);
    for (; *hay; hay++)
        if (_wcsnicmp(hay, needle, n) == 0)
            return TRUE;
    return FALSE;
}

/* --monitor value -> index, or -1 with the reason in the reply. A number is
   the position from --list; anything else must match exactly one name,
   ignoring case, either whole or as a part of it. */
static int FindMonitor(Reply *r, MonitorList *ml, const WCHAR *spec)
{
    WCHAR *end = NULL;
    long n = wcstol(spec, &end, 10);
    if (end && *end == L'\0' && end != spec) {
        if (n >= 1 && n <= ml->count)
            return (int)n - 1;
        Say(r, L"There is no monitor %ls. Run lumosctl --list to see them.\n", spec);
        return -1;
    }

    /* Two identical models share a name, so an exact match must be unique too. */
    int exact = -1, exactCount = 0;
    for (int i = 0; i < ml->count; i++)
        if (_wcsicmp(ml->monitors[i].name, spec) == 0) { exact = i; exactCount++; }
    if (exactCount == 1)
        return exact;
    if (exactCount > 1) {
        Say(r, L"More than one monitor is called \"%ls\". Use its number:\n", spec);
        for (int i = 0; i < ml->count; i++)
            if (_wcsicmp(ml->monitors[i].name, spec) == 0)
                SayMonitor(r, ml, i);
        return -1;
    }

    int found = -1, matches = 0;
    for (int i = 0; i < ml->count; i++) {
        if (ContainsNoCase(ml->monitors[i].name, spec)) {
            found = i;
            matches++;
        }
    }
    if (matches == 1)
        return found;
    if (matches == 0) {
        Say(r, L"No monitor matches \"%ls\". Run lumosctl --list to see them.\n", spec);
    } else {
        Say(r, L"\"%ls\" matches more than one monitor:\n", spec);
        for (int i = 0; i < ml->count; i++)
            if (ContainsNoCase(ml->monitors[i].name, spec))
                SayMonitor(r, ml, i);
    }
    return -1;
}

static int FindPreset(Settings *s, const WCHAR *name)
{
    for (int i = 0; i < s->presetCount; i++)
        if (_wcsicmp(s->presets[i].name, name) == 0)
            return i;
    return -1;
}

static void SayWriteFailed(Reply *r)
{
    Say(r, L"A monitor did not accept the new brightness. lumosctl --rescan may help.\n");
}

static int Clamp100(int v)
{
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

/* ---- Commands ---- */

/* The request comes from another process, so its numbers are checked here
   again: cliparse.c guards only lumosctl itself, not any other sender. */
static BOOL ValidRequest(const IpcRequest *q)
{
    switch (q->command) {
    case CLI_SET:      return q->value >= 0 && q->value <= 100;
    case CLI_UP:
    case CLI_DOWN:     return q->value >= 0 && q->value <= 100;   /* 0 = the step setting */
    case CLI_SCHEDULE:
    case CLI_IDLE_DIM: return q->value == 0 || q->value == 1;
    case CLI_GET:
    case CLI_LIST:
    case CLI_PRESET:
    case CLI_RESCAN:   return TRUE;
    default:           return FALSE;
    }
}

/* Runs one request; returns IPC_RESULT_*. */
static int Execute(const IpcRequest *q, Reply *r)
{
    MonitorList *ml = g_app->monitors();
    Settings *s = g_app->settings();

    int mon = -1;
    if (q->monitor[0]) {
        mon = FindMonitor(r, ml, q->monitor);
        if (mon < 0)
            return IPC_RESULT_FAILED;
        if (!ml->monitors[mon].controllable && q->command != CLI_GET) {
            Say(r, L"Monitor %d (%ls) is unavailable.\n", mon + 1, ml->monitors[mon].name);
            return IPC_RESULT_FAILED;
        }
    }

    switch (q->command) {
    case CLI_SET:
    case CLI_UP:
    case CLI_DOWN: {
        if (!Monitor_HasControllable(ml)) {
            Say(r, L"No monitor can be controlled right now.\n");
            return IPC_RESULT_FAILED;
        }
        int step = (q->value > 0) ? q->value : s->step;
        if (q->command == CLI_DOWN)
            step = -step;
        BOOL ok;
        if (mon >= 0) {
            int target = (q->command == CLI_SET) ? q->value
                         : Monitor_GetPercent(&ml->monitors[mon]) + step;
            ok = g_app->setMonitor(mon, Clamp100(target));
        } else if (q->command == CLI_SET) {
            ok = g_app->setMaster(q->value);
        } else {
            ok = g_app->stepMaster(step);
        }
        if (!ok)
            SayWriteFailed(r);
        if (mon >= 0)
            SayMonitor(r, ml, mon);
        else
            SayLevels(r, ml);
        return ok ? IPC_RESULT_OK : IPC_RESULT_FAILED;
    }

    case CLI_GET:
        if (mon >= 0)
            SayMonitor(r, ml, mon);
        else
            SayLevels(r, ml);
        return IPC_RESULT_OK;

    case CLI_LIST:
        SayList(r, ml);
        return IPC_RESULT_OK;

    case CLI_PRESET: {
        int p = FindPreset(s, q->name);
        if (p < 0) {
            Say(r, L"There is no preset \"%ls\". Presets:", q->name);
            for (int i = 0; i < s->presetCount; i++)
                Say(r, L"%ls %ls (%u%%)", i ? L"," : L"", s->presets[i].name, s->presets[i].brightness);
            Say(r, L"\n");
            return IPC_RESULT_FAILED;
        }
        if (!g_app->applyPreset(p)) {
            SayWriteFailed(r);
            SayLevels(r, ml);
            return IPC_RESULT_FAILED;
        }
        Say(r, L"Preset %ls (%u%%) applied.\n", s->presets[p].name, s->presets[p].brightness);
        SayLevels(r, ml);
        return IPC_RESULT_OK;
    }

    case CLI_SCHEDULE:
        g_app->setSchedule(q->value != 0);
        Say(r, L"Brightness schedule is %ls.\n", q->value ? L"on" : L"off");
        if (q->value && s->scheduleCount == 0)
            Say(r, L"It has no points yet; add them with Edit Schedule in the tray menu.\n");
        return IPC_RESULT_OK;

    case CLI_IDLE_DIM:
        g_app->setIdleDim(q->value != 0);
        Say(r, L"Dim when idle is %ls (%d%% after %d minutes).\n", q->value ? L"on" : L"off",
            s->idleDimPercent, s->idleDimMinutes);
        return IPC_RESULT_OK;

    case CLI_RESCAN:
        g_app->rescan();
        Say(r, L"Monitor re-scan started.\n");
        return IPC_RESULT_OK;

    default:
        Say(r, L"This command is not supported by the running Lumos.\n");
        return IPC_RESULT_FAILED;
    }
}

static void SendReply(HWND from, HWND to, int result, const Reply *r)
{
    if (!to || !IsWindow(to))
        return;
    static IpcReply out;   /* 8 KB; static so it stays off the UI thread's stack */
    out.size = sizeof(out);
    out.result = result;
    memcpy(out.text, r->text, (r->len + 1) * sizeof(WCHAR));
    COPYDATASTRUCT back;
    back.dwData = IPC_REPLY_MAGIC;
    back.cbData = sizeof(out);
    back.lpData = &out;
    DWORD_PTR ignored;
    SendMessageTimeoutW(to, WM_COPYDATA, (WPARAM)from, (LPARAM)&back,
                        SMTO_ABORTIFHUNG, 2000, &ignored);
}

BOOL Remote_HandleCopyData(HWND hwnd, WPARAM wParam, LPARAM lParam, LRESULT *result)
{
    const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lParam;
    if (!g_app || !cds || cds->dwData != IPC_REQUEST_MAGIC)
        return FALSE;

    HWND replyTo = (HWND)wParam;
    static Reply reply;   /* static so it stays off the UI thread's stack */
    reply.len = 0;
    reply.text[0] = L'\0';

    /* A command runs after ReplyMessage below, and a WMI write inside it makes
       COM calls whose waits dispatch incoming messages. A second request that
       arrives then is turned away rather than run in the middle of the first. */
    static BOOL busy = FALSE;
    if (busy) {
        Say(&reply, L"Lumos is busy with another lumosctl command. Try again.\n");
        SendReply(hwnd, replyTo, IPC_RESULT_FAILED, &reply);
        *result = IPC_RESULT_FAILED;
        return TRUE;
    }

    /* Everything from another process is checked before it is used: the size
       must match exactly, and both strings get a terminator of our own. The
       data is copied out now, because it is gone once the sender is released. */
    IpcRequest q;
    BOOL wellFormed = (cds->cbData == sizeof(q) && cds->lpData);
    if (wellFormed) {
        memcpy(&q, cds->lpData, sizeof(q));
        wellFormed = (q.size == sizeof(q));
    }
    if (!wellFormed) {
        Say(&reply, L"This lumosctl does not match the running Lumos. Use the lumosctl from the same release.\n");
        SendReply(hwnd, replyTo, IPC_RESULT_FAILED, &reply);
        *result = IPC_RESULT_FAILED;
        return TRUE;
    }
    q.name[CLI_NAME_MAX - 1] = L'\0';
    q.monitor[CLI_NAME_MAX - 1] = L'\0';
    if (!ValidRequest(&q)) {
        Say(&reply, L"The request has an invalid command or value.\n");
        SendReply(hwnd, replyTo, IPC_RESULT_FAILED, &reply);
        *result = IPC_RESULT_FAILED;
        return TRUE;
    }

    /* Release lumosctl before running anything: COM refuses outgoing calls
       while a SendMessage from another process is being handled, and the WMI
       backend is exactly such a call. */
    busy = TRUE;
    ReplyMessage(IPC_RESULT_ACCEPTED);
    int res = Execute(&q, &reply);
    SendReply(hwnd, replyTo, res, &reply);
    busy = FALSE;

    *result = IPC_RESULT_ACCEPTED;   /* already delivered by ReplyMessage */
    return TRUE;
}
