# S/PDIF input: a biphase-mark receiver on PIO2, an asynchronous sample rate
# converter onto the local 48 kHz grid, and a continuity policy that keeps the
# block flowing when the source does not.
#
# **The whole design question this module answers is that an S/PDIF source
# brings its own clock**, and the RP2350 has no steerable audio PLL to slave
# to it - the finest trim available is the PIO divider's ~156 ppm, coarser
# than the +-100 ppm being corrected. So the correction happens in the data
# domain instead: resample whatever arrives onto our own exactly-48000 grid,
# and let every effect downstream keep believing AUDIO_SAMPLE_RATE. README.md
# has the reasoning and the measurements.
#
# asrc/test_*.cpp are deliberately absent from SOURCES - five host programs
# with no place in a firmware image, the same as the gate's, Auto Gain's and
# the Leveller's. Their build commands are in their own headers.
#
# **Both front ends are built.** SPDIF_USE_PIO_DECODE in asrc/SpdifInput.h
# picks which one the module uses, and the other is dropped whole by
# --gc-sections: the decoder is a member of SpdifInput, so choosing the type
# leaves nothing referring to the other one, and an unused PIO program is a
# const array nobody names. Keeping the loser buildable is the point - it is
# the reference the winner is tested against, and test_spdif.cpp still drives
# it directly.
#
# Between them the suites put 182 checks behind this directory, and
# test_spdif_ui.cpp covers the PIO program itself by assembling it with pioasm
# and executing the instruction words. See asrc/PioSim.h.
nova_module(
    TYPE     INPUT
    NAME     spdif
    ACCESSOR spdif_input_module
    DESC     "S/PDIF receiver with ASRC to 48 kHz"
    SOURCES  module.cpp
             asrc/SpdifInput.cpp
             asrc/SpdifRx.cpp
             asrc/SpdifRxUi.cpp
             asrc/Asrc.cpp
             asrc/RateEstimator.cpp
    PIO      spdif_rx.pio
             spdif_rx_ui.pio
)
