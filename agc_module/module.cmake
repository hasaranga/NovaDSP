# Automatic volume levelling with an exact lookahead brick-wall limiter behind
# it. 680 bytes.
#
# ORDER 60 puts it immediately below the noise gate, which is the order the two
# dynamics processors belong in inside a chain: a gate wants the signal as it
# arrived, before anything has lifted the noise floor it is shutting out, and
# this belongs last because it is the only thing that bounds the output.
#
# **It is the one effect that changes the latency of the box** - one block of
# lookahead, 1.33 ms on top of the ~2.7 ms the buffering already costs.
#
# agc/test_autogain.cpp is deliberately absent from SOURCES, for the same
# reason as the gate's.
nova_module(
    TYPE     EFFECT
    NAME     agc
    ACCESSOR agc_module
    ORDER    60
    DESC     "Auto Gain - levelling plus a brick-wall limiter"
    SOURCES  module.cpp agc/AutoGain.cpp
)
