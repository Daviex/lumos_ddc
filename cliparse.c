#include "cliparse.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    const wchar_t *flag;
    int            command;
    int            arg;       /* ARG_* */
} CliOption;

enum {
    ARG_NONE = 0,
    ARG_PERCENT,        /* required 0..100 */
    ARG_STEP,           /* optional 1..100 */
    ARG_NAME,           /* required text */
    ARG_ONOFF           /* required on/off */
};

static const CliOption kOptions[] = {
    { L"--help",     CLI_HELP,     ARG_NONE },
    { L"-h",         CLI_HELP,     ARG_NONE },
    { L"/?",         CLI_HELP,     ARG_NONE },
    { L"--version",  CLI_VERSION,  ARG_NONE },
    { L"--set",      CLI_SET,      ARG_PERCENT },
    { L"--up",       CLI_UP,       ARG_STEP },
    { L"--down",     CLI_DOWN,     ARG_STEP },
    { L"--get",      CLI_GET,      ARG_NONE },
    { L"--list",     CLI_LIST,     ARG_NONE },
    { L"--preset",   CLI_PRESET,   ARG_NAME },
    { L"--schedule", CLI_SCHEDULE, ARG_ONOFF },
    { L"--idle-dim", CLI_IDLE_DIM, ARG_ONOFF },
    { L"--rescan",   CLI_RESCAN,   ARG_NONE },
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int EqualNoCase(const wchar_t *a, const wchar_t *b)
{
    for (; *a && *b; a++, b++) {
        wchar_t x = *a, y = *b;
        if (x >= L'A' && x <= L'Z') x = (wchar_t)(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = (wchar_t)(y - L'A' + L'a');
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* Whole-string decimal number, or -1. */
static int ParseNumber(const wchar_t *s)
{
    if (!s || !*s)
        return -1;
    long v = 0;
    for (; *s; s++) {
        if (*s < L'0' || *s > L'9')
            return -1;
        v = v * 10 + (*s - L'0');
        if (v > 100000)
            return -1;
    }
    return (int)v;
}

static void SetError(wchar_t *err, int errLen, const wchar_t *fmt, const wchar_t *arg)
{
    if (!err || errLen <= 0)
        return;
    swprintf(err, (size_t)errLen, fmt, arg ? arg : L"");
}

static int CopyName(wchar_t *dst, const wchar_t *src)
{
    if (wcslen(src) >= CLI_NAME_MAX)
        return 0;
    wcscpy(dst, src);
    return 1;
}

int Cli_Parse(int argc, wchar_t **argv, CliCommand *out, wchar_t *err, int errLen)
{
    memset(out, 0, sizeof(*out));
    if (err && errLen > 0)
        err[0] = L'\0';

    if (argc <= 0) {
        out->command = CLI_HELP;   /* no arguments: show the usage */
        return 1;
    }

    for (int i = 0; i < argc; i++) {
        const wchar_t *a = argv[i];

        if (EqualNoCase(a, L"--monitor") || EqualNoCase(a, L"-m")) {
            if (i + 1 >= argc) {
                SetError(err, errLen, L"%ls needs a monitor number or name", a);
                return 0;
            }
            if (out->monitor[0]) {
                SetError(err, errLen, L"--monitor is given twice", NULL);
                return 0;
            }
            if (!argv[i + 1][0]) {
                SetError(err, errLen, L"%ls needs a monitor number or name, not an empty one", a);
                return 0;
            }
            if (!CopyName(out->monitor, argv[++i])) {
                SetError(err, errLen, L"the monitor name is too long", NULL);
                return 0;
            }
            continue;
        }

        const CliOption *opt = NULL;
        for (int k = 0; k < COUNT(kOptions); k++)
            if (EqualNoCase(a, kOptions[k].flag)) { opt = &kOptions[k]; break; }
        if (!opt) {
            SetError(err, errLen, L"unknown option: %ls", a);
            return 0;
        }
        if (out->command != CLI_NONE) {
            SetError(err, errLen, L"only one command at a time (found another: %ls)", a);
            return 0;
        }
        out->command = opt->command;

        const wchar_t *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        switch (opt->arg) {
        case ARG_PERCENT: {
            int v = ParseNumber(next);
            if (v < 0 || v > 100) {
                SetError(err, errLen, L"%ls needs a level from 0 to 100", a);
                return 0;
            }
            out->value = v;
            i++;
            break;
        }
        case ARG_STEP: {
            int v = ParseNumber(next);
            if (v >= 0) {                 /* the step is optional */
                if (v < 1 || v > 100) {
                    SetError(err, errLen, L"%ls takes a step from 1 to 100", a);
                    return 0;
                }
                out->value = v;
                i++;
            }
            break;
        }
        case ARG_NAME:
            if (!next || !next[0] || next[0] == L'-') {
                SetError(err, errLen, L"%ls needs a name", a);
                return 0;
            }
            if (!CopyName(out->name, next)) {
                SetError(err, errLen, L"the name is too long", NULL);
                return 0;
            }
            i++;
            break;
        case ARG_ONOFF:
            if (next && EqualNoCase(next, L"on"))       out->value = 1;
            else if (next && EqualNoCase(next, L"off")) out->value = 0;
            else {
                SetError(err, errLen, L"%ls needs on or off", a);
                return 0;
            }
            i++;
            break;
        default:
            break;
        }
    }

    if (out->command == CLI_NONE) {
        SetError(err, errLen, L"no command given (try --help)", NULL);
        return 0;
    }
    if (out->monitor[0] && out->command != CLI_SET && out->command != CLI_UP &&
        out->command != CLI_DOWN && out->command != CLI_GET) {
        SetError(err, errLen, L"--monitor works only with --set, --up, --down and --get", NULL);
        return 0;
    }
    return 1;
}
