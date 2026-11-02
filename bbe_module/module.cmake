# A BBE Sonic Maximizer style processor: dynamic HF restoration driven by how
# much the lows are masking the highs, plus a bass shelf. 444 bytes.
nova_module(
    TYPE     EFFECT
    NAME     bbe
    ACCESSOR bbe_module
    ORDER    20
    DESC     "BBE Sonic Maximizer"
    SOURCES  module.cpp bbe_maximizer/SonicMaximizer.cpp
)
