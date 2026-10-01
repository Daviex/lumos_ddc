# Regression tests

`test_settings.c` includes the real `presets.c` implementation and redirects
every filesystem, INI and registry API it uses to in-memory mocks. It verifies
path bounds, quoted autostart commands, API errors and idempotent disabling.
Running it does not launch Lumos or change the registry or configuration files.

From the repository root with a Windows Clang/MinGW toolchain in `PATH`:
Create the `build` directory first if it does not exist.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DUNICODE -D_UNICODE tests/test_settings.c schedule.c -o build/test_settings.exe
./build/test_settings.exe
```

With MSVC from a developer command prompt:

```bat
cl /nologo /std:c11 /W4 /DUNICODE /D_UNICODE tests\test_settings.c schedule.c /Fe:build\test_settings.exe /Fo"build\\"
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

```powershell
clang -std=c11 -Wall -Wextra -Werror -DUNICODE -D_UNICODE tests/test_monitor_worker.c -o build/test_monitor_worker.exe
./build/test_monitor_worker.exe
```

```bat
cl /nologo /std:c11 /W4 /DUNICODE /D_UNICODE tests\test_monitor_worker.c /Fe:build\test_monitor_worker.exe /Fo"build\\"
build\test_monitor_worker.exe
```

`test_monitor.c` includes the real monitor module and mocks physical brightness
reads/writes/destruction, WMI and the worker. Enumeration is never invoked. It
checks shared handle leases (including valid handle zero), queued ownership,
nonblocking cleanup during enumeration, rejected invalid readings and scaling
across the full 32-bit brightness range.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DUNICODE -D_UNICODE tests/test_monitor.c brightness.c -ldxva2 -luser32 -lgdi32 -ladvapi32 -o build/test_monitor.exe
./build/test_monitor.exe
```

```bat
cl /nologo /std:c11 /W4 /DUNICODE /D_UNICODE tests\test_monitor.c brightness.c /Fe:build\test_monitor.exe /Fo"build\\" /link dxva2.lib user32.lib gdi32.lib advapi32.lib
build\test_monitor.exe
```

`test_ui.c` includes `ui_popup.c` and `ui_graphics.c` with instrumented Windows
APIs, renders the real popup into offscreen DIBs and simulates mouse
messages. It verifies GDI resource reuse and cleanup, pixel stability, resize
and allocation errors, the final drag value, capture loss, cancellation and
master target bookkeeping. Window and monitor operations are mocked;
no window is shown and no monitor or registry setting is changed.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DUNICODE -D_UNICODE tests/test_ui.c brightness.c -o build/test_ui.exe -lgdi32 -luser32 -lshell32 -ldwmapi
./build/test_ui.exe
```

The existing `test_schedule.c` checks interpolation, midnight wrapping and
schedule suspension/resumption without accessing the system clock or settings.

```powershell
clang -std=c11 -Wall -Wextra -Werror test_schedule.c schedule.c -o build/test_schedule.exe
./build/test_schedule.exe
```

`test_brightness.c` exercises the shared pure calculations and identity matching:
nonzero offsets, rounding, full DWORD ranges, delta compensation, master slider
endpoints, renamed/reordered displays and ambiguous monitor identities.

```powershell
clang -std=c11 -Wall -Wextra -Werror -DUNICODE -D_UNICODE tests/test_brightness.c brightness.c -o build/test_brightness.exe
./build/test_brightness.exe
```
