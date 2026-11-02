# Audacity's Leveller: a static soft-compression curve applied one to five
# times, driving a gain off a peak envelope rather than shaping each sample.
# No lookahead, so it adds no latency.
#
# ORDER 65 slots it between the Noise Gate (50) / Auto Gain (60) pair and FX
# Chain (70), which is also the order the three dynamics processors belong in
# inside a chain: gate first because it wants the signal as it arrived, this
# next because it rides levels within a passage, Auto Gain last because it is
# the only thing that bounds the output. The 10-spaced ORDER field is there
# exactly so this could be slotted in without renumbering anything.
#
# leveller/test_leveller.cpp is deliberately absent from SOURCES - it is a host
# program with no place in a firmware image, the same as the gate's and Auto
# Gain's. It carries the double engine this one was converted from, so it is
# what stands behind the conversion; its build command is in its own header.
#
# **Three of the engine's eight parameters are published** - Degree, Thresh and
# Bypass. The other five sit at the engine's defaults and are not on the panel,
# in flash or in the layout hash. module.cpp says which and why.
nova_module(
    TYPE     EFFECT
    NAME     leveller
    ACCESSOR leveller_module
    ORDER    65
    DESC     "Leveller - soft compression curve, no lookahead"
    SOURCES  module.cpp leveller/Leveller.cpp
)
