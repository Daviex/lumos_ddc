# Regression tests

`test_settings.c` includes the real `presets.c` implementation and redirects
every filesystem, INI and registry API it uses to in-memory mocks. It verifies
path bounds, quoted autostart commands, safe migration of legacy Run entries,
API errors, idempotent disabling and the configured Day/Giorno preset lookup.
Global monitor selections are also checked for INI round trips, legacy defaults,
bounded identities, duplicates, disconnected displays and empty selections.
Running it does not launch Lumos or change the registry or configuration files.
Input-source rules are covered independently of selection: saved port/filter
round trips, unchecked/offline retention, identity reorder, duplicate/corrupt
rules and fail-closed handling of unassigned inputs.
The source polling interval covers the legacy 3-second default, persistence of
every whole-second value from 1 to 60, and bounds on malformed values.

From the repository root with a Windows Clang/MinGW toolchain in `PATH`:
Create the `build` directory first if it does not exist.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE tests/test_settings.c tests/diagnostics_stub.c schedule.c monitor_selection.c brightmap.c hotkey.c -o build/test_settings.exe
./build/test_settings.exe
```

With MSVC from a developer command prompt:

```bat
cl /nologo /std:c11 /W4 /DUNICODE /D_UNICODE tests\test_settings.c tests\diagnostics_stub.c schedule.c monitor_selection.c brightmap.c hotkey.c /Fe:build\test_settings.exe /Fo"build\\"
build\test_settings.exe
```

`test_monitor_worker.c` includes the real worker and uses actual Win32 events
and threads, with in-memory monitor and result-delivery mocks. Tests hold a
hardware call at an event while verifying that UI requests return promptly,
only the latest queued value is written, stale results are rejected and all
handle leases are released after replacement, reset and shutdown. Pending target
snapshots preserve the latest write intent without including refresh reads.
Refresh and hardware failure paths are also covered. It never accesses physical
monitors.
Source polling includes suspended displays and physical DDC monitors whose
initial brightness read failed. Recovery reports validated brightness separately
from source availability and never performs a native write. Tests block a source read while
changing a target or resetting the worker, ensuring no obsolete native write
follows it. Source telemetry, intentional skips and hardware failures have
distinct results, and brightness refreshes cannot overwrite source telemetry.
Typed idle writes and alternate-input restores retain their purpose, idle cycle
and stable identity across queue replacement and reset. Only actual hardware
writes report applied ownership; refresh reads never do. Idle restores are not
saved as pending user targets.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE tests/test_monitor_worker.c tests/diagnostics_stub.c -o build/test_monitor_worker.exe
./build/test_monitor_worker.exe
```

```bat
cl /nologo /std:c11 /W4 /DUNICODE /D_UNICODE tests\test_monitor_worker.c tests\diagnostics_stub.c /Fe:build\test_monitor_worker.exe /Fo"build\\"
build\test_monitor_worker.exe
```

`test_monitor.c` includes the real monitor module and mocks physical brightness
reads/writes/destruction, WMI and the worker. Enumeration is never invoked. It
checks shared handle leases (including valid handle zero), queued ownership,
nonblocking cleanup during enumeration, rejected invalid readings and scaling
across the full 32-bit brightness range.
Transient source errors are retried once after 100 ms; persistent failures and
invalid replies cannot authorize writes from cached telemetry. Brightness
capability recovers independently, without revoking previously validated values
after another failed read.
DDC recovery tests reproduce a display rejecting a write without a 100 ms command
gap. They cover bounded retries of transmit/receive errors, fresh source checks
between attempts, source changes and cancellation during the waits, unsupported
errors without retries, and preserving the original idle baseline across retries.
Transient brightness reads receive one retry without revoking confirmed values.
Excluded displays are checked at the preview, DDC/WMI write, group and active
monitor boundaries to ensure their brightness remains unchanged.
Idle dimming captures a fresh, validated native brightness only on a successful
write. Restoration uses that exact raw value (including 128 on a 0-255 range),
and is skipped when its final source read reports the PC input, a different
alternate input, or an unknown input. Cancellation prevents pending native writes.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE tests/test_monitor.c tests/diagnostics_stub.c brightness.c brightmap.c -ldxva2 -luser32 -lgdi32 -ladvapi32 -o build/test_monitor.exe
./build/test_monitor.exe
```

```bat
cl /nologo /std:c11 /W4 /DUNICODE /D_UNICODE tests\test_monitor.c tests\diagnostics_stub.c brightness.c brightmap.c /Fe:build\test_monitor.exe /Fo"build\\" /link dxva2.lib user32.lib gdi32.lib advapi32.lib
build\test_monitor.exe
```

`test_ui.c` includes `ui_popup.c` and `ui_graphics.c` with instrumented Windows
APIs, renders the real popup into offscreen DIBs and simulates mouse
messages. It verifies GDI resource reuse and cleanup, pixel stability, resize
and allocation errors, the final drag value, capture loss, cancellation and
master target bookkeeping. Excluded sliders and range controls cannot write, and the master operates only
on the selected displays, including the case where none is available.
Source-filtered rows reject slider/range input on another or unknown source and
become available again when the PC input is reported.
Window and monitor operations are mocked; no window is shown and no monitor or
registry setting is changed.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE tests/test_ui.c brightness.c brightmap.c ui_draw.c -o build/test_ui.exe -lgdi32 -luser32 -lshell32 -ldwmapi
./build/test_ui.exe
```

The existing `test_schedule.c` checks interpolation, midnight wrapping and
schedule suspension/resumption without accessing the system clock or settings.

```powershell
clang -std=c11 -Wall -Wextra -Werror test_schedule.c schedule.c -o build/test_schedule.exe
./build/test_schedule.exe
```

`test_brightness.c` exercises the shared pure calculations and identity matching:
nonzero native bounds, rounding, full DWORD ranges, per-monitor range mapping, master slider
endpoints, renamed/reordered displays and ambiguous monitor identities.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE tests/test_brightness.c brightness.c brightmap.c -o build/test_brightness.exe
./build/test_brightness.exe
```

`test_startup.c` includes the real application orchestration with mocked clock,
timers, windows, settings and monitor I/O. It checks the login argument, manual
launch behavior, Day priority until the next dated schedule anchor, idle and
manual overrides, and recovery of the latest target after delayed monitor
discovery, exhausted retries and failed writes. The application entry point is never called.
Day lookup and registry migration are covered separately by `test_settings.c`.
The real selection helpers also verify that login, presets, idle, schedule,
hotkeys and reordered rescan results leave a second, excluded monitor unchanged.
Source suspension and return also cover latest-target resume, idle/schedule
precedence, reconnect, source-rule edits, filter removal and polling without a
Windows topology event. Skips and unknown reads must never trigger rescans.
Runtime source-timer checks cover all intervals from 1 to 60 seconds and align
the picker freshness window with the saved timer interval.
Idle handoff tests keep the PC idle while only a second monitor changes input:
it restores its original raw brightness, dims again on return, and preserves the
same baseline across repeated switches while leaving the first monitor dimmed.
They also cover failed releases, wake/re-idle with queued work, rule edits, and
applied results superseded by newer requests or a uniquely matched rescan.
Mixed OLED/LCD idle checks verify that black-idle displays receive no idle
brightness write and no manual-policy brightness restore when input returns.
The mixed OLED/LCD wake regression verifies that a failed LCD restore stays
pending after the cover is removed. Recovery retries only the dimmed LCD, uses
the newest intent, respects source/selection, avoids duplicating in-flight work
and stops after success.
Manual idle wake preserves different per-monitor requests and exact native
baselines when no request exists. Queued raw restores remain separate from user
targets; failed writes and late dim acknowledgements retain ownership until
the correct latest target or native baseline has been restored.
Changing an already dimmed display to black idle restores the old brightness
ownership, including failed restores and dim acknowledgements arriving late.

`test_idle_black.c` exercises the overlay with mocked windows, monitor geometry,
last-input timestamps, source selection and power requests. It verifies an
opaque cover over the full monitor (including negative desktop coordinates),
prompt wake, failed input/timer setup, session lock and cleanup. An offscreen
GDI DIB verifies that every painted pixel is RGB 0,0,0. No screen is covered
and no real execution-state request is made.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE tests/test_idle_black.c tests/diagnostics_stub.c brightness.c brightmap.c -lgdi32 -luser32 -o build/test_idle_black.exe
./build/test_idle_black.exe
```

```powershell
clang -O2 -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE -ffunction-sections -fdata-sections tests/test_startup.c tests/diagnostics_stub.c brightness.c brightmap.c schedule.c monitor_selection.c hotkey.c '-Wl,--gc-sections' -lshell32 -o build/test_startup.exe
./build/test_startup.exe
```

`test_monitor_selection_ui.c` exercises the real picker and settings handlers
with mocked window APIs. It checks All/custom choices, Apply/Cancel isolation,
parent Save, retained offline selections, reordered displays, ambiguous identities,
selection limits and keyboard navigation. It does not create windows or access
hardware, the registry or configuration files.
The Settings slider covers whole-second click/drag, wheel and keyboard changes,
exact drag release, capture loss, Save/Cancel isolation and long polling telemetry.
Its real renderer draws to offscreen DIBs; optional `--preview` saves 3-second
and 60-second Settings BMP previews under `build` for visual inspection.

```powershell
clang -O2 -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE -ffunction-sections -fdata-sections tests/test_monitor_selection_ui.c tests/diagnostics_stub.c monitor_selection.c schedule.c ui_graphics.c ui_draw.c hotkey.c '-Wl,--gc-sections' -lgdi32 -luser32 -lshell32 -ldwmapi -lcomctl32 -o build/test_monitor_selection_ui.exe
./build/test_monitor_selection_ui.exe
```

The upstream `test_brightmap.c`, `test_hotkey.c` and `test_cliparse.c` cover
range mapping/inversion, hotkey parsing/formatting and command line parsing.
They perform no hardware writes or configuration changes.

```powershell
clang -std=c11 -Wall -Wextra -Werror test_brightmap.c brightmap.c -o build/test_brightmap.exe
./build/test_brightmap.exe
clang -std=c11 -Wall -Wextra -Werror test_hotkey.c hotkey.c -o build/test_hotkey.exe
./build/test_hotkey.exe
clang -std=c11 -Wall -Wextra -Werror test_cliparse.c cliparse.c -o build/test_cliparse.exe
./build/test_cliparse.exe
```

Settings tests also verify legacy hotkeys and range migration together with
the saved monitor selection, source filters, polling interval and OLED policy.
Startup tests cover partial monitor recovery with steady retries, popup deferral
and command line activity across idle, range mapping and reordered discovery.

`test_remote.c` runs the real command execution with mocked windows, monitor
snapshots and worker actions. It checks cumulative relative commands while a
filtered write is pending, fresh unfiltered readings, selected preset targets,
queued replies, invalid requests and ambiguous monitor names. It performs no
hardware writes, window creation or configuration I/O.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DUNICODE -D_UNICODE tests/test_remote.c -o build/test_remote.exe
./build/test_remote.exe
```

`diagnostics_stub.c` suppresses runtime logging in existing regression suites
(no log file is created). `test_diagnostics.c` uses the real logger with mocked
file APIs and real Win32 threads. It verifies the executable-relative path,
read-only-folder fallback, UTF-8 monitor names/stable identity, timestamps and
process/thread tags, Win32 last-error preservation, concurrent complete entries,
5 MiB rotation, write/rotation failures and late worker logs after shutdown.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DSTRSAFE_NO_DEPRECATE -DUNICODE -D_UNICODE tests/test_diagnostics.c -o build/test_diagnostics.exe
./build/test_diagnostics.exe
```

When using GCC rather than Clang, the startup test's renamed static Windows entry
point requires `-Wno-old-style-declaration` in addition to the documented flags.
