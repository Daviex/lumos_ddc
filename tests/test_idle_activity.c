/* Pure clock/input samples: no Windows calls, input injection or hardware. */
#include <stdio.h>
#include "../idle_activity.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void TestInitialSamples(void)
{
    IdleActivity activity = {0};
    CHECK(IdleActivity_Sample(&activity, 300000, TRUE, 270000) == 30000);
    CHECK(IdleActivity_Sample(&activity, 302000, TRUE, 270000) == 32000);
    activity = (IdleActivity){0};
    CHECK(IdleActivity_Sample(&activity, 100000, TRUE, 100001) == 0);
    CHECK(IdleActivity_Sample(&activity, 160000, TRUE, 100001) == 60000);
    activity = (IdleActivity){0};
    CHECK(IdleActivity_Sample(&activity, 1000, TRUE, 0xffffff00UL) == 0);
    activity = (IdleActivity){0};
    CHECK(IdleActivity_Sample(&activity, 0x100000000ULL + 1000, TRUE,
                              0xffffff00UL) == 1256);
}

static void TestChangedInputAndLongUptime(void)
{
    IdleActivity activity = {0};
    CHECK(IdleActivity_Sample(&activity, 300000, TRUE, 270000) == 30000);
    CHECK(IdleActivity_Sample(&activity, 302000, TRUE, 1000) == 0);
    CHECK(IdleActivity_Sample(&activity, 362000, TRUE, 1000) == 60000);
    CHECK(IdleActivity_Sample(&activity, 362001, TRUE, 400000) == 0);
    CHECK(IdleActivity_Sample(&activity, 362001 + 0x100000000ULL,
                              TRUE, 400000) == MAXDWORD);
    CHECK(IdleActivity_Sample(&activity, 362002 + 0x100000000ULL,
                              TRUE, 400001) == 0);
}

static void TestFailuresAndExplicitActivity(void)
{
    IdleActivity activity = {0};
    CHECK(IdleActivity_Sample(&activity, 300000, TRUE, 0) == 300000);
    CHECK(IdleActivity_Sample(&activity, 302000, FALSE, 0) == 0);
    CHECK(IdleActivity_Sample(&activity, 352000, TRUE, 0) == 0);
    CHECK(IdleActivity_Sample(&activity, 412000, TRUE, 0) == 60000);
    IdleActivity_Record(&activity, 413000);
    CHECK(IdleActivity_Sample(&activity, 414000, TRUE, 0) == 1000);
    CHECK(IdleActivity_Sample(&activity, 474000, TRUE, 0) == 61000);
    activity = (IdleActivity){0};
    IdleActivity_Record(&activity, 300000);
    CHECK(IdleActivity_Sample(&activity, 302000, TRUE, 0) == 2000);
    CHECK(IdleActivity_Sample(&activity, 303000, TRUE, 302500) == 0);
}

int main(void)
{
    TestInitialSamples();
    TestChangedInputAndLongUptime();
    TestFailuresAndExplicitActivity();
    if (failures) return 1;
    puts("ALL PASS: monotonic inactivity, anomalous input, DWORD wrap, query recovery and explicit activity");
    return 0;
}
