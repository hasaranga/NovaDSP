/*
 * no_display_module - a build with no panel and no keys.
 *
 * init() succeeds, tick() does nothing, and core 1 spins at a tenth of a hertz
 * doing it. Everything else in the firmware is unchanged: the effects run, the
 * spectrum is computed and published, the settings store loads and saves, and
 * on a dev build the USB console still reports and still adjusts parameters.
 *
 * ---- why core 1 is still launched ---------------------------------------
 *
 * It looks like waste and it is not. A flash write on core 0 has to be able to
 * park the other core, and the handshake that does it needs
 * multicore_lockout_victim_init() to have run *there*. That call is in
 * display.c's core 1 loop, so it happens whichever display module is selected
 * - which is exactly the property wanted, because a settings save that works
 * on one display module and silently fails to commit on another is the kind of
 * fault that takes a day to find.
 *
 * The cost is core 1's stack, which lives in the scratch banks and is not
 * counted against the image either way.
 *
 * ---- what is genuinely lost ---------------------------------------------
 *
 * On a **dist** build this leaves the box with no interface at all: no panel,
 * no keys, and no console either. It will run whatever was last saved to
 * flash, for ever, and the only way to change it is to reflash. That is a
 * legitimate thing to want for a fixed installation; it is not a good thing to
 * discover afterwards.
 */

#include "module.h"

static bool none_init(void) { return true; }

static void none_tick(const display_status_t *status,
                      const spectrum_frame_t *spec)
{
    (void)status;
    (void)spec;
}

static void none_report(void)
{
    NOVA_LOG("  display: none - no panel, no keys\n");
}

static const display_module_t kModule = {
    .name    = "none",
    .init    = none_init,
    .start   = NULL,
    .tick    = none_tick,
    .report  = none_report,
    .errors  = NULL,

    // Nothing here is waiting for an edge, so there is no reason to wake often.
    // The loop still has to turn over for display_core1_ticks() to climb, which
    // is what the once-a-second health line watches.
    .poll_us = 100000u,
};

const display_module_t *no_display_module(void) { return &kModule; }
