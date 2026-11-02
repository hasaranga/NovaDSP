# No panel and no keys.
#
#   cmake -B build -DNOVA_DISPLAY=none
#
# The audio path, the effects, the spectrum and the settings store are all
# unchanged - what goes is the ability to *see* or *change* anything from the
# box itself. On a dev build the USB console still reports and still adjusts
# parameters; on a dist build there is no interface at all, which is a
# deliberate choice rather than an accident and is worth knowing before
# flashing one.
nova_module(
    TYPE     DISPLAY
    NAME     none
    ACCESSOR no_display_module
    DESC     "no display, no keys"
    SOURCES  module.c
)
