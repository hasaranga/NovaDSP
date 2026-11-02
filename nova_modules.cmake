# =====================================================================
#  nova_modules.cmake - module declaration and discovery.
#
#  A module is a directory containing a `module.cmake` that calls
#  nova_module() exactly once. Nothing else registers it: the top-level
#  CMakeLists globs */module.cmake, includes each one, and generates
#  nova_modules_generated.h naming whichever survived the build options.
#
#  **Adding a module is adding a directory.** No core file is edited, and no
#  core file names any module.
#
#  ---- declaring one --------------------------------------------------
#
#      nova_module(
#          TYPE     EFFECT             # EFFECT | DISPLAY | OUTPUT | INPUT
#          NAME     dee                # build-option name, lower case
#          ACCESSOR dee_module         # the C function the core links against
#          ORDER    10                 # EFFECT only: menu position
#          DEFAULT  ON                 # EFFECT only: compiled in unless -DNOVA_EFFECT_DEE=OFF
#          DESC     "three-band loudness"
#          SOURCES  module.cpp dee/DeeFloat.cpp
#          INCLUDES dee                # rarely needed; see below
#          DEFINES  DEE_REAL_FLOAT
#          LIBS     hardware_i2c
#          PIO      encoder.pio
#      )
#
#  SOURCES, INCLUDES and PIO are relative to the module's own directory.
#
#  ---- include paths, and why modules get none ------------------------
#
#  A module's directory is deliberately **not** added to the include path.
#  Its own headers are reached the way GCC already reaches them - relative to
#  the file doing the including, so `#include "dee/DeeFloat.h"` from
#  dee_module/module.cpp works with no -I at all. The core directory is on the
#  path, so effect.h, module.h and the rest are available everywhere.
#
#  The effect of that is that a module cannot reach another module's headers
#  by accident: it would have to write `../other_module/x.h` and mean it.
#  INCLUDES exists for a module that genuinely needs a flat include path -
#  usually vendored code whose own sources use angle brackets.
#
#  ---- the accessor ---------------------------------------------------
#
#  ACCESSOR names a function with C linkage returning a pointer to the
#  module's descriptor:
#
#      const effect_module_t  *dee_module(void);
#      const display_module_t *oled_display_module(void);
#      const output_module_t  *i2s_output_module(void);
#      const input_module_t   *i2s_input_module(void);
#
#  It must be unique across every module of every type in the build, because
#  every generated list is declared in the same translation unit (modules.c). A
#  clash is a compile error there, not a silent substitution.
# =====================================================================

# Accumulated in global properties rather than parent-scope variables, because
# each module.cmake is include()d and the results have to outlive that scope.
define_property(GLOBAL PROPERTY NOVA_MOD_EFFECT_LIST)
define_property(GLOBAL PROPERTY NOVA_MOD_DISPLAY_LIST)
define_property(GLOBAL PROPERTY NOVA_MOD_OUTPUT_LIST)
define_property(GLOBAL PROPERTY NOVA_MOD_INPUT_LIST)

function(nova_module)
    set(options)
    set(one   TYPE NAME ACCESSOR ORDER DEFAULT DESC)
    set(multi SOURCES INCLUDES DEFINES LIBS PIO VENDORED)
    cmake_parse_arguments(M "${options}" "${one}" "${multi}" ${ARGN})

    if(NOT M_TYPE OR NOT M_NAME OR NOT M_ACCESSOR)
        message(FATAL_ERROR
            "nova_module() in ${NOVA_MODULE_DIR}: TYPE, NAME and ACCESSOR are all required")
    endif()
    if(NOT M_TYPE MATCHES "^(EFFECT|DISPLAY|OUTPUT|INPUT)$")
        message(FATAL_ERROR
            "nova_module() in ${NOVA_MODULE_DIR}: TYPE must be EFFECT, DISPLAY, OUTPUT or INPUT, not '${M_TYPE}'")
    endif()
    if(NOT M_SOURCES)
        message(FATAL_ERROR "nova_module(${M_NAME}): no SOURCES")
    endif()
    if(M_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "nova_module(${M_NAME}): unrecognised argument(s) ${M_UNPARSED_ARGUMENTS}")
    endif()

    # A name is unique per type. Two modules of the same type with the same
    # name would make -DNOVA_DISPLAY=<name> ambiguous, and the second would
    # silently overwrite the first's properties.
    get_property(_seen GLOBAL PROPERTY NOVA_MOD_${M_TYPE}_LIST)
    if("${M_NAME}" IN_LIST _seen)
        message(FATAL_ERROR
            "nova_module: two ${M_TYPE} modules are both named '${M_NAME}'; "
            "the second is ${NOVA_MODULE_DIR}")
    endif()

    # ORDER is the menu position, zero-padded so a plain lexical list(SORT)
    # orders it numerically. Effects only; the other two types are single
    # choice and have nothing to order.
    if(NOT DEFINED M_ORDER)
        set(M_ORDER 500)
    endif()
    string(LENGTH "${M_ORDER}" _len)
    math(EXPR _pad "4 - ${_len}")
    set(_key "${M_ORDER}")
    while(_pad GREATER 0)
        set(_key "0${_key}")
        math(EXPR _pad "${_pad} - 1")
    endwhile()

    if(NOT DEFINED M_DEFAULT)
        set(M_DEFAULT ON)
    endif()
    if(NOT M_DESC)
        set(M_DESC "${M_NAME}")
    endif()

    # Make the paths absolute here, while NOVA_MODULE_DIR is still the module's.
    set(_srcs "")
    foreach(s ${M_SOURCES})
        list(APPEND _srcs "${NOVA_MODULE_DIR}/${s}")
    endforeach()
    set(_incs "")
    foreach(i ${M_INCLUDES})
        list(APPEND _incs "${NOVA_MODULE_DIR}/${i}")
    endforeach()
    set(_pios "")
    foreach(p ${M_PIO})
        list(APPEND _pios "${NOVA_MODULE_DIR}/${p}")
    endforeach()

    # VENDORED names the sources in this module that are somebody else's code.
    # They are still compiled and still linked; what they are excused is the
    # house warning set, which is written for code we can fix. Held to it, a
    # vendored engine offers a choice between diverging from upstream and
    # living with warnings that train the eye to skip the whole list.
    #
    # **It is declared per module, not per file in CMakeLists.txt**, for the
    # same reason nothing else about a module is: a core file naming
    # `dee/DeeFloat.cpp` would be a core file naming a module.
    set(_vend "")
    foreach(v ${M_VENDORED})
        list(APPEND _vend "${NOVA_MODULE_DIR}/${v}")
    endforeach()

    set(_k "NOVA_MOD_${M_TYPE}_${M_NAME}")
    set_property(GLOBAL PROPERTY ${_k}_DIR      "${NOVA_MODULE_DIR}")
    set_property(GLOBAL PROPERTY ${_k}_ACCESSOR "${M_ACCESSOR}")
    set_property(GLOBAL PROPERTY ${_k}_ORDERKEY "${_key}")
    set_property(GLOBAL PROPERTY ${_k}_DEFAULT  "${M_DEFAULT}")
    set_property(GLOBAL PROPERTY ${_k}_DESC     "${M_DESC}")
    set_property(GLOBAL PROPERTY ${_k}_SOURCES  "${_srcs}")
    set_property(GLOBAL PROPERTY ${_k}_INCLUDES "${_incs}")
    set_property(GLOBAL PROPERTY ${_k}_DEFINES  "${M_DEFINES}")
    set_property(GLOBAL PROPERTY ${_k}_LIBS     "${M_LIBS}")
    set_property(GLOBAL PROPERTY ${_k}_PIO      "${_pios}")
    set_property(GLOBAL PROPERTY ${_k}_VENDORED "${_vend}")

    set_property(GLOBAL APPEND PROPERTY NOVA_MOD_${M_TYPE}_LIST "${M_NAME}")
endfunction()


# ---- discovery ------------------------------------------------------
# Every */module.cmake under `root`. A directory with no module.cmake is not a
# module and is not looked at - which is how lcd_display_module/ can sit in the tree
# holding nothing but the contract it is going to implement.
function(nova_discover_modules root)
    file(GLOB _manifests "${root}/*/module.cmake")
    list(SORT _manifests)
    foreach(m ${_manifests})
        get_filename_component(_dir "${m}" DIRECTORY)
        set(NOVA_MODULE_DIR "${_dir}")
        include("${m}")
    endforeach()
endfunction()


# ---- applying one to the target -------------------------------------
function(nova_apply_module target type name)
    set(_k "NOVA_MOD_${type}_${name}")

    get_property(_srcs GLOBAL PROPERTY ${_k}_SOURCES)
    get_property(_incs GLOBAL PROPERTY ${_k}_INCLUDES)
    get_property(_defs GLOBAL PROPERTY ${_k}_DEFINES)
    get_property(_libs GLOBAL PROPERTY ${_k}_LIBS)
    get_property(_pios GLOBAL PROPERTY ${_k}_PIO)
    get_property(_vend GLOBAL PROPERTY ${_k}_VENDORED)

    target_sources(${target} PRIVATE ${_srcs})

    # See VENDORED in nova_module(). -Wdouble-promotion is the one worth naming:
    # on an M33 it is how a software-emulated double is caught in a DSP path, so
    # it stays on for every line this project wrote. A vendored engine that
    # designs coefficients in double on purpose is a different case, and the
    # invariant that actually covers it is a disassembly check on the per-sample
    # function - SESSION.md has DeeFloat's.
    if(_vend)
        set_source_files_properties(${_vend} PROPERTIES COMPILE_OPTIONS
            "-Wno-double-promotion")
    endif()
    if(_incs)
        target_include_directories(${target} PRIVATE ${_incs})
    endif()
    if(_defs)
        target_compile_definitions(${target} PRIVATE ${_defs})
    endif()
    if(_libs)
        target_link_libraries(${target} ${_libs})
    endif()
    foreach(p ${_pios})
        pico_generate_pio_header(${target} "${p}")
    endforeach()
endfunction()


# ---- choosing which ones ---------------------------------------------
#
# Effects are many and independent: one ON/OFF option each, defaulting to the
# module's own DEFAULT.
#
# Display and output are single choice, because there is one panel and one set
# of pins: -DNOVA_DISPLAY=<name>, -DNOVA_OUTPUT=<name>. A name that matches no
# module fails the configure and lists what there was, rather than building
# something that will not link.
function(nova_select_modules target out_effects out_display out_output out_input)
    # --- effects, in ORDER order
    get_property(_all GLOBAL PROPERTY NOVA_MOD_EFFECT_LIST)
    set(_keyed "")
    foreach(n ${_all})
        get_property(_ok GLOBAL PROPERTY NOVA_MOD_EFFECT_${n}_ORDERKEY)
        list(APPEND _keyed "${_ok}:${n}")
    endforeach()
    list(SORT _keyed)

    set(_enabled "")
    foreach(kn ${_keyed})
        string(REGEX REPLACE "^[0-9]+:" "" n "${kn}")
        string(TOUPPER "${n}" N)
        get_property(_def  GLOBAL PROPERTY NOVA_MOD_EFFECT_${n}_DEFAULT)
        get_property(_desc GLOBAL PROPERTY NOVA_MOD_EFFECT_${n}_DESC)
        option(NOVA_EFFECT_${N} "effect module: ${_desc}" ${_def})
        if(NOVA_EFFECT_${N})
            list(APPEND _enabled "${n}")
        endif()
    endforeach()

    # A build with no effects at all is legal - the core passes the block
    # through untouched - but it leaves the menu, the settings store and the
    # boot banner with nothing to name. no_effect_module is the placeholder for
    # exactly that, so pull it in rather than leaving the list empty.
    if(NOT _enabled)
        if("none" IN_LIST _all)
            message(STATUS "NovaDSP: no effect modules selected - enabling 'none'")
            set(_enabled "none")
        else()
            message(WARNING "NovaDSP: no effect modules, and no 'none' module to fall back on")
        endif()
    endif()

    # --- display, output and input, one each
    foreach(pair "DISPLAY:NOVA_DISPLAY:oled" "OUTPUT:NOVA_OUTPUT:i2s" "INPUT:NOVA_INPUT:i2s")
        string(REPLACE ":" ";" _p "${pair}")
        list(GET _p 0 _type)
        list(GET _p 1 _var)
        list(GET _p 2 _fallback)

        get_property(_avail GLOBAL PROPERTY NOVA_MOD_${_type}_LIST)
        if(NOT _avail)
            message(FATAL_ERROR "NovaDSP: no ${_type} module found at all")
        endif()
        if(NOT "${_fallback}" IN_LIST _avail)
            list(GET _avail 0 _fallback)
        endif()

        set(${_var} "${_fallback}" CACHE STRING "${_type} module: one of ${_avail}")
        set_property(CACHE ${_var} PROPERTY STRINGS ${_avail})

        if(NOT "${${_var}}" IN_LIST _avail)
            message(FATAL_ERROR
                "NovaDSP: -D${_var}=${${_var}} matches no ${_type} module. Available: ${_avail}")
        endif()
        set(_chosen_${_type} "${${_var}}")
    endforeach()

    set(${out_effects} "${_enabled}"           PARENT_SCOPE)
    set(${out_display} "${_chosen_DISPLAY}"    PARENT_SCOPE)
    set(${out_output}  "${_chosen_OUTPUT}"     PARENT_SCOPE)
    set(${out_input}   "${_chosen_INPUT}"      PARENT_SCOPE)
endfunction()


# ---- the generated manifest -------------------------------------------
# X-macro lists, so modules.c can declare the externs and build the table from
# the same source of truth and cannot get them out of step.
function(nova_generate_manifest path effects display output input)
    set(_x "")
    foreach(n ${effects})
        get_property(_a GLOBAL PROPERTY NOVA_MOD_EFFECT_${n}_ACCESSOR)
        set(_x "${_x} \\\n    X(${_a})")
    endforeach()
    list(LENGTH effects _count)

    get_property(_d GLOBAL PROPERTY NOVA_MOD_DISPLAY_${display}_ACCESSOR)
    get_property(_o GLOBAL PROPERTY NOVA_MOD_OUTPUT_${output}_ACCESSOR)
    get_property(_i GLOBAL PROPERTY NOVA_MOD_INPUT_${input}_ACCESSOR)

    file(WRITE "${path}"
"/*
 * nova_modules_generated.h - written by CMake at configure time. Do not edit;
 * every edit is lost on the next configure, and the file it was generated from
 * is the set of module.cmake manifests in the tree.
 *
 * Effect order below is menu order, and the first entry is the effect the box
 * comes up running before stored settings are applied. It comes from each
 * module.cmake's ORDER field.
 */

#ifndef NOVA_MODULES_GENERATED_H
#define NOVA_MODULES_GENERATED_H

// The effect modules, as an X-macro list. modules.c declares one extern per
// entry and builds its table from the same list, so the two cannot get out of
// step.
#define NOVA_EFFECT_MODULES(X)${_x}

// The same count as a plain integer, because the list above is only usable
// where an X macro is currently defined - and EFFECT_MAX in effect.h is used
// far from any such place.
#define NOVA_EFFECT_MODULE_COUNT  ${_count}

#define NOVA_DISPLAY_MODULE  ${_d}
#define NOVA_OUTPUT_MODULE   ${_o}
#define NOVA_INPUT_MODULE    ${_i}

#define NOVA_DISPLAY_NAME    \"${display}\"
#define NOVA_OUTPUT_NAME     \"${output}\"
#define NOVA_INPUT_NAME      \"${input}\"

#endif // NOVA_MODULES_GENERATED_H
")
endfunction()
