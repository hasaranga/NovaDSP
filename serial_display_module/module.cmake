# The USB serial port as the display, the keyboard as the knob.
#
#   cmake -B build-dist -DNOVA_DIST=ON -DNOVA_DISPLAY=serial
#
# The whole menu on a terminal: the effect list, the parameter pages, the chain
# editor, DSP STATUS with live meters and the Save row. For a box with no panel
# fitted, where the alternative - -DNOVA_DISPLAY=none on a dist image - leaves
# no interface at all.
#
# ---- dist only, and the build says so at compile time -----------------------
#
# There is one USB CDC device and a dev build is already using it: the
# once-a-second status line writes to it and the main loop's getchar reads from
# it. Both would land in the middle of this module's escape sequences.
#
# That check is an #error in module.c rather than a message() here, and
# deliberately: this file is the module declaring itself, and NOVA_DIST is not
# declared yet when nova_discover_modules() includes it. Putting the test in
# CMakeLists.txt instead would mean a core file naming a module, which is the
# one thing the module system exists to avoid. A compile error naming the fix
# is a fine second best.
#
# ---- the character set -----------------------------------------------------
#
# The frame is drawn with UTF-8 box-drawing characters, which is what current
# PuTTY expects by default (Window -> Translation -> Remote character set). Add
# DEFINES NOVA_SERIAL_ASCII=1 below for a terminal set to something else; the
# frame becomes +, - and | and nothing else changes.
#
# No extra LIBS: stdio over USB CDC is already in the core's pico_stdlib, and
# turning it off is what would break this module rather than what would be
# needed to enable it.
nova_module(
    TYPE     DISPLAY
    NAME     serial
    ACCESSOR serial_display_module
    DESC     "USB serial terminal + keyboard (dist builds only)"
    SOURCES  module.c term.c keys.c ui.c
)
