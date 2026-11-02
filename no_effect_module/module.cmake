# The placeholder effect, for a build with no real ones.
#
# DEFAULT OFF: it is not something you would choose alongside a real effect,
# and it would only take up a menu row. CMake enables it automatically when the
# effect set would otherwise be empty - see nova_select_modules().
#
# ORDER is the last position, so that if somebody does enable it alongside real
# effects it sits at the bottom of the menu and is not what the box boots on.
nova_module(
    TYPE     EFFECT
    NAME     none
    ACCESSOR no_effect_module
    ORDER    900
    DEFAULT  OFF
    DESC     "no effect - bit-exact passthrough"
    SOURCES  module.c
)
