# The PCM5102A on the I2S bus. The default, and on this hardware the ordinary
# case: the block the effect processed is already the buffer the TX DMA will
# drain, so there is nothing for this module to do but say so at boot.
nova_module(
    TYPE     OUTPUT
    NAME     i2s
    ACCESSOR i2s_output_module
    DESC     "I2S DAC (PCM5102A)"
    SOURCES  module.c
)
