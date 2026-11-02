# A broadband gate: below the threshold it pulls the signal to silence, above
# it the signal passes untouched. 68 bytes, no lookahead.
#
# noisegate/test_noisegate.cpp is deliberately absent from SOURCES - it is a
# host program with no place in a firmware image. Its build command is in its
# own header.
nova_module(
    TYPE     EFFECT
    NAME     noisegate
    ACCESSOR noisegate_module
    ORDER    50
    DESC     "broadband noise gate"
    SOURCES  module.cpp noisegate/NoiseGate.cpp
)
