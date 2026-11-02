# A gain trim, -12 to +12 dB.
#
# ORDER 5 is first on purpose, and it is the whole of how "the box comes up on
# Passthru" is expressed: the effect modules register in ORDER order and the
# first one registered is what is active before settings_load() runs. There is
# no separate "default effect" setting, and this is why there does not need to
# be one.
#
# It used to be ORDER 80, last, on the argument that Passthru is the thing you
# switch *to* in order to hear what an effect was doing. That argument is still
# true and it is now outranked by the other one: an audio box should come up
# passing audio through unchanged, so that what you hear on the first boot
# after a flash is your source and not somebody's power-on parameter table.
# Reaching it is one press either way.
nova_module(
    TYPE     EFFECT
    NAME     thru
    ACCESSOR thru_module
    ORDER    5
    DESC     "Passthru - gain trim"
    SOURCES  module.c
)
