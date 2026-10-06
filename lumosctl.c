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

/* Console output that also works when redirected to a file or a pipe: the
   console gets UTF-16 directly, anything else gets UTF-8. */
static void Write(HANDLE h, const WCHAR *text)
{
    DWORD mode, written;
    int len = lstrlenW(text);
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, text, (DWORD)len, &written, NULL);
        return;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, len, NULL, 0, NULL, NULL);
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, bytes > 0 ? bytes : 1);
    if (!buf)
        return;
    WideCharToMultiByte(CP_UTF8, 0, text, len, buf, bytes, NULL, NULL);
    WriteFile(h, buf, (DWORD)bytes, &written, NULL);
    HeapFree(GetProcessHeap(), 0, buf);
}

static void Out(const WCHAR *text) { Write(GetStdHandle(STD_OUTPUT_HANDLE), text); }
static void Err(const WCHAR *text) { Write(GetStdHandle(STD_ERROR_HANDLE), text); }

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
        if (cds && cds->dwData == IPC_REPLY_MAGIC && cds->lpData &&
            cds->cbData >= sizeof(WCHAR) && cds->cbData <= sizeof(g_reply)) {
            memcpy(g_reply, cds->lpData, cds->cbData);
            g_reply[IPC_REPLY_MAX - 1] = L'\0';
            g_reply[cds->cbData / sizeof(WCHAR) - 1] = L'\0';
            g_gotReply = TRUE;
            return TRUE;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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
    if (cmd.command == CLI_HELP) {
        Out(kUsage);
        return EXIT_OK;
    }
    if (cmd.command == CLI_VERSION) {
        Out(L"lumosctl " APP_VERSION L"\n");
        return EXIT_OK;
    }

    HWND lumos = FindWindowW(LUMOS_MAIN_CLASS, NULL);
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

    /* The reply arrives as a message sent to replyWnd while this call waits;
       Windows dispatches it here, so no message loop is needed. Ten seconds
       covers a DDC write to a slow monitor. */
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(lumos, WM_COPYDATA, (WPARAM)replyWnd, (LPARAM)&cds,
                             SMTO_ABORTIFHUNG, 10000, &result)) {
        Err(L"Lumos did not answer.\n");
        return EXIT_NO_ANSWER;
    }
    if (result == 0) {
        Err(L"The running Lumos is too old for lumosctl. Update Lumos.\n");
        return EXIT_NO_ANSWER;
    }

    if (g_gotReply) {
        if (result == IPC_RESULT_OK) Out(g_reply);
        else                         Err(g_reply);
    }
    return (result == IPC_RESULT_OK) ? EXIT_OK : EXIT_FAILED;
}
