#include "cliparse.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

/* Parse a fixed argument list; returns the result of Cli_Parse. */
static int P(CliCommand *c, int argc, const wchar_t *a0, const wchar_t *a1,
             const wchar_t *a2, const wchar_t *a3)
{
    wchar_t *argv[4] = { (wchar_t *)a0, (wchar_t *)a1, (wchar_t *)a2, (wchar_t *)a3 };
    wchar_t err[160];
    return Cli_Parse(argc, argv, c, err, 160);
}

int main(void)
{
    CliCommand c;

    CHECK(P(&c, 0, NULL, NULL, NULL, NULL) && c.command == CLI_HELP, "no args shows help");
    CHECK(P(&c, 1, L"/?", NULL, NULL, NULL) && c.command == CLI_HELP, "/? is help");

    CHECK(P(&c, 2, L"--set", L"40", NULL, NULL) && c.command == CLI_SET && c.value == 40, "set 40");
    CHECK(P(&c, 2, L"--SET", L"0", NULL, NULL) && c.value == 0, "set is case-insensitive, 0 allowed");
    CHECK(!P(&c, 2, L"--set", L"101", NULL, NULL), "set above 100 rejected");
    CHECK(!P(&c, 2, L"--set", L"4x", NULL, NULL), "set non-number rejected");
    CHECK(!P(&c, 1, L"--set", NULL, NULL, NULL), "set without value rejected");

    CHECK(P(&c, 1, L"--up", NULL, NULL, NULL) && c.command == CLI_UP && c.value == 0, "up uses the default step");
    CHECK(P(&c, 2, L"--down", L"10", NULL, NULL) && c.command == CLI_DOWN && c.value == 10, "down 10");
    CHECK(!P(&c, 2, L"--down", L"0", NULL, NULL), "step 0 rejected");

    CHECK(P(&c, 3, L"--set", L"30", L"--monitor", L"2") == 0, "monitor flag without value rejected");
    CHECK(P(&c, 4, L"--set", L"30", L"--monitor", L"2") && wcscmp(c.monitor, L"2") == 0, "set with monitor number");
    CHECK(P(&c, 4, L"-m", L"DELL U2414H", L"--up", L"5") && wcscmp(c.monitor, L"DELL U2414H") == 0 &&
          c.command == CLI_UP && c.value == 5, "monitor name before the command");
    CHECK(!P(&c, 3, L"--preset", L"Night", L"-m", NULL), "dangling -m rejected");
    CHECK(!P(&c, 4, L"--preset", L"Night", L"-m", L"1"), "monitor with preset rejected");

    CHECK(P(&c, 2, L"--preset", L"Night", NULL, NULL) && c.command == CLI_PRESET && wcscmp(c.name, L"Night") == 0, "preset");
    CHECK(!P(&c, 1, L"--preset", NULL, NULL, NULL), "preset without name rejected");

    CHECK(P(&c, 2, L"--schedule", L"on", NULL, NULL) && c.command == CLI_SCHEDULE && c.value == 1, "schedule on");
    CHECK(P(&c, 2, L"--idle-dim", L"OFF", NULL, NULL) && c.command == CLI_IDLE_DIM && c.value == 0, "idle dim off");
    CHECK(!P(&c, 2, L"--schedule", L"maybe", NULL, NULL), "schedule needs on or off");

    CHECK(P(&c, 1, L"--rescan", NULL, NULL, NULL) && c.command == CLI_RESCAN, "rescan");
    CHECK(P(&c, 1, L"--list", NULL, NULL, NULL) && c.command == CLI_LIST, "list");
    CHECK(P(&c, 1, L"--get", NULL, NULL, NULL) && c.command == CLI_GET, "get");

    CHECK(!P(&c, 2, L"--get", L"--list", NULL, NULL), "two commands rejected");
    CHECK(!P(&c, 1, L"--brighter", NULL, NULL, NULL), "unknown option rejected");
    CHECK(!P(&c, 2, L"-m", L"1", NULL, NULL), "monitor alone rejected");
    CHECK(!P(&c, 4, L"--set", L"5", L"--monitor", L""), "empty monitor name rejected");

    if (failures == 0)
        printf("All cliparse tests passed\n");
    return failures ? 1 : 0;
}
