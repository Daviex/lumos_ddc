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
        Say(r, L"%d. %ls: cannot be controlled\n", i + 1, mon->name);
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
            Say(r, L"%d. %ls (cannot be controlled)\n", i + 1, mon->name);
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

    for (int i = 0; i < ml->count; i++)
        if (_wcsicmp(ml->monitors[i].name, spec) == 0)
            return i;

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

static int Clamp100(int v)
{
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

/* ---- Commands ---- */

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
            Say(r, L"Monitor %d (%ls) cannot be controlled.\n", mon + 1, ml->monitors[mon].name);
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
        if (mon >= 0) {
            int target = (q->command == CLI_SET) ? q->value
                         : Monitor_GetPercent(&ml->monitors[mon]) + step;
            g_app->setMonitor(mon, Clamp100(target));
            SayMonitor(r, ml, mon);
        } else {
            if (q->command == CLI_SET)
                g_app->setMaster(q->value);
            else
                g_app->stepMaster(step);
            SayLevels(r, ml);
        }
        return IPC_RESULT_OK;
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
        g_app->applyPreset(p);
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

BOOL Remote_HandleCopyData(HWND hwnd, WPARAM wParam, LPARAM lParam, LRESULT *result)
{
    const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lParam;
    if (!g_app || !cds || cds->dwData != IPC_REQUEST_MAGIC)
        return FALSE;

    /* Everything from another process is checked before it is used: the size
       must match exactly, and both strings get a terminator of our own. */
    IpcRequest q;
    if (cds->cbData != sizeof(q) || !cds->lpData) {
        *result = IPC_RESULT_FAILED;
        return TRUE;
    }
    memcpy(&q, cds->lpData, sizeof(q));
    if (q.size != sizeof(q)) {
        *result = IPC_RESULT_FAILED;
        return TRUE;
    }
    q.name[CLI_NAME_MAX - 1] = L'\0';
    q.monitor[CLI_NAME_MAX - 1] = L'\0';

    static Reply reply;   /* 8 KB; static so it stays off the UI thread's stack */
    reply.len = 0;
    reply.text[0] = L'\0';
    *result = Execute(&q, &reply);

    HWND replyTo = (HWND)wParam;
    if (replyTo && IsWindow(replyTo)) {
        COPYDATASTRUCT back;
        back.dwData = IPC_REPLY_MAGIC;
        back.cbData = (DWORD)((reply.len + 1) * sizeof(WCHAR));
        back.lpData = reply.text;
        DWORD_PTR ignored;
        SendMessageTimeoutW(replyTo, WM_COPYDATA, (WPARAM)hwnd, (LPARAM)&back,
                            SMTO_ABORTIFHUNG, 2000, &ignored);
    }
    return TRUE;
}
