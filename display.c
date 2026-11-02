#include "display.h"
#include "nova_build.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/sync.h"

// ---- core 0 -> core 1 handover -----------------------------------------
//
// A seqlock. The writer bumps the counter to odd, writes, bumps it to even;
// the reader takes a copy between two equal even reads. Core 0 never waits,
// which is the whole point.

static volatile uint32_t  core1_ticks;

// Where core 1 is, for the once-a-second stall report. "core1 STALLED at 1
// ticks" says it froze inside the first pass of the loop and nothing says
// which call it froze in - these four values do, and they cost one store
// each on a loop that runs 500 times a second.
//
//   1 status_read   2 spectrum_read   3 module tick   4 sleep
static volatile uint32_t  core1_stage;

static volatile uint32_t  status_seq;
static display_status_t   status_shared;

void display_publish(const display_status_t *s)
{
    uint32_t seq = status_seq + 1u;
    status_seq = seq;              // odd: a write is in progress
    __dmb();
    status_shared = *s;
    __dmb();
    status_seq = seq + 1u;         // even: consistent again
}

static void status_read(display_status_t *out)
{
    uint32_t a, b;
    do {
        a = status_seq;
        __dmb();
        *out = status_shared;
        __dmb();
        b = status_seq;
    } while ((a & 1u) || a != b);
}

static volatile uint32_t  spec_seq;
static spectrum_frame_t   spec_shared;

void display_publish_spectrum(const spectrum_frame_t *s)
{
    uint32_t seq = spec_seq + 1u;
    spec_seq = seq;
    __dmb();
    spec_shared = *s;
    __dmb();
    spec_seq = seq + 1u;
}

static void spectrum_read(spectrum_frame_t *out)
{
    uint32_t a, b;
    do {
        a = spec_seq;
        __dmb();
        *out = spec_shared;
        __dmb();
        b = spec_seq;
    } while ((a & 1u) || a != b);
}

// ---- core 1 ------------------------------------------------------------

static void core1_main(void)
{
    const display_module_t *m = modules_display();

    display_status_t   st   = {0};
    spectrum_frame_t   spec = {0};

    // Flash writes on core 0 have to be able to pause this core. Without this
    // the settings save fails its handshake and never commits - and it fails
    // whether or not there is a panel, so it belongs here rather than in any
    // module.
    multicore_lockout_victim_init();

    if (m->start) m->start();

    const uint32_t poll_us = m->poll_us ? m->poll_us : 2000u;

    while (true) {
        core1_ticks++;

        core1_stage = 1; status_read(&st);
        core1_stage = 2; spectrum_read(&spec);
        core1_stage = 3; m->tick(&st, &spec);
        core1_stage = 4; sleep_us(poll_us);
    }
}

// ---- entry points (core 0) ---------------------------------------------

bool display_init(void)
{
    // Seed the spectrum channel at silence. Nothing has published yet, and an
    // all-zeroes frame would read as 0 dBFS in every band - a screen of
    // full-height bars for the first frame the idle screen is up.
    spectrum_frame_t quiet;
    for (int b = 0; b < SPECTRUM_BANDS; ++b) quiet.band_db[b] = -200.0f;
    display_publish_spectrum(&quiet);

    const display_module_t *m = modules_display();
    return m->init ? m->init() : true;
}

void display_start(void)
{
    multicore_launch_core1(core1_main);
}

uint32_t display_core1_ticks(void) { return core1_ticks; }
uint32_t display_core1_stage(void) { return core1_stage; }
void     display_core1_mark(uint32_t where) { core1_stage = where; }

uint32_t display_errors(void)
{
    const display_module_t *m = modules_display();
    return m->errors ? m->errors() : 0u;
}
