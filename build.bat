@echo off
setlocal

:: Lumos build script (MSVC)
:: Usage: build.bat          (release, no logging)
::        build.bat debug    (debug, logging to %APPDATA%\Lumos\lumos-*.log)

if not exist build mkdir build

set DEFS=/D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0A00
if /i "%1"=="debug" (
    echo Building Lumos [DEBUG]...
    set DEFS=%DEFS% /DDEBUG
) else (
    echo Building Lumos [RELEASE]...
)

:: Compile resource
rc /nologo /fo build\lumos.res lumos.rc
if errorlevel 1 (
    echo Resource compilation failed.
    exit /b 1
)

:: Compile and link (all intermediates and the exe go to build\)
cl /nologo /O2 /W4 /WX- %DEFS% ^
   lumos.c monitor.c ui.c ui_draw.c ui_popup.c ui_osd.c ui_menu.c ^
   ui_sched.c ui_settings.c ui_about.c presets.c schedule.c wmibright.c capture.c ^
   build\lumos.res ^
   /Fo"build\\" /Fe:build\lumos.exe ^
   /link /subsystem:windows ^
   dxva2.lib user32.lib gdi32.lib shell32.lib ^
   comctl32.lib advapi32.lib ole32.lib oleaut32.lib wbemuuid.lib ^
   dwmapi.lib wtsapi32.lib kernel32.lib

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo Build successful: build\lumos.exe
del /q build\*.obj 2>nul

endlocal
