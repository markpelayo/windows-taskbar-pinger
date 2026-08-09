@echo off
REM build.bat - compiles Pinger.exe with MSVC, no CMake required.
REM
REM Run this from a "Developer Command Prompt for VS 2022" (or x64 Native Tools
REM Command Prompt), which puts cl.exe, rc.exe and link.exe on PATH.
REM
REM   build.bat           release build into build\
REM   build.bat debug     debug build with symbols

setlocal enabledelayedexpansion

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo.
    echo   cl.exe was not found on PATH.
    echo.
    echo   Open "x64 Native Tools Command Prompt for VS 2022" from the Start
    echo   menu and run this script from there. If Visual Studio is not
    echo   installed, get the free Build Tools:
    echo   https://visualstudio.microsoft.com/downloads/
    echo.
    exit /b 1
)

set CONFIG=release
if /i "%~1"=="debug" set CONFIG=debug

set OUTDIR=build
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo Compiling resources...
rc.exe /nologo /fo "%OUTDIR%\resources.res" /i src src\resources.rc
if errorlevel 1 goto :failed

REM /O1 favours size over speed, the right trade for a widget that spends its
REM life asleep. It already implies /Os and /Gy, so neither is listed.
REM
REM /Gw gives each global and static its own COMDAT, which is what lets the
REM /OPT:REF below actually discard unreferenced *data* -- without it the linker
REM can only drop unused functions, and this project is mostly constant tables.
REM /Zc:inline drops unreferenced COMDATs the compiler would otherwise emit.
REM
REM /GS stays on deliberately. This app parses a user-editable file and formats
REM network-derived data into fixed wchar_t buffers, which is precisely the
REM pattern the stack cookie protects; a couple of KB is not worth removing it.
if "%CONFIG%"=="release" (
    set CFLAGS=/O1 /Gw /Zc:inline /GL /DNDEBUG /MT
    set LFLAGS=/LTCG /OPT:REF /OPT:ICF
) else (
    set CFLAGS=/Od /Zi /MTd
    set LFLAGS=/DEBUG
)

echo Compiling and linking (%CONFIG%)...

REM /utf-8 matters: the menus contain typographic characters (·, …, curly
REM quotes, an em dash) and without it MSVC reads these sources as the system
REM code page and mangles them.
cl.exe /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 ^
    /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    !CFLAGS! ^
    /I src ^
    /Fe:"%OUTDIR%\Pinger.exe" ^
    /Fo:"%OUTDIR%\\" ^
    /Fd:"%OUTDIR%\Pinger.pdb" ^
    src\main.cpp src\app.cpp src\monitor.cpp src\grid.cpp ^
    src\ping.cpp src\taskbar.cpp src\settings.cpp src\dialogs.cpp ^
    /link !LFLAGS! ^
    /SUBSYSTEM:WINDOWS ^
    /ENTRY:wWinMainCRTStartup ^
    /MANIFEST:EMBED ^
    /MANIFESTINPUT:src\app.manifest ^
    "%OUTDIR%\resources.res" ^
    iphlpapi.lib ws2_32.lib shell32.lib ole32.lib uuid.lib comdlg32.lib ^
    gdi32.lib user32.lib advapi32.lib

if errorlevel 1 goto :failed

echo.
echo   Built %OUTDIR%\Pinger.exe
echo   Run it with:  %OUTDIR%\Pinger.exe
echo.
exit /b 0

:failed
echo.
echo   Build failed.
echo.
exit /b 1
