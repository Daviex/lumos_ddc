#ifndef CLIPARSE_H
#define CLIPARSE_H

/* Command line of lumosctl.exe. Win32-free so it can be unit-tested with
   native gcc (test_cliparse.c); lumosctl.c turns the result into an
   IpcRequest. */

#include <wchar.h>

enum {
    CLI_NONE = 0,
    CLI_HELP,
    CLI_VERSION,
    CLI_SET,        /* value = 0..100 */
    CLI_UP,         /* value = step, 0 = the Lumos brightness step */
    CLI_DOWN,       /* value = step, 0 = the Lumos brightness step */
    CLI_GET,
    CLI_LIST,
    CLI_PRESET,     /* name = preset name */
    CLI_SCHEDULE,   /* value = 1 on, 0 off */
    CLI_IDLE_DIM,   /* value = 1 on, 0 off */
    CLI_RESCAN
};

#define CLI_NAME_MAX 128

typedef struct {
    int     command;                 /* CLI_* */
    int     value;
    wchar_t name[CLI_NAME_MAX];      /* CLI_PRESET */
    wchar_t monitor[CLI_NAME_MAX];   /* --monitor: a number from --list or a name; empty = all */
} CliCommand;

/* Parse argv (without the program name). Returns 1 on success. On failure it
   returns 0 and writes a one-line reason into err. */
int Cli_Parse(int argc, wchar_t **argv, CliCommand *out, wchar_t *err, int errLen);

#endif /* CLIPARSE_H */
