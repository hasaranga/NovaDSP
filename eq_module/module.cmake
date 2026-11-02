# Ten band graphic EQ, 70 Hz to 16 kHz, +-12 dB a band, with a preamp. 468 bytes.
nova_module(
    TYPE     EFFECT
    NAME     eq
    ACCESSOR eq_module
    ORDER    30
    DESC     "ten band graphic equalizer"
    SOURCES  module.cpp equalizer/Equalizer.cpp
)
