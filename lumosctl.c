/* lumosctl: command line control for a running Lumos.

   A console program, so cmd and PowerShell wait for it, print its output and
   see its exit code. It sends one request to the running Lumos (see ipc.h)
   and prints the reply. */

#include <windows.h>
#include <stdio.h>
#include "ipc.h"
#include "resource.h"

enum {
    EXIT_OK = 0,
    EXIT_USAGE = 1,
    EXIT_NOT_RUNNING = 2,
    EXIT_FAILED = 3,
    EXIT_NO_ANSWER = 4
};

static WCHAR g_reply[IPC_REPLY_MAX];
static BOOL  g_gotReply = FALSE;
static int   g_replyResult = 0;
static HWND  g_lumos = NULL;   /* replies are accepted only from this window */

/* Console output that also works when redirected to a file or a pipe: the
   console gets UTF-16 directly, anything else gets UTF-8. */
/* Returns FALSE when the text could not be written completely (a full disk
   or a closed pipe), so the exit code does not claim a result nobody got. */
static BOOL Write(HANDLE h, const WCHAR *text)
{
    DWORD mode, written = 0;
    int len = lstrlenW(text);
    if (len == 0)
        return TRUE;
    if (GetConsoleMode(h, &mode))
        return WriteConsoleW(h, text, (DWORD)len, &written, NULL) && written == (DWORD)len;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, len, NULL, 0, NULL, NULL);
    if (bytes <= 0)
        return FALSE;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!buf)
        return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, text, len, buf, bytes, NULL, NULL);
    BOOL ok = WriteFile(h, buf, (DWORD)bytes, &written, NULL) && written == (DWORD)bytes;
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

static BOOL Out(const WCHAR *text) { return Write(GetStdHandle(STD_OUTPUT_HANDLE), text); }
static BOOL Err(const WCHAR *text) { return Write(GetStdHandle(STD_ERROR_HANDLE), text); }

static const WCHAR kUsage[] =
    L"lumosctl: control the brightness through the running Lumos.\n"
    L"\n"
    L"Usage: lumosctl COMMAND [--monitor N|NAME]\n"
    L"\n"
    L"Commands:\n"
    L"  --set N              set the brightness to N percent (0 to 100)\n"
    L"  --up [N]             raise it by N, or by the Lumos brightness step\n"
    L"  --down [N]           lower it by N, or by the Lumos brightness step\n"
    L"  --get                print the current levels\n"
    L"  --list               list the monitors with their numbers\n"
    L"  --preset NAME        apply a preset, for example Night\n"
    L"  --schedule on|off    turn the brightness schedule on or off\n"
    L"  --idle-dim on|off    turn dim when idle on or off\n"
    L"  --rescan             look for monitors again\n"
    L"  --version, --help\n"
    L"\n"
    L"Without --monitor, --set, --up and --down work like the All Monitors slider:\n"
    L"each monitor keeps its offset. --monitor takes a number from --list or a\n"
    L"name (or a unique part of it) and sets that monitor alone.\n"
    L"\n"
    L"Exit codes: 0 done, 1 wrong usage, 2 Lumos is not running,\n"
    L"3 the command failed, 4 Lumos did not answer.\n";

static LRESULT CALLBACK ReplyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lParam;
        if (cds && (HWND)wParam == g_lumos && cds->dwData == IPC_REPLY_MAGIC &&
            cds->lpData && cds->cbData == sizeof(IpcReply)) {
            const IpcReply *r = (const IpcReply *)cds->lpData;
            if (r->size == sizeof(IpcReply)) {
                memcpy(g_reply, r->text, sizeof(g_reply));
                g_reply[IPC_REPLY_MAX - 1] = L'\0';
                g_replyResult = r->result;
                g_gotReply = TRUE;
                return TRUE;
            }
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Pump messages until the reply has arrived or the time is up. The reply is
   a message sent to our window, so it is delivered while we wait here. */
static void WaitForReply(DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    while (!g_gotReply) {
        DWORD spent = GetTickCount() - start;
        if (spent >= timeoutMs)
            return;
        MsgWaitForMultipleObjects(0, NULL, FALSE, timeoutMs - spent, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

int wmain(int argc, wchar_t **argv)
{
    CliCommand cmd;
    WCHAR err[200];
    if (!Cli_Parse(argc - 1, argv + 1, &cmd, err, 200)) {
        Err(L"lumosctl: ");
        Err(err);
        Err(L"\nRun lumosctl --help for the commands.\n");
        return EXIT_USAGE;
    }
    if (cmd.command == CLI_HELP)
        return Out(kUsage) ? EXIT_OK : EXIT_FAILED;
    if (cmd.command == CLI_VERSION)
        return Out(L"lumosctl " APP_VERSION L"\n") ? EXIT_OK : EXIT_FAILED;

    HWND lumos = FindWindowW(LUMOS_MAIN_CLASS, NULL);
    g_lumos = lumos;
    if (!lumos) {
        Err(L"Lumos is not running.\n");
        return EXIT_NOT_RUNNING;
    }

    /* A message-only window receives the reply. The filter lets a Lumos at a
       lower integrity level answer when lumosctl runs in an elevated terminal. */
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = ReplyProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"LumosCtlReply";
    RegisterClassW(&wc);
    HWND replyWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!replyWnd) {
        Err(L"lumosctl: could not create a window for the reply.\n");
        return EXIT_FAILED;
    }
    ChangeWindowMessageFilterEx(replyWnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);

    IpcRequest q;
    memset(&q, 0, sizeof(q));
    q.size = sizeof(q);
    q.command = cmd.command;
    q.value = cmd.value;
    lstrcpynW(q.name, cmd.name, CLI_NAME_MAX);
    lstrcpynW(q.monitor, cmd.monitor, CLI_NAME_MAX);

    COPYDATASTRUCT cds;
    cds.dwData = IPC_REQUEST_MAGIC;
    cds.cbData = sizeof(q);
    cds.lpData = &q;

    /* Lumos accepts the request at once and runs it afterwards (see ipc.h).
       Thirty seconds covers DDC writes to several slow monitors, close to a
       second each on some DisplayPort panels. */
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(lumos, WM_COPYDATA, (WPARAM)replyWnd, (LPARAM)&cds,
                             SMTO_ABORTIFHUNG, 10000, &result)) {
        Err(L"Lumos did not answer.\n");
        return EXIT_NO_ANSWER;
    }
    if (result == 0) {
        Err(L"The running Lumos is too old for this lumosctl. Update Lumos.\n");
        return EXIT_NO_ANSWER;
    }
    if (result == IPC_RESULT_ACCEPTED)
        WaitForReply(30000);

    if (!g_gotReply) {
        Err(L"Lumos did not send a reply.\n");
        return EXIT_NO_ANSWER;
    }
    if (g_replyResult != IPC_RESULT_OK) {
        Err(g_reply);
        return EXIT_FAILED;
    }
    return Out(g_reply) ? EXIT_OK : EXIT_FAILED;
}
