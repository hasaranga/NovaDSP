@echo off
setlocal enabledelayedexpansion
REM =====================================================================
REM  Configure and build several module combinations, to prove the plug
REM  points actually unplug.
REM
REM  This is a *build* test, not a firmware test: it says the tree still
REM  compiles and links with a module missing, and it says what each one
REM  costs in text and bss. It does not flash anything and it does not say
REM  the result sounds right.
REM
REM      build-variants.bat            build every variant below
REM
REM  One of them - serial-display - carries -DNOVA_DIST=ON in its options,
REM  because that module refuses to compile in a dev build: it draws on the
REM  same USB CDC port a dev build's console is already using. Its sizes are
REM  therefore not comparable with the rows either side of it.
REM      build-variants.bat clean      wipe build-var\ first
REM
REM  Everything lands under build-var\<name>\, so none of it disturbs
REM  build\ or build-dist\.
REM =====================================================================

cd /d "%~dp0"
call "%~dp0pico-env.bat"
if errorlevel 1 exit /b 1

if /i "%~1"=="clean" if exist "build-var" rmdir /s /q "build-var"

REM Every log below is redirected into this directory, and a redirection
REM into a directory that does not exist fails before cmake is reached -
REM which reads as every variant failing to configure. cmake would create
REM build-var\<name>\ itself; it does not get the chance.
if not exist "build-var" mkdir "build-var"

call :variant full            ""
call :variant no-display      "-DNOVA_DISPLAY=none"
call :variant serial-display  "-DNOVA_DIST=ON -DNOVA_DISPLAY=serial"
call :variant no-output       "-DNOVA_OUTPUT=none"
call :variant analyser-only   "-DNOVA_OUTPUT=none -DNOVA_EFFECT_BBE=OFF -DNOVA_EFFECT_EQ=OFF -DNOVA_EFFECT_NOISEGATE=OFF -DNOVA_EFFECT_AGC=OFF -DNOVA_EFFECT_FXCHAIN=OFF -DNOVA_EFFECT_THRU=OFF"
call :variant headless-eq     "-DNOVA_DISPLAY=none -DNOVA_EFFECT_BBE=OFF -DNOVA_EFFECT_NOISEGATE=OFF -DNOVA_EFFECT_FXCHAIN=OFF"
call :variant spdif-out       "-DNOVA_OUTPUT=spdif"
call :variant spdif-in        "-DNOVA_INPUT=spdif"
call :variant spdif-both      "-DNOVA_INPUT=spdif -DNOVA_OUTPUT=spdif"

echo.
echo [variants] ==================== sizes ====================
for /d %%D in (build-var\*) do (
    if exist "%%D\NovaDSP.elf" (
        for /f "tokens=1,3" %%a in ('arm-none-eabi-size "%%D\NovaDSP.elf" ^| findstr /v text') do (
            echo   %%~nxD: text %%a  bss %%b
        )
    ) else (
        echo   %%~nxD: FAILED
    )
)
endlocal
goto :eof

:variant
set "NAME=%~1"
set "OPTS=%~2"
echo.
echo [variants] ---- %NAME% %OPTS%
"%CMAKE_EXE%" -S . -B "build-var\%NAME%" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DPICO_BOARD=%PICO_BOARD% ^
    -DPICO_SDK_PATH="%PICO_SDK_PATH%" ^
    %PICO_EXTRA_CMAKE% ^
    %OPTS% > "build-var\%NAME%-configure.log" 2>&1
if errorlevel 1 (
    echo [variants] CONFIGURE FAILED - see build-var\%NAME%-configure.log
    goto :eof
)
findstr /c:"NovaDSP:" "build-var\%NAME%-configure.log"
"%CMAKE_EXE%" --build "build-var\%NAME%" > "build-var\%NAME%-build.log" 2>&1
if errorlevel 1 (
    echo [variants] BUILD FAILED - see build-var\%NAME%-build.log
) else (
    echo [variants] ok
)
goto :eof
