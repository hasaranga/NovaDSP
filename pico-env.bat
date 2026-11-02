@echo off
REM =====================================================================
REM  Shared environment for every build script here. Sourced by build.bat,
REM  make_firmware.bat, flash.bat and build-variants.bat.
REM
REM  It supports two installations and prefers whichever it finds first:
REM
REM    1. A STANDARD Pico SDK install - you cloned pico-sdk yourself and put
REM       the ARM toolchain, CMake, Ninja and picotool on PATH. Set one
REM       environment variable and nothing here needs editing:
REM
REM           setx PICO_SDK_PATH C:\pico\pico-sdk
REM
REM    2. The VS Code "Raspberry Pi Pico" extension, which installs
REM       everything privately under %USERPROFILE%\.pico-sdk and puts none
REM       of it on PATH. Found automatically, newest version of each
REM       component, no version numbers written down anywhere.
REM
REM  Anything already set in the environment always wins, so a one-off
REM  override is a `set` before the call and needs no edit here.
REM
REM  To pin something permanently without modifying a tracked file, create
REM  pico-env.local.bat next to this one and set what you want in it. It is
REM  read first and is in .gitignore.
REM =====================================================================

if exist "%~dp0pico-env.local.bat" call "%~dp0pico-env.local.bat"

REM ---- board / build settings -----------------------------------------
REM  Override either of these in the environment or in pico-env.local.bat.
if not defined PICO_BOARD   set "PICO_BOARD=pico2"
if not defined BUILD_TYPE   set "BUILD_TYPE=Release"
set "PROJECT_NAME=NovaDSP"

REM ---- 1. the SDK ------------------------------------------------------
REM  An already-set PICO_SDK_PATH is taken as given - that is the standard
REM  install, and it is the whole of what a standard install has to provide.
if defined PICO_SDK_PATH goto have_sdk

REM  Otherwise look for the VS Code extension's private copy and take the
REM  newest SDK it has. /o-n sorts descending by name, so the first hit is
REM  the highest version; no version number is written down in this file.
if not defined PICO_INSTALL_ROOT set "PICO_INSTALL_ROOT=%USERPROFILE%\.pico-sdk"

if exist "%PICO_INSTALL_ROOT%\sdk\" (
    for /f "delims=" %%v in ('dir /b /ad /o-n "%PICO_INSTALL_ROOT%\sdk" 2^>nul') do (
        if not defined PICO_SDK_PATH (
            if exist "%PICO_INSTALL_ROOT%\sdk\%%v\pico_sdk_init.cmake" (
                set "PICO_SDK_PATH=%PICO_INSTALL_ROOT%\sdk\%%v"
                set "PICO_SDK_VERSION=%%v"
            )
        )
    )
)

:have_sdk
if not defined PICO_SDK_PATH goto no_sdk
if not exist "%PICO_SDK_PATH%\pico_sdk_init.cmake" goto bad_sdk

REM ---- 2. the toolchain, CMake, Ninja and picotool ---------------------
REM  On a standard install these are on PATH and there is nothing to find.
REM  On the extension install they are not, so the private copies are used
REM  and prepended to PATH for the duration of the build.
REM
REM  PICO_TOOLCHAIN_PATH and picotool_DIR are passed to CMake only when they
REM  were actually found, which is what PICO_EXTRA_CMAKE carries. Passing an
REM  empty -DPICO_TOOLCHAIN_PATH= is worse than not passing it: it overrides
REM  the SDK's own search with nothing.
set "PICO_EXTRA_CMAKE="

if not defined PICO_INSTALL_ROOT set "PICO_INSTALL_ROOT=%USERPROFILE%\.pico-sdk"

if not defined PICO_TOOLCHAIN_PATH call :newest_subdir PICO_TOOLCHAIN_PATH "%PICO_INSTALL_ROOT%\toolchain" "bin\arm-none-eabi-gcc.exe"
if not defined CMAKE_EXE           call :newest_exe    CMAKE_EXE    "%PICO_INSTALL_ROOT%\cmake"   "bin\cmake.exe"
if not defined NINJA_EXE           call :newest_exe    NINJA_EXE    "%PICO_INSTALL_ROOT%\ninja"   "ninja.exe"
if not defined PICOTOOL_EXE        call :find_picotool
if not defined pioasm_DIR          call :newest_subdir pioasm_DIR "%PICO_INSTALL_ROOT%\tools" "pioasm\pioasmConfig.cmake"
if defined pioasm_DIR set "pioasm_DIR=%pioasm_DIR%\pioasm"

REM  Whatever was not found privately has to come from PATH.
if not defined CMAKE_EXE    set "CMAKE_EXE=cmake"
if not defined NINJA_EXE    set "NINJA_EXE=ninja"
if not defined PICOTOOL_EXE set "PICOTOOL_EXE=picotool"

if defined PICO_TOOLCHAIN_PATH (
    set "PATH=%PICO_TOOLCHAIN_PATH%\bin;%PATH%"
    set "PICO_EXTRA_CMAKE=%PICO_EXTRA_CMAKE% -DPICO_TOOLCHAIN_PATH=\"%PICO_TOOLCHAIN_PATH%\""
    set "SIZE_EXE=%PICO_TOOLCHAIN_PATH%\bin\arm-none-eabi-size.exe"
) else (
    set "SIZE_EXE=arm-none-eabi-size"
)
if defined picotool_DIR set "PICO_EXTRA_CMAKE=%PICO_EXTRA_CMAKE% -Dpicotool_DIR=\"%picotool_DIR%\""
REM  pioasm matters as much as picotool and is easier to miss: without it the
REM  SDK builds pioasm from source as a HOST tool, which needs a host C++
REM  compiler on PATH. On a machine that only has arm-none-eabi that fails
REM  with "No CMAKE_CXX_COMPILER could be found" a hundred targets into an
REM  otherwise clean build, and the error names the SDK rather than this.
if defined pioasm_DIR set "PICO_EXTRA_CMAKE=%PICO_EXTRA_CMAKE% -Dpioasm_DIR=\"%pioasm_DIR%\""

REM ---- 3. say what is going to be used ---------------------------------
REM  Printed once per script run. A build that picked up a different SDK
REM  than you meant is otherwise indistinguishable from one that did not.
echo [env] SDK        %PICO_SDK_PATH%
if defined PICO_TOOLCHAIN_PATH (echo [env] toolchain  %PICO_TOOLCHAIN_PATH%) else (echo [env] toolchain  from PATH)
echo [env] board      %PICO_BOARD%  ^(%BUILD_TYPE%^)

exit /b 0

REM ---------------------------------------------------------------------
REM  Helpers. Each takes the variable to set, a parent directory whose
REM  subdirectories are version numbers, and a path within one that must
REM  exist for it to count.
REM ---------------------------------------------------------------------
:newest_subdir
if not exist "%~2\" exit /b 0
for /f "delims=" %%v in ('dir /b /ad /o-n "%~2" 2^>nul') do (
    if not defined %~1 if exist "%~2\%%v\%~3" set "%~1=%~2\%%v"
)
exit /b 0

:newest_exe
if not exist "%~2\" exit /b 0
for /f "delims=" %%v in ('dir /b /ad /o-n "%~2" 2^>nul') do (
    if not defined %~1 if exist "%~2\%%v\%~3" set "%~1=%~2\%%v\%~3"
)
exit /b 0

REM  picotool is the odd one: the extension nests it a directory deeper, as
REM  <root>\picotool\<version>\picotool\picotool.exe, and CMake wants the
REM  containing directory as picotool_DIR while flash.bat wants the exe.
:find_picotool
if not exist "%PICO_INSTALL_ROOT%\picotool\" exit /b 0
for /f "delims=" %%v in ('dir /b /ad /o-n "%PICO_INSTALL_ROOT%\picotool" 2^>nul') do (
    if not defined PICOTOOL_EXE (
        if exist "%PICO_INSTALL_ROOT%\picotool\%%v\picotool\picotool.exe" (
            set "picotool_DIR=%PICO_INSTALL_ROOT%\picotool\%%v\picotool"
            set "PICOTOOL_EXE=%PICO_INSTALL_ROOT%\picotool\%%v\picotool\picotool.exe"
        )
    )
)
exit /b 0

REM ---------------------------------------------------------------------
:no_sdk
echo.
echo [env] ERROR: the Pico SDK was not found.
echo.
echo [env] Either set PICO_SDK_PATH to your pico-sdk checkout:
echo [env]
echo [env]     setx PICO_SDK_PATH C:\pico\pico-sdk
echo [env]
echo [env] and make sure arm-none-eabi-gcc, cmake, ninja and picotool are
echo [env] on PATH - or install the VS Code "Raspberry Pi Pico" extension,
echo [env] which puts all of it under %%USERPROFILE%%\.pico-sdk and is found
echo [env] here automatically.
echo.
echo [env] The SDK must be cloned with submodules:
echo [env]     git clone -b master --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
echo.
exit /b 1

:bad_sdk
echo.
echo [env] ERROR: PICO_SDK_PATH is set to "%PICO_SDK_PATH%"
echo [env]        but there is no pico_sdk_init.cmake there, so it is not
echo [env]        an SDK checkout.
echo.
exit /b 1
