<div align="center">

# ☀️ Lumos

**One brightness control for every screen: external monitors over DDC/CI and the laptop panel over WMI, from the Windows tray.**

[![Release](https://img.shields.io/github/v/release/sfortis/lumos_ddc?logo=github)](https://github.com/sfortis/lumos_ddc/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/sfortis/lumos_ddc/total?logo=github)](https://github.com/sfortis/lumos_ddc/releases)
![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?logo=windows&logoColor=white)
![Language](https://img.shields.io/badge/C-native%20Windows%20API-A8B9CC?logo=c&logoColor=white)
![Size](https://img.shields.io/badge/size-~170%20KB-success)

</div>

---

## Table of Contents

- [What it does](#what-it-does)
- [Why Lumos?](#why-lumos)
- [Screenshots](#screenshots)
- [Features](#features)
- [How it works](#how-it-works)
- [Install](#install)
- [Usage](#usage)
- [Keyboard and screen readers](#keyboard-and-screen-readers)
- [Command line](#command-line)
- [Configuration](#configuration)
- [Requirements](#requirements)
- [Notes and limitations](#notes-and-limitations)
- [License](#license)

## What it does

Lumos is a small Windows system-tray utility that adjusts the hardware brightness of your monitors. It talks **DDC/CI** to external displays and uses the **WMI backlight** interface for internal laptop panels, so a single slider or hotkey dims everything at once: desktop monitors, a laptop screen, or a mixed setup.

It is written against the native Windows API and GDI, with no runtime and no installer. The app is one 64-bit `.exe` of about 170 KB, and an optional companion, `lumosctl.exe`, controls it from the command line.

## Why Lumos?

Windows can dim a laptop panel, but it will not touch the brightness of external monitors, and third-party tools are often heavy or cluttered.

| | Windows built-in | Lumos |
|---|:---:|:---:|
| Dim internal laptop panel | Yes | Yes |
| Dim external monitors (DDC/CI) | No | Yes |
| One control for all screens at once | No | Yes |
| Global hotkeys | No | Yes, configurable |
| Mouse wheel over the tray icon | No | Yes |
| Per-monitor brightness range | No | Yes |
| Time-of-day brightness schedule | No | Yes |
| Dim when idle | No | Yes |
| Command line control | No | Yes |
| Footprint | n/a | One ~170 KB exe, no dependencies |

## Screenshots

<p align="center">
  <img src="screenshots/01-popup.png" width="320" alt="Brightness popup: a slider and a maximum for each monitor, and the All Monitors slider" />
  &nbsp;&nbsp;
  <img src="screenshots/02-context-menu.png" width="242" alt="Dark context menu: presets, re-scan, Settings, autostart, schedule, idle dim, About" />
</p>
<p align="center">
  <img src="screenshots/03-osd.png" width="248" alt="On-screen display overlay with percentage and progress bar" />
</p>

## Features

- **Dual backend** - External monitors are driven over DDC/CI (dxva2) and internal laptop panels over WMI, in the same interface.
- **Tray popup** - A dark popup with a slider for each monitor and an "All Monitors" slider that moves them together.
- **Per-monitor range** - Each monitor has a minimum and a maximum: its level when All Monitors is at 0% and at 100%. All Monitors moves every monitor linearly across its own range, so two monitors matched at both ends stay matched in between, and every step moves every monitor. The maximum is set with the `-` / `+` under the monitor in the popup and the minimum in Settings. Offsets from older versions are converted automatically.
- **Global hotkeys** - `Ctrl+Win+Up` / `Ctrl+Win+Down` change the brightness of all screens, and `Ctrl+Win+B` opens the popup. All three can be changed in Settings. An installation upgraded from 1.1 or older keeps the `Ctrl+Alt+Up` / `Ctrl+Alt+Down` it had.
- **Mouse wheel on the tray icon** - Scrolling over the tray icon moves the brightness up or down by one step.
- **On-screen display** - A hotkey or wheel change shows an overlay with the All Monitors level and a progress bar.
- **Keyboard and screen reader access** - The tray icon, the popup, the menu, Settings and About work entirely from the keyboard, and NVDA and Narrator can read their controls. With NVDA, a hotkey or wheel change is spoken even while another application has the focus. The schedule editor is not covered yet.
- **Brightness schedule** - An optional time-of-day schedule that ramps the brightness smoothly between the points you set, wrapping around midnight. A manual change pauses it until the next point.
- **Idle auto-dim** - Optional. After a period without keyboard or mouse input (default 5 minutes) the brightness drops to a low level (default 5%), and it comes back as soon as you touch the keyboard or the mouse. Lumos does not dim during fullscreen video, presentation mode, or while the microphone or the camera is in use, so a video call is not dimmed whatever application it runs in.
- **Presets** - Night, Day and Presentation, with editable brightness values.
- **Command line** - `lumosctl.exe` sets, raises, lowers and reads the brightness of all monitors or one of them, applies presets, and switches the schedule and idle dim, through the running Lumos.
- **Settings window** - A dark window for the brightness step, the hotkeys, idle dim, the schedule and autostart switches, the minimum of each monitor, and the preset values.
- **Reconnect and restore** - Lumos detects the monitors again after a plug or unplug, an unlock, a display power-on and a wake from sleep, and re-applies your brightness, because displays often reset to full brightness across sleep. An external monitor often wakes a few seconds after the laptop panel and does not answer DDC/CI at first. Lumos then shows it as **Unavailable**, keeps the other screens working, and tries again after 2, 5, 10 and 20 seconds and then every minute until the monitor answers.
- **Autostart** - Optional launch at login.
- **Single instance with handoff** - Starting a newer build takes over from the one that is running.

## How it works

Internal laptop panels do not answer DDC/CI, which is an I2C protocol meant for external displays. Their backlight is driven through the embedded controller and exposed to Windows through WMI. Lumos picks the right backend for each display automatically:

| Backend | Used for | API |
|---|---|---|
| **DDC/CI** | External monitors | `dxva2` (`GetMonitorBrightness` / `SetMonitorBrightness`) |
| **WMI** | Internal laptop panels | `root\WMI` (`WmiMonitorBrightnessMethods::WmiSetBrightness`) |

During detection, Lumos first tries to read each monitor's brightness over DDC/CI. If that fails, it matches the display to its WMI panel by the PnP instance key (never by hardcoded vendor or product IDs) and drives it over WMI instead. Everything above the backend, such as the sliders, hotkeys, schedule and presets, works the same for both.

## Install

1. Download `lumos-vX.Y.Z.exe` from the [Releases](https://github.com/sfortis/lumos_ddc/releases/latest) page, and `lumosctl.exe` too if you want the command line tool.
2. Run it. Lumos lives in the system tray as a small sun icon; there is nothing to install.
3. Optional: right-click the tray icon and enable **Start with Windows**.

### Build from source

Cross-compile from Linux or WSL with MinGW (the outputs go to `build/`):

```bash
mkdir -p build
x86_64-w64-mingw32-windres lumos.rc -O coff -o build/lumos.res
x86_64-w64-mingw32-gcc -O2 -s -Wall -mwindows -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
  lumos.c monitor.c brightmap.c ui.c ui_draw.c ui_popup.c ui_osd.c ui_menu.c ui_sched.c \
  ui_settings.c ui_about.c presets.c schedule.c hotkey.c a11y.c remote.c wmibright.c capture.c \
  build/lumos.res -o build/lumos.exe \
  -ldxva2 -luser32 -lgdi32 -lshell32 -lcomctl32 -ladvapi32 -lole32 -loleaut32 -lwbemuuid -ldwmapi -lwtsapi32 -loleacc -lkernel32 -lm
x86_64-w64-mingw32-gcc -O2 -s -Wall -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
  lumosctl.c cliparse.c -o build/lumosctl.exe -luser32
```

Or with MSVC from a Developer Command Prompt (this also writes to `build/`):

```bat
build.bat            :: release
build.bat debug      :: debug build, logs to %APPDATA%\Lumos\lumos-*.log
```

## Usage

| Action | Result |
|---|---|
| **Left-click** the tray icon | Open the brightness popup |
| **Right-click** the tray icon | Open the menu: presets, re-scan, Settings, autostart, schedule, idle dim, About, exit |
| **Mouse wheel** over the tray icon | Brightness up or down by one step (default 5%) |
| `Ctrl+Win+Up` / `Ctrl+Win+Down` | Brightness up or down on all monitors |
| `Ctrl+Win+B` | Open the brightness popup with keyboard focus |
| Drag a slider in the popup | Set that monitor, or all of them with the All Monitors slider |
| Click `-` / `+` under a monitor | Change that monitor's maximum, its level at All Monitors 100% |

To match two monitors, set All Monitors to 100% and change the maximum of the brighter monitor until both look the same. Then set All Monitors to 0% and, in Settings, raise the minimum of the monitor that is darker at 0% until they match again.

## Keyboard and screen readers

The tray icon, the popup, the menu, Settings and About can be used without a mouse, and they expose their controls to screen readers through Microsoft Active Accessibility. These windows are drawn by Lumos itself rather than built from standard Windows controls, so this support is part of each window and not something Windows supplies on its own. The schedule editor does not have keyboard or screen reader support yet.

To reach the tray icon, press `Win+B` and move to the Lumos icon with the arrow keys. `Enter` or `Space` opens the popup, and `Shift+F10` or the Menu key opens the menu. The `Ctrl+Win+B` hotkey opens the popup directly.

| Window | Keys |
|---|---|
| Popup | `Tab` / `Shift+Tab` move between the sliders and the monitor maximums. The arrow keys change the value by 1 (`Up` and `Right` raise it), `Page Up` / `Page Down` change a slider by 10 and a maximum by 5, and `Home` / `End` jump to the limits. `Esc` closes. |
| Menu | `Up` / `Down` move, `Home` / `End` jump to the first or last item, a letter jumps to the next item that starts with it, `Enter` or `Space` chooses, and `Esc` closes. |
| Settings | `Tab`, `Shift+Tab`, `Up` and `Down` move between rows and the buttons. `Left` / `Right` change a number or flip a switch, and `Space` flips a switch. `Enter` on a hotkey row starts recording a new combination, and anywhere else it saves. `Esc` closes without saving. |
| About | `Enter` opens the project page and `Esc` closes. |

To change a hotkey, focus its row in Settings, press `Enter`, and press the new combination. A hotkey needs `Ctrl`, `Alt` or `Win`. While a row is recording, `Esc` cancels and `Backspace` turns the hotkey off. If another program already uses the combination, Save leaves the window open and the row says "In use by another app". The `Ctrl+Alt+Up` / `Ctrl+Alt+Down` hotkeys of upgraded installations are also the table navigation commands of NVDA and JAWS, so screen reader users should change them.

When a hotkey or the mouse wheel changes the brightness, the on-screen display announces the All Monitors level ("Brightness 45%") as a live region. NVDA reads it even while another application has the focus. Narrator does not, because it ignores announcements from applications in the background; with Narrator, open the popup (`Ctrl+Win+B`) to hear the current level. While the popup is open, the focused slider reports every change.

## Command line

`lumosctl.exe` controls the running Lumos from a terminal, a script or a shortcut. Put it next to `lumos.exe` or anywhere on your `PATH`. It talks to Lumos, so Lumos has to be running.

```
lumosctl --set 40                 All Monitors to 40%, each monitor within its range
lumosctl --up                     one brightness step up (--down for down)
lumosctl --down 10                10% down
lumosctl --get                    print the current levels
lumosctl --list                   list the monitors with their numbers and ranges
lumosctl --set 60 --monitor 2     one monitor only, by number from --list
lumosctl --up -m dell             or by a name, or a unique part of it
lumosctl --preset Night           apply a preset
lumosctl --schedule off           turn the schedule on or off
lumosctl --idle-dim on            turn dim when idle on or off
lumosctl --rescan                 look for monitors again
```

Every command prints its result, which also makes it usable with a screen reader in a terminal. Changes from the command line count as manual changes, so they pause the schedule until its next point, and they do not show the on-screen display.

The exit code is 0 when the command worked, 1 for wrong usage, 2 when Lumos is not running, 3 when the command failed (an unknown preset or monitor, for example), and 4 when Lumos did not answer.

## Configuration

Settings live in an INI file that is created on the first run:

```
%APPDATA%\Lumos\config.ini
```

Almost everything in it can be set from the interface. Settings covers the brightness step, the hotkeys, idle dim, the schedule and autostart switches, the monitor minimums and the preset values. The popup sets the monitor maximums, and the schedule points have their own editor (right-click the tray icon, then Edit Schedule). Preset names are the one thing that has to be edited in the file, because the interface has no text input.

```ini
[Presets]
Night=30
Day=80
Presentation=100

[Settings]
Step=5
Autostart=0
ScheduleEnabled=0
IdleDimEnabled=0
IdleDimPercent=5
IdleDimMinutes=5
HotkeyBrighten=Ctrl+Win+Up
HotkeyDim=Ctrl+Win+Down
HotkeyPopup=Ctrl+Win+B

[Schedule]
07:00=60
12:00=100
19:00=70
23:00=25

[Ranges]
DELL U2414H=0,60
Wide viewing angle & High density FlexView Display 1920x1080=40,100
```

In Settings, a number changes with its `-` and `+` buttons or with the mouse wheel over the row, a switch flips when you click it, and nothing is written until you press Save. Cancel, `Esc` or a click outside the window closes it without saving. The schedule editor has the same buttons and closes the same way. Both windows can be moved by dragging any spot that is not a control.

Hotkeys are stored as text. Modifiers are `Ctrl`, `Alt`, `Shift` and `Win`, and keys are letters, digits, `F1` to `F24`, the arrows, `Home`, `End`, `PageUp`, `PageDown`, `Insert`, `Delete`, `Space`, `Enter`, `Tab`, `Backspace`, `Pause` and the numeric keypad (`Num0` to `Num9`, `NumPlus`, `NumMinus`, `NumMultiply`, `NumDivide`, `NumDecimal`). `None` turns a hotkey off, and a value that cannot be read falls back to the default. A `config.ini` from version 1.1 or older has no hotkey lines, and it keeps the `Ctrl+Alt+Up` / `Ctrl+Alt+Down` brightness hotkeys those versions used.

Each line in `[Ranges]` is a monitor name followed by its minimum and maximum, the levels it takes when All Monitors is at 0% and at 100%. A second monitor with the same name is stored as `Name #2`, so two identical models keep separate ranges. A range is at least 20 points wide, so every brightness step still moves the monitor. A monitor without a line starts at `0,100`. A `config.ini` from an older version has a `[Deltas]` section with one offset per monitor instead. Lumos converts those offsets into ranges that keep the monitors matched the same way (offsets of +10 and -30 become `40,100` and `0,60`), and it leaves `[Deltas]` untouched so an older version still finds its offsets.

`IdleDimEnabled` turns idle dim on and off, and the tray menu toggles the same key. `IdleDimPercent` is the level held while the session is idle (0 to 100). `IdleDimMinutes` is how long there must be no keyboard or mouse input before the dim happens (1 to 1440 minutes).

## Requirements

- Windows 10 or 11 (64-bit).
- For external monitors: a display and a connection that support **DDC/CI**, with DDC/CI enabled in the monitor's own menu. Most monitors support it; some cheap or very old ones do not.
- For laptop panels: a standard WMI-controllable backlight, the same one the Windows brightness slider uses. Most laptops have one.

## Notes and limitations

- A few external monitors report DDC/CI support but respond poorly. If a slider has no effect, check that DDC/CI is enabled in the monitor's menu.
- A monitor that stays **Unavailable** does not answer DDC/CI. Lumos keeps retrying a monitor that worked before, but one that never answered (DDC/CI turned off in its menu, or a dock that blocks it) is not retried until the next plug, unlock or manual re-scan.
- Some KVM switches and docking stations block DDC/CI.
- Idle auto-dim is not a substitute for turning the display off. On an LCD, image retention is temporary and burn-in is not really a risk. On an OLED, a lower brightness slows pixel wear but does not stop it, because the content stays static. Use the Windows power plan to switch the display off for real protection.

## License

No license has been specified for this project. All rights reserved by the author.
