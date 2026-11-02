# The PCM1808 on the shared I2S state machine - the input this box has always
# had, now named.
#
# Its fill() is NULL, and that is the honest answer rather than a stub: the
# ADC and the DAC share one PIO state machine and one DMA ring, so the buffer
# the core hands to an input module is *already* the buffer the RX DMA filled.
# There is nothing to copy and nothing to convert.
#
# That mirrors i2s_output_module's empty write(), which is the same fact seen
# from the other end. Both are empty for the same hardware reason, and both
# earn their place by letting a build exist that replaces one end without the
# core learning about it.
nova_module(
    TYPE     INPUT
    NAME     i2s
    ACCESSOR i2s_input_module
    DESC     "I2S ADC - PCM1808 on the shared state machine"
    SOURCES  module.c
)
