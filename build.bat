@echo off
setlocal
REM =====================================================================
REM  Configure + build the NovaDSP firmware for the Pico 2 (RP2350).
REM
REM    build.bat              - dev build, incremental        -> build\
REM    build.bat dist         - dist build, incremental       -> build-dist\
REM    build.bat serial       - dist + serial display         -> build-serial\
REM    build.bat clean        - wipe build\, then dev
REM    build.bat dist clean   - wipe build-dist\, then dist
REM
REM  dev and dist differ only in telemetry. dev has the USB console, the boot
REM  banner, the once-a-second status line and the boot benchmark; dist has
REM  none of them and comes up two seconds sooner. The audio path, the panel
REM  and the knob are identical, and DSP STATUS is live in both. See
REM  nova_build.h.
REM
REM  `serial` is not a third telemetry level - it is dist plus one module
REM  option, -DNOVA_DISPLAY=serial, which puts the whole menu on the USB port
REM  for a box with no panel fitted. It is here as a shortcut because that one
REM  combination is the only place the two axes are not independent: the serial
REM  display refuses to build in a dev image, since it would be drawing on the
REM  port the console prints to.
REM
REM  **For any other combination, use make_firmware.bat.** This script covers
REM  the handful of configurations that get built every day; that one takes
REM  every module as a token - `make_firmware.bat dist oled output:i2s agc` - and is
REM  the only safe way to reuse one build directory across configurations,
REM  because NOVA_EFFECT_<NAME> is a cached option and an effect you stop
REM  naming otherwise stays switched on. cmake directly and build-variants.bat
REM  both still work.
REM
REM  They live in separate directories so switching between them is not a
REM  full rebuild and all three .uf2 files exist at once.
REM =====================================================================

cd /d "%~dp0"
call "%~dp0pico-env.bat"
if errorlevel 1 exit /b 1

set "MODE=dev"
set "BUILD_DIR=build"
set "DIST_FLAG=OFF"
set "EXTRA="
set "DO_CLEAN="

:parse
if /i "%~1"=="dist" (
    set "MODE=dist"
    set "BUILD_DIR=build-dist"
    set "DIST_FLAG=ON"
    shift
    goto parse
)
if /i "%~1"=="serial" (
    set "MODE=serial"
    set "BUILD_DIR=build-serial"
    set "DIST_FLAG=ON"
    set "EXTRA=-DNOVA_DISPLAY=serial"
    shift
    goto parse
)
if /i "%~1"=="clean" (
    set "DO_CLEAN=1"
    shift
    goto parse
)

if defined DO_CLEAN (
    echo [build] Removing %BUILD_DIR%\...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

if not exist "%BUILD_DIR%\build.ninja" (
    echo [build] Configuring CMake ^(%MODE%, board=%PICO_BOARD%, type=%BUILD_TYPE%^)...
    "%CMAKE_EXE%" -S . -B "%BUILD_DIR%" -G Ninja ^
        -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
        -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
        -DPICO_BOARD=%PICO_BOARD% ^
        -DPICO_SDK_PATH="%PICO_SDK_PATH%" ^
        %PICO_EXTRA_CMAKE% ^
        -DNOVA_DIST=%DIST_FLAG% %EXTRA%
    if errorlevel 1 (
        echo [build] ERROR: CMake configure failed.
        exit /b 1
    )
)

echo [build] Building %MODE%...
"%CMAKE_EXE%" --build "%BUILD_DIR%"
if errorlevel 1 (
    echo [build] ERROR: Build failed.
    exit /b 1
)

echo.
echo [build] OK ^(%MODE%^) -^> %~dp0%BUILD_DIR%\%PROJECT_NAME%.uf2
endlocal
