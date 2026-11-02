cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Raspberry Pi Pico 2 -> RP2350, Cortex-M33
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(NovaDSP C CXX ASM)

pico_sdk_init()

include(nova_modules.cmake)

# ---- the core -----------------------------------------------------------
#
# Everything here is true of every build: take a block from the ADC, hand it to
# whichever effect is active, transform it for the spectrum, hand it to the
# output. It names no module, and nothing below is conditional on which modules
# are selected.
#
#   NovaDSP.c    main loop, metering, the banner
#   audio.c      PIO I2S in and out, the DMA ring
#   effects.c    the registry, the core 0 / core 1 command ring, the timing
#   spectrum.c   the 512-point real FFT and the band folding
#   settings.c   parameters and effect selection, persisted to flash
#   display.c    core 1 and the two seqlocks that feed it
#   modules.c    walks the generated module list, and names nothing
add_executable(NovaDSP
    NovaDSP.c
    fault.c
    audio.c
    effects.c
    spectrum.c
    settings.c
    display.c
    modules.c
)

# Core headers - effect.h, module.h, audio.h and the rest - are reachable from
# anywhere. Module directories deliberately are not: a module's own headers are
# found relative to the file including them, so a module cannot pick up another
# module's header by accident. See nova_modules.cmake.
target_include_directories(NovaDSP PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# ---- the modules --------------------------------------------------------
#
# Discovered by globbing */module.cmake. Nothing here lists a module by name,
# so adding one is adding a directory.
#
#   -DNOVA_EFFECT_<NAME>=OFF    leave an effect out (see the STATUS lines below
#                               for the full set and their defaults)
#   -DNOVA_DISPLAY=<name>       oled | none
#   -DNOVA_OUTPUT=<name>        i2s  | none
#   -DNOVA_INPUT=<name>         i2s  | spdif
#
# A configure prints which modules it found and which it selected. If a module
# directory is not listed, it has no module.cmake.
nova_discover_modules(${CMAKE_CURRENT_LIST_DIR})
nova_select_modules(NovaDSP NOVA_EFFECTS NOVA_DISPLAY_SEL NOVA_OUTPUT_SEL NOVA_INPUT_SEL)

foreach(m ${NOVA_EFFECTS})
    nova_apply_module(NovaDSP EFFECT ${m})
endforeach()
nova_apply_module(NovaDSP DISPLAY ${NOVA_DISPLAY_SEL})
nova_apply_module(NovaDSP OUTPUT  ${NOVA_OUTPUT_SEL})
nova_apply_module(NovaDSP INPUT   ${NOVA_INPUT_SEL})

nova_generate_manifest(
    ${CMAKE_CURRENT_BINARY_DIR}/generated/nova_modules_generated.h
    "${NOVA_EFFECTS}" ${NOVA_DISPLAY_SEL} ${NOVA_OUTPUT_SEL} ${NOVA_INPUT_SEL})

message(STATUS "NovaDSP: effects  ${NOVA_EFFECTS}")
message(STATUS "NovaDSP: display  ${NOVA_DISPLAY_SEL}")
message(STATUS "NovaDSP: output   ${NOVA_OUTPUT_SEL}")
message(STATUS "NovaDSP: input    ${NOVA_INPUT_SEL}")

# ---- the settings store's identity --------------------------------------
#
# There is no VERSION to bump any more. The store is keyed by a hash of the
# registered layout - every effect's name in registration order, its parameter
# count, and each parameter's name, kind and range - computed at boot in
# modules.c. It changes exactly when a stored record would stop meaning what it
# meant, and never otherwise, so an unrelated rebuild keeps your tuning.
#
# What a hash of the layout cannot see is a parameter whose *meaning* changed
# while its name, kind and range did not. Bump this for that case, and for that
# case only:
#
#   cmake -B build -DNOVA_SETTINGS_SALT=1
set(NOVA_SETTINGS_SALT 0 CACHE STRING
    "Bump to invalidate every stored settings record; see settings.c")
target_compile_definitions(NovaDSP PRIVATE NOVA_SETTINGS_SALT=${NOVA_SETTINGS_SALT})

# ---- the two builds -----------------------------------------------------
#
#   cmake -B build                       dev  - everything
#   cmake -B build-dist -DNOVA_DIST=ON   dist - no console, no benchmark
#
# The difference is telemetry only; the audio path, the display module and the
# effects are identical. See nova_build.h for what dist removes and, more
# importantly, what it deliberately does not: USB stays enumerated, because
# that is what `picotool load -f` needs to reboot the board without BOOTSEL.
option(NOVA_DIST "Build without console output, telemetry or the benchmark" OFF)

if(NOVA_DIST)
    target_compile_definitions(NovaDSP PRIVATE NOVA_DIST)
    message(STATUS "NovaDSP: dist build - no console output or benchmark")
else()
    message(STATUS "NovaDSP: dev build - console, telemetry and benchmark on")
endif()

# The I2S state machine is core: it carries both directions and generates the
# clocks the ADC needs, whichever output module is selected. A module's own
# .pio files are declared in its module.cmake.
pico_generate_pio_header(NovaDSP ${CMAKE_CURRENT_LIST_DIR}/i2s.pio)

pico_set_program_name(NovaDSP "NovaDSP")
pico_set_program_version(NovaDSP "0.2")

# Execute from SRAM instead of XIP flash. The image still lives in flash and is
# copied to RAM by the startup code, so it survives power cycles - but no code
# fetch touches flash at runtime, which removes XIP cache misses from the DSP
# path. Costs SRAM equal to the image size, so leaving a heavy module out is
# worth real bytes.
pico_set_binary_type(NovaDSP copy_to_ram)

# stdio over USB CDC, not UART
pico_enable_stdio_usb(NovaDSP 1)
pico_enable_stdio_uart(NovaDSP 0)

# GCC's Release config is already -O3 -DNDEBUG; pin it explicitly so the DSP
# code can never be silently built at a lower optimisation level.
#
# -fno-math-errno lets GCC emit the FPU's VSQRT.F32 inline instead of calling
# sqrtf and testing errno afterwards; it changes no result, only errno. The C++
# flags keep the C++ runtime out of the image - no effect throws or uses RTTI,
# and their static objects do not need thread-safe guards.
target_compile_options(NovaDSP PRIVATE
    $<$<CONFIG:Release>:-O3>

    # **Pinned, because a claim the build cannot make is not a claim.** These
    # were the standard for this tree and were checked by hand, which held
    # right up until it did not: a -Wunused-function introduced while fixing
    # the OLED frame bugs compiled clean and silent, and was only found by
    # forcing the flags into a throwaway build directory.
    #
    # -Wdouble-promotion is the one that earns its place on an M33. The FPU is
    # single precision and double is software-emulated, so a stray `2.0` or a
    # `sin()` in the DSP path is a missed deadline rather than a style point.
    # PRIVATE, so only this target's sources are held to it - the SDK is not.
    -Wall
    -Wextra
    -Wdouble-promotion

    # **Off deliberately, and it is part of -Wextra rather than a thing we
    # asked for.** Every effect module builds its `effect_t` with designated
    # initialisers and omits `flags` and `slot_effect`, which the seven that
    # declare no capability bits and resolve no chain slot have no business
    # writing out. C++ value-initialises what an aggregate initialiser omits,
    # so those members are zero - which is the correct default for both.
    #
    # The reason it is off rather than silenced by adding `= 0` in seven files
    # is the treadmill: `effect_t` has grown twice, and each growth would fire
    # this warning once per module, on code that is already correct. A warning
    # that goes off across the whole tree whenever a struct gains a field is
    # one people learn to scroll past - and the list it is buried in is the
    # one -Wdouble-promotion has to be visible in.
    -Wno-missing-field-initializers

    -fno-math-errno
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
)

# The core's own dependencies. A module needing more says so in its LIBS.
target_link_libraries(NovaDSP
    pico_stdlib
    hardware_vreg
    hardware_pio
    hardware_dma
    hardware_clocks
    pico_multicore      # display modules run on core 1
    hardware_flash      # settings storage
    pico_flash          # flash_safe_execute: locks core 1 out of the write
)

# .uf2 / .hex / .bin / .dis / .elf.map
pico_add_extra_outputs(NovaDSP)
