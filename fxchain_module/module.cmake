# Runs up to four of the other effects in series, in an order you set.
#
# It names none of them: late_init() walks the finished registry to build its
# candidate list, which is why it works with whatever module set this build
# happens to have and why it needs no updating when one is added or removed.
#
# ORDER 70 puts it below every processor and above Passthru.
nova_module(
    TYPE     EFFECT
    NAME     fxchain
    ACCESSOR fxchain_module
    ORDER    70
    DESC     "FX Chain - four effects in series"
    SOURCES  module.c
)
