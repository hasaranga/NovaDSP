@echo off
setlocal enabledelayedexpansion
REM =====================================================================
REM  Build (unless told not to) and flash the Pico 2 over USB via picotool.
REM
REM    flash.bat                 - build dev,    then flash and run
REM    flash.bat dist            - build dist,   then flash and run
REM    flash.bat serial          - build serial, then flash and run
REM    flash.bat nobuild         - flash whatever is already in build\
REM    flash.bat dist nobuild    - flash whatever is already in build-dist\
REM    flash.bat serial nobuild  - flash whatever is already in build-serial\
REM    flash.bat dir:<path>      - flash whatever is already in <path>\
REM
REM  `dir:` is what make_firmware.bat's out:<dir> pairs with. It names the
REM  build directory outright and implies nobuild, because a directory this
REM  script did not choose is one it has no arguments to rebuild. A plain
REM  make_firmware.bat writes to build\, so `flash.bat nobuild` already covers
REM  that case and dir: is only for an out:<dir> that went somewhere else.
REM
REM  The board can be plugged in normally: picotool -f asks the running
REM  firmware to reboot into BOOTSEL. Both builds keep USB enumerated for
REM  exactly this reason - dist stops writing to the port, it does not remove
REM  it. If no firmware with USB stdio is on the board yet, hold BOOTSEL while
REM  plugging it in.
REM =====================================================================

cd /d "%~dp0"
call "%~dp0pico-env.bat"
if errorlevel 1 exit /b 1

set "MODE=dev"
set "BUILD_DIR=build"
set "NOBUILD="

:parse
if /i "%~1"=="dist" (
    set "MODE=dist"
    set "BUILD_DIR=build-dist"
    shift
    goto parse
)
if /i "%~1"=="serial" (
    set "MODE=serial"
    set "BUILD_DIR=build-serial"
    shift
    goto parse
)
if /i "%~1"=="nobuild" (
    set "NOBUILD=1"
    shift
    goto parse
)
set "_A=%~1"
if /i "!_A:~0,4!"=="dir:" (
    if "!_A:~4!"=="" (
        echo [flash] ERROR: dir: needs a path, e.g. dir:build-headless
        exit /b 1
    )
    set "BUILD_DIR=!_A:~4!"
    set "MODE=!_A:~4!"
    set "EXPLICIT_DIR=1"
    set "NOBUILD=1"
    shift
    goto parse
)

REM dir: has already said what the directory is, so it has nothing to compose.
if defined EXPLICIT_DIR goto composed
set "BUILD_ARGS=%MODE:dev=%"
:composed

if not defined NOBUILD (
    call "%~dp0build.bat" %BUILD_ARGS%
    if errorlevel 1 exit /b 1
)

set "UF2=%~dp0%BUILD_DIR%\%PROJECT_NAME%.uf2"
if not exist "%UF2%" (
    echo [flash] ERROR: "%UF2%" not found. Build it first - build.bat %MODE%,
    echo [flash]        or make_firmware.bat with the modules you want.
    exit /b 1
)

REM  ---- the exit code is not enough ------------------------------------
REM
REM  **picotool can exit 0 having written nothing.** Asked to load with -f
REM  against a board whose core 0 is saturated, it prints "The device was
REM  asked to reboot into BOOTSEL mode", waits, gives up - and still returns
REM  success. This script then said "Done. The board is running the build-safe
REM  build" for a flash that never transferred a byte, and the next hour was
REM  spent debugging a serial reader that was working correctly all along.
REM
REM  So the output is checked as well as the code. A real load always prints
REM  a "Loading into Flash" progress line; a reboot that never happened does
REM  not. Buffering it to a file costs the live progress bar, which is drawn
REM  with carriage returns and does not survive redirection anyway.
set "PICOLOG=%TEMP%\nova-picotool.log"

echo [flash] Loading %MODE% %PROJECT_NAME%.uf2 ...
"%PICOTOOL_EXE%" load -f -x "%UF2%" > "%PICOLOG%" 2>&1
set "PICORC=%ERRORLEVEL%"
type "%PICOLOG%"

if not "%PICORC%"=="0" (
    echo.
    echo [flash] ERROR: picotool could not load the firmware.
    echo [flash] Hold the BOOTSEL button while plugging the board in, then retry.
    exit /b 1
)

findstr /c:"Loading into Flash" "%PICOLOG%" >nul
if errorlevel 1 (
    echo.
    echo [flash] ERROR: picotool exited 0 but never wrote flash - the board did
    echo [flash]        not reboot into BOOTSEL. **It is still running whatever
    echo [flash]        it was running before**, which is usually a build whose
    echo [flash]        core 0 is too busy to service the reset request.
    echo [flash] Hold the BOOTSEL button while plugging the board in, then retry.
    exit /b 1
)

echo.
echo [flash] Done. The board is running the %MODE% build.
echo [flash] Waiting for USB serial to re-enumerate...
REM Give Windows a couple of seconds to bring the CDC port back up.
ping -n 4 127.0.0.1 >nul
powershell -NoProfile -Command "Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\(COM\d+\)' } | ForEach-Object { '[flash] ' + $_.Name }"
if /i "%MODE%"=="dist" (
    echo [flash] Note: the dist build never writes to the port - it is there so
    echo [flash]       picotool -f can still reboot the board for the next flash.
) else if /i "%MODE%"=="serial" (
    echo [flash] The menu is on the port. Open it with any terminal:
    echo [flash]     putty -serial COMxx -sercfg 115200
    echo [flash] Make the window at least 80x25, and set Window - Translation
    echo [flash] - Remote character set to UTF-8 if the borders look wrong.
    echo [flash] Arrows move, Enter selects, Esc goes back, S saves, R redraws.
) else (
    echo [flash] Open the port with any terminal, e.g.: putty -serial COMxx -sercfg 115200
)
endlocal
