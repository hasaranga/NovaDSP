# S/PDIF output: the processed block, biphase-encoded and shifted out on PIO2.
#
# **The easy half of S/PDIF.** Every piece of machinery in spdif_input_module
# - the ASRC, the rate estimator, the lock states, the fade policy - exists
# because a receiver has to follow somebody else's clock. Transmitting, this
# box is the master: the UI rate is 128 * 48000 = 6.144 MHz, clk_sys/50
# exactly, and there is nothing to track and nothing to resample.
#
# tx/test_tx.cpp is deliberately absent from SOURCES - a host program with no
# place in a firmware image, the same as the other five in this tree. It is
# a **loopback**: it decodes what this encoder produces using
# spdif_input_module's receiver and requires the audio back bit exact. That
# is worth more than checking the encoder against a table of expected bytes,
# and it is what found the receiver's digital-silence bug.
#
# Note that this module claims GP22 and leaves GP16 alone, so a build using it
# has no analogue output. module.cpp says why, and what the alternative would
# look like.
nova_module(
    TYPE     OUTPUT
    NAME     spdif
    ACCESSOR spdif_output_module
    DESC     "S/PDIF transmitter (consumer, 24-bit)"
    SOURCES  module.cpp tx/SpdifTx.cpp
    PIO      spdif_tx.pio
)
