@echo off
setlocal enabledelayedexpansion
REM =====================================================================
REM  make_firmware.bat - build any combination of modules, into one
REM  build directory.
REM
REM      make_firmware.bat dist oled i2s bbe
REM      make_firmware.bat oled agc
REM      make_firmware.bat dist oled thru eq agc fxchain flash
REM      make_firmware.bat list
REM
REM  build.bat covers the two or three combinations that get built every
REM  day. This covers the rest: every effect is a token, the display is a
REM  token, the output is a token, dev/dist is a token. Order does not
REM  matter and nothing is positional.
REM
REM  ---- how the tokens are read ----------------------------------------
REM
REM  The module names are not written down in this file. They are read out
REM  of the */module.cmake manifests at run time - the same set CMake globs
REM  - so a new module directory is a new token with no edit here, which is
REM  the same rule the rest of the build follows.
REM
REM    * naming NO effect     -> every effect whose manifest says DEFAULT ON
REM    * naming ANY effect    -> exactly those, and every other one OFF
REM    * display defaults to oled, output defaults to i2s
REM    * dev is the default; dist removes the telemetry
REM
REM  The second rule is the one that matters, and it is why this is a
REM  script rather than a documented cmake line. NOVA_EFFECT_<NAME> is a
REM  cached option, so an effect you simply stop naming stays ON in a build
REM  directory that already has it - you would get a firmware containing an
REM  effect you did not ask for and no line of output would say so. This
REM  script always passes an explicit ON or OFF for every effect in the
REM  tree, which is what makes reusing one build directory across
REM  configurations correct rather than merely convenient.
REM
REM  ---- the build directory --------------------------------------------
REM
REM  build\ unless out:<dir> says otherwise, so `flash.bat nobuild` flashes
REM  what you just made. The chosen configuration is written to
REM  <dir>\nova-config.txt and compared on the next run: an unchanged
REM  configuration skips the CMake configure and goes straight to ninja.
REM =====================================================================

cd /d "%~dp0"
call "%~dp0pico-env.bat"
if errorlevel 1 exit /b 1

REM ---------------------------------------------------------------------
REM  Discover the modules, the same way nova_discover_modules() does:
REM  every */module.cmake, one directory deep.
REM ---------------------------------------------------------------------
set "EFFECTS_ALL="
set "DISPLAYS_ALL="
set "OUTPUTS_ALL="

for /d %%D in (*) do (
    if exist "%%D\module.cmake" (
        set "_T="
        set "_N="
        set "_DF=ON"
        set "_OR=500"
        set "_DS="
        for /f "usebackq tokens=1,*" %%a in (`findstr /r /c:"^ *TYPE  *" /c:"^ *NAME  *" /c:"^ *DEFAULT  *" /c:"^ *ORDER  *" /c:"^ *DESC  *" "%%D\module.cmake"`) do (
            if /i "%%a"=="TYPE"    set "_T=%%b"
            if /i "%%a"=="NAME"    set "_N=%%b"
            if /i "%%a"=="DEFAULT" set "_DF=%%b"
            if /i "%%a"=="ORDER"   set "_OR=%%b"
            if /i "%%a"=="DESC"    set "_DS=%%~b"
        )
        if not "!_N!"=="" (
            if /i "!_T!"=="EFFECT" (
                set "EFFECTS_ALL=!EFFECTS_ALL! !_N!"
                set "IS_EFFECT_!_N!=1"
                set "ORD_!_N!=!_OR!"
                set "DESC_EFFECT_!_N!=!_DS!"
                if /i "!_DF!"=="ON" set "DEF_EFFECT_!_N!=1"
            )
            if /i "!_T!"=="DISPLAY" (
                set "DISPLAYS_ALL=!DISPLAYS_ALL! !_N!"
                set "IS_DISPLAY_!_N!=1"
                set "DESC_DISPLAY_!_N!=!_DS!"
            )
            if /i "!_T!"=="OUTPUT" (
                set "OUTPUTS_ALL=!OUTPUTS_ALL! !_N!"
                set "IS_OUTPUT_!_N!=1"
                set "DESC_OUTPUT_!_N!=!_DS!"
            )
            if /i "!_T!"=="INPUT" (
                set "INPUTS_ALL=!INPUTS_ALL! !_N!"
                set "IS_INPUT_!_N!=1"
                set "DESC_INPUT_!_N!=!_DS!"
            )
        )
    )
)

REM ---------------------------------------------------------------------
REM  Read the command line.
REM ---------------------------------------------------------------------
set "DIST=OFF"
set "DISPLAY_SEL="
set "OUTPUT_SEL="
set "INPUT_SEL="
set "ANY_EFFECT="
set "BUILD_DIR=build"
set "DO_CLEAN="
set "DO_FLASH="
set "PASSTHRU="

:parse
if "%~1"=="" goto parsed
set "A=%~1"

if /i "!A!"=="help"   goto usage
if /i "!A!"=="-h"     goto usage
if /i "!A!"=="--help" goto usage
if /i "!A!"=="/?"     goto usage
if /i "!A!"=="list"   goto listmods

if /i "!A!"=="dev"    ( set "DIST=OFF"   & shift & goto parse )
if /i "!A!"=="dist"   ( set "DIST=ON"    & shift & goto parse )
if /i "!A!"=="clean"  ( set "DO_CLEAN=1" & shift & goto parse )
if /i "!A!"=="flash"  ( set "DO_FLASH=1" & shift & goto parse )

REM Every effect the tree has a DEFAULT ON for, said explicitly.
if /i "!A!"=="all" (
    for %%e in (!EFFECTS_ALL!) do if defined DEF_EFFECT_%%e set "SEL_%%e=1"
    set "ANY_EFFECT=1"
    shift & goto parse
)

REM Anything starting -D goes to CMake untouched: -DNOVA_SETTINGS_SALT=1 and
REM the like. It lands in the cache, so it also lands in the signature.
REM
REM cmd splits arguments on '=' as well as on spaces, so an unquoted
REM -DNOVA_SETTINGS_SALT=1 arrives here as two arguments. Detect that - the
REM head is the whole token only when there was no '=' in it - and put them
REM back together, so the option works quoted or bare.
if /i "!A:~0,2!"=="-D" (
    set "HEAD="
    for /f "tokens=1 delims==" %%x in ("!A!") do set "HEAD=%%x"
    if "!HEAD!"=="!A!" (
        set "PASSTHRU=!PASSTHRU! !A!=%~2"
        shift
        shift
        goto parse
    )
    set "PASSTHRU=!PASSTHRU! !A!"
    shift
    goto parse
)

if /i "!A:~0,4!"=="out:" ( set "BUILD_DIR=!A:~4!" & shift & goto parse )

REM Qualified forms, needed wherever a bare name would be ambiguous.
if /i "!A:~0,7!"=="effect:" (
    set "V=!A:~7!"
    if not defined IS_EFFECT_!V! goto badeffect
    set "SEL_!V!=1"
    set "ANY_EFFECT=1"
    shift & goto parse
)
if /i "!A:~0,8!"=="display:" (
    set "V=!A:~8!"
    if not defined IS_DISPLAY_!V! goto baddisplay
    set "DISPLAY_SEL=!V!"
    shift & goto parse
)
if /i "!A:~0,7!"=="output:" (
    set "V=!A:~7!"
    if not defined IS_OUTPUT_!V! goto badoutput
    set "OUTPUT_SEL=!V!"
    shift & goto parse
)
REM `input:` and not `in:`, to match `display:` and `output:` - and note that
REM `out:` is already this script's build-directory switch, so the tempting
REM `in:`/`out:` pair would have quietly redefined one of them.
if /i "!A:~0,6!"=="input:" (
    set "V=!A:~6!"
    if not defined IS_INPUT_!V! goto badinput
    set "INPUT_SEL=!V!"
    shift & goto parse
)

REM A bare name. It has to resolve to exactly one module of one type -
REM `none` is a display, an output and an effect, so it never does.
set /a NM=0
if defined IS_EFFECT_!A!  set /a NM+=1
if defined IS_DISPLAY_!A! set /a NM+=1
if defined IS_OUTPUT_!A!  set /a NM+=1
if defined IS_INPUT_!A!   set /a NM+=1

if !NM! EQU 0 goto unknown
if !NM! GTR 1 goto ambiguous

if defined IS_EFFECT_!A!  ( set "SEL_!A!=1" & set "ANY_EFFECT=1" )
if defined IS_DISPLAY_!A! set "DISPLAY_SEL=!A!"
if defined IS_OUTPUT_!A!  set "OUTPUT_SEL=!A!"
if defined IS_INPUT_!A!   set "INPUT_SEL=!A!"
shift
goto parse

:parsed

if not defined DISPLAY_SEL set "DISPLAY_SEL=oled"
if not defined OUTPUT_SEL  set "OUTPUT_SEL=i2s"
if not defined INPUT_SEL   set "INPUT_SEL=i2s"

REM No effect named at all means the manifests' own defaults, which is what
REM a plain `cmake -B build` gives you.
if not defined ANY_EFFECT (
    for %%e in (!EFFECTS_ALL!) do if defined DEF_EFFECT_%%e set "SEL_%%e=1"
)

REM One combination of the two axes is not independent. The module says so
REM itself with an #error; saying it here as well costs a wasted compile.
if /i "!DISPLAY_SEL!"=="serial" if /i "!DIST!"=="OFF" (
    echo [make] ERROR: display 'serial' needs a dist build - it draws on the
    echo [make]        port the dev console is already printing to.
    echo [make]        Add `dist` to the command line.
    exit /b 1
)

REM ---------------------------------------------------------------------
REM  Turn the selection into CMake arguments. Exhaustive by construction:
REM  every effect in the tree gets an explicit ON or OFF, so a stale cache
REM  in a reused build directory cannot leave one switched on behind you.
REM ---------------------------------------------------------------------
set "EFFECT_ARGS="
set "EFFECT_ON="
set "EFFECT_OFF="
for %%e in (!EFFECTS_ALL!) do (
    set "U=%%e"
    for %%c in ("a=A" "b=B" "c=C" "d=D" "e=E" "f=F" "g=G" "h=H" "i=I" "j=J" "k=K" "l=L" "m=M" "n=N" "o=O" "p=P" "q=Q" "r=R" "s=S" "t=T" "u=U" "v=V" "w=W" "x=X" "y=Y" "z=Z") do set "U=!U:%%~c!"
    if defined SEL_%%e (
        set "EFFECT_ARGS=!EFFECT_ARGS! -DNOVA_EFFECT_!U!=ON"
        set "EFFECT_ON=!EFFECT_ON! %%e"
    ) else (
        set "EFFECT_ARGS=!EFFECT_ARGS! -DNOVA_EFFECT_!U!=OFF"
        set "EFFECT_OFF=!EFFECT_OFF! %%e"
    )
)

set "CFG_ARGS=-DNOVA_DIST=!DIST! -DNOVA_DISPLAY=!DISPLAY_SEL! -DNOVA_OUTPUT=!OUTPUT_SEL! -DNOVA_INPUT=!INPUT_SEL!!EFFECT_ARGS!!PASSTHRU!"

if /i "!DIST!"=="ON" ( set "MODE=dist" ) else ( set "MODE=dev" )

echo.
echo [make] build    !MODE!  -^>  !BUILD_DIR!\
echo [make] display  !DISPLAY_SEL!
echo [make] input    !INPUT_SEL!
echo [make] output   !OUTPUT_SEL!
echo [make] effects !EFFECT_ON!
if not "!EFFECT_OFF!"=="" echo [make] omitted !EFFECT_OFF!
if not "!PASSTHRU!"=="" echo [make] extra   !PASSTHRU!
echo.

if defined DO_CLEAN (
    echo [make] Removing !BUILD_DIR!\...
    if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
)

REM ---------------------------------------------------------------------
REM  Configure, unless this build directory is already configured for
REM  exactly this selection. The signature is the argument list itself, so
REM  it cannot describe a configuration other than the one it caused.
REM ---------------------------------------------------------------------
set "SIGFILE=!BUILD_DIR!\nova-config.txt"
set "NEED_CONFIG=1"
if exist "!BUILD_DIR!\build.ninja" if exist "!SIGFILE!" (
    set "OLDSIG="
    set /p OLDSIG=<"!SIGFILE!"
    if "!OLDSIG!"=="!CFG_ARGS!" set "NEED_CONFIG="
)

if defined NEED_CONFIG (
    echo [make] Configuring CMake, board=%PICO_BOARD%, type=%BUILD_TYPE%...
    "%CMAKE_EXE%" -S . -B "!BUILD_DIR!" -G Ninja ^
        -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
        -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
        -DPICO_BOARD=%PICO_BOARD% ^
        -DPICO_SDK_PATH="%PICO_SDK_PATH%" ^
        %PICO_EXTRA_CMAKE% ^
        !CFG_ARGS!
    if errorlevel 1 (
        echo [make] ERROR: CMake configure failed.
        exit /b 1
    )
    >"!SIGFILE!" echo !CFG_ARGS!
) else (
    echo [make] Configuration unchanged - skipping the CMake configure.
)

echo [make] Building...
"%CMAKE_EXE%" --build "!BUILD_DIR!"
if errorlevel 1 (
    echo [make] ERROR: Build failed.
    exit /b 1
)

echo.
echo [make] OK -^> %~dp0!BUILD_DIR!\%PROJECT_NAME%.uf2
"%SIZE_EXE%" "!BUILD_DIR!\%PROJECT_NAME%.elf" 2>nul

if not defined DO_FLASH (
    echo.
    if /i "!BUILD_DIR!"=="build" (
        echo [make] Flash it with:  flash.bat nobuild
    ) else (
        echo [make] Flash it with:  flash.bat dir:!BUILD_DIR!
    )
    goto :eof
)

echo.
REM  The exit code is not enough - see the same note in flash.bat. picotool
REM  can print "The device was asked to reboot into BOOTSEL mode", wait, give
REM  up, and still return 0. This script then reports a build as flashed that
REM  never left the host, so the board keeps running the *previous* firmware
REM  while every measurement is attributed to the new one. That is exactly how
REM  a decoder change appeared to make a marginal build livelock.
set "PICOLOG=%TEMP%\nova-picotool.log"

echo [make] Loading %PROJECT_NAME%.uf2 ...
"%PICOTOOL_EXE%" load -f -x "!BUILD_DIR!\%PROJECT_NAME%.uf2" > "%PICOLOG%" 2>&1
set "PICORC=!ERRORLEVEL!"
type "%PICOLOG%"

if not "!PICORC!"=="0" (
    echo.
    echo [make] ERROR: picotool could not load the firmware.
    echo [make] Hold the BOOTSEL button while plugging the board in, then retry.
    exit /b 1
)

findstr /c:"Loading into Flash" "%PICOLOG%" >nul
if errorlevel 1 (
    echo.
    echo [make] ERROR: picotool exited 0 but never wrote flash - the board did
    echo [make]        not reboot into BOOTSEL. **It is still running whatever
    echo [make]        it was running before**, so do not trust any measurement
    echo [make]        taken after this point.
    echo [make] Hold the BOOTSEL button while plugging the board in, then retry.
    exit /b 1
)

echo [make] Done. The board is running the !MODE! build.
goto :eof


REM ---------------------------------------------------------------------
:listmods
echo.
echo   Effects - name any number of them. Naming none gives the defaults.
for %%e in (!EFFECTS_ALL!) do (
    set "L=    %%e               "
    set "L=!L:~0,18!"
    if defined DEF_EFFECT_%%e ( set "D=default on " ) else ( set "D=default off" )
    echo !L! order !ORD_%%e!  !D!  !DESC_EFFECT_%%e!
)
echo.
echo   Displays - name at most one. Defaults to oled.
for %%d in (!DISPLAYS_ALL!) do (
    set "L=    %%d               "
    set "L=!L:~0,18!"
    echo !L! !DESC_DISPLAY_%%d!
)
echo.
echo   Inputs - name at most one, as input:^<name^>. Defaults to i2s.
for %%i in (!INPUTS_ALL!) do (
    set "L=    %%i               "
    set "L=!L:~0,18!"
    echo !L! !DESC_INPUT_%%i!
)
echo.
echo   Outputs - name at most one. Defaults to i2s.
for %%o in (!OUTPUTS_ALL!) do (
    set "L=    %%o               "
    set "L=!L:~0,18!"
    echo !L! !DESC_OUTPUT_%%o!
)
echo.
echo   Several names belong to more than one kind of module - 'none' is a
echo   display, an output and an effect, and 'i2s' is both an input and an
echo   output. Write which one you mean: display:none, output:none,
echo   effect:none, input:i2s, output:i2s.
echo.
exit /b 0

:usage
echo.
echo   make_firmware.bat [dev^|dist] [display] [output] [effects...] [options]
echo.
echo   Every token is a module name, dev/dist, or one of the options below.
echo   Order does not matter. Run `make_firmware.bat list` for the modules
echo   this tree actually has.
echo.
echo     dev ^| dist      telemetry. dev is the default: USB console, boot
echo                     banner, status line, benchmark. dist has none.
echo     ^<effect^>        add that effect. Naming any effect means naming
echo                     them all - the ones you leave out are left out.
echo     all             every effect whose manifest says DEFAULT ON.
echo     ^<display^>       the panel. One only. Defaults to oled.
echo     ^<output^>        where the block goes. One only. Defaults to i2s.
echo     effect:^<name^>   qualified forms, for a name that is ambiguous
echo     display:^<name^>  because more than one kind of module has it.
echo     output:^<name^>
echo     out:^<dir^>       build directory. Defaults to build\.
echo     clean           wipe the build directory first.
echo     flash           flash it afterwards with picotool.
echo     -D^<x^>=^<y^>       passed to CMake untouched, e.g. -DNOVA_SETTINGS_SALT=1
echo     list            print the modules in this tree, then stop.
echo.
echo   Examples:
echo     make_firmware.bat dist oled i2s bbe
echo     make_firmware.bat oled agc
echo     make_firmware.bat dist oled thru eq agc fxchain flash
echo     make_firmware.bat display:none output:none eq out:build-headless
echo.
exit /b 0

:unknown
echo [make] ERROR: '!A!' is not a module, a build mode or an option.
echo [make]        Run `make_firmware.bat list` to see the modules, or
echo [make]        `make_firmware.bat help` for the options.
exit /b 1

:ambiguous
echo [make] ERROR: '!A!' names more than one kind of module. Write which
echo [make]        one you mean: effect:!A!, display:!A!, input:!A! or
echo [make]        output:!A!.
exit /b 1

:badeffect
echo [make] ERROR: '!V!' is not an effect module. Available:!EFFECTS_ALL!
exit /b 1

:baddisplay
echo [make] ERROR: '!V!' is not a display module. Available:!DISPLAYS_ALL!
exit /b 1

:badoutput
echo [make] ERROR: '!V!' is not an output module. Available:!OUTPUTS_ALL!
exit /b 1

:badinput
echo [make] ERROR: '!V!' is not an input module. Available:!INPUTS_ALL!
exit /b 1
