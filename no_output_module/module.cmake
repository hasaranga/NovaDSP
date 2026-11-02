# Nothing meaningful reaches the DAC.
#
#   cmake -B build -DNOVA_OUTPUT=none
#
# For a box used as an analyser: the ADC, the effects and the spectrum all run,
# and the output is silence. Read module.c before assuming this turns the DAC
# off - it cannot, and the reason is the state machine rather than the code.
nova_module(
    TYPE     OUTPUT
    NAME     none
    ACCESSOR no_output_module
    DESC     "silent output - analyser only"
    SOURCES  module.c
)
