# SH1106/SSD1306 128x64 I2C OLED plus a rotary encoder with push.
#
# The module owns all of it - the panel driver, the quadrature decoder, the
# four-page menu, the meters and the spectrum drawing. The core hands it
# numbers through display.c's seqlocks and knows nothing else about it.
nova_module(
    TYPE     DISPLAY
    NAME     oled
    ACCESSOR oled_display_module
    DESC     "SH1106/SSD1306 OLED + rotary encoder"
    # oled_anim*.c are generated artwork - 61 and 101 frames of 1 KB, all
    # __in_flash, so they cost flash and no SRAM. See the headers for the
    # layout. Adding another is a file here and a row in kAnims in ui.c.
    SOURCES  module.c sh1106.c encoder.c ui.c oled_anim6.c
    LIBS     hardware_i2c
    PIO      encoder.pio
)
