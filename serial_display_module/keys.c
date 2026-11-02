#include "keys.h"

#include "pico/stdlib.h"

// ---- the escape-sequence state machine ---------------------------------

typedef enum { ST_IDLE, ST_ESC, ST_CSI } state_t;

static state_t  state;
static uint32_t esc_ms;        // when the pending ESC arrived
static int      csi_param;     // digits accumulated since the CSI introducer

static keys_state_t acc;       // filled by feed(), drained by keys_poll()

// One page of movement, for PgUp/PgDn. Nothing on the panel does this; a
// keyboard has the keys and a list can be longer than the box.
#define PAGE_STEP  10

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void feed(int c);

// The final byte of a CSI sequence. Anything not handled is swallowed rather
// than passed on - an unrecognised function key should do nothing, not scroll.
static void csi_final(int c)
{
    switch (c) {
    case 'A': acc.delta -= 1;          break;   // up
    case 'B': acc.delta += 1;          break;   // down
    case 'C': acc.press_edge = true;   break;   // right: into the thing
    case 'D': acc.back_edge  = true;   break;   // left:  out of it
    case '~':
        if (csi_param == 5) acc.delta -= PAGE_STEP;   // PgUp
        if (csi_param == 6) acc.delta += PAGE_STEP;   // PgDn
        break;
    default: break;
    }
    state = ST_IDLE;
}

static void feed_idle(int c)
{
    switch (c) {
    case 0x1B: state = ST_ESC; esc_ms = now_ms(); break;

    case '\r': case '\n': case ' ':
        acc.press_edge = true;  break;

    // Up: the same three spellings the dev console uses for "less", plus the
    // vi key, so nobody has to remember which one this is.
    case '-': case ',': case 'k': case 'K':
        acc.delta -= 1;         break;

    case '+': case '.': case 'j': case 'J':
        acc.delta += 1;         break;

    case 'q': case 'Q': case 0x08: case 0x7F:
        acc.back_edge = true;   break;

    case 's': case 'S':
        acc.save_edge = true;   break;

    case 'r': case 'R': case 0x0C:              // 0x0C is Ctrl-L
        acc.redraw_edge = true; break;

    default: break;
    }
}

static void feed(int c)
{
    switch (state) {
    case ST_IDLE:
        feed_idle(c);
        break;

    case ST_ESC:
        if (c == '[' || c == 'O') {             // CSI, or application cursor
            state     = ST_CSI;
            csi_param = 0;
        } else if (c == 0x1B) {
            // Esc pressed twice. The first one is real; the second starts over.
            acc.back_edge = true;
            esc_ms        = now_ms();
        } else {
            // Esc followed by something that is not a sequence: the Esc was
            // real and the character is its own keystroke.
            acc.back_edge = true;
            state         = ST_IDLE;
            feed_idle(c);
        }
        break;

    case ST_CSI:
        if (c >= '0' && c <= '9') {
            csi_param = csi_param * 10 + (c - '0');
        } else if (c == ';') {
            csi_param = 0;                      // only the last parameter matters
        } else {
            csi_final(c);
        }
        break;
    }
}

// ---- entry points -------------------------------------------------------

void keys_init(void)
{
    state     = ST_IDLE;
    esc_ms    = 0;
    csi_param = 0;

    acc.delta       = 0;
    acc.press_edge  = false;
    acc.back_edge   = false;
    acc.save_edge   = false;
    acc.redraw_edge = false;
}

void keys_poll(keys_state_t *out)
{
    // Bounded rather than "until empty": a host pasting text, or a stuck key
    // repeat, must not be able to hold core 1 in this loop. Anything left over
    // is read on the next poll a few milliseconds later.
    for (int i = 0; i < 64; ++i) {
        const int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) break;
        feed(c);
    }

    // A lone ESC that nothing followed. Delivered here rather than in feed()
    // because the thing that decides it is the passage of time, and feed() is
    // only reached when a byte arrives - which in this case is exactly what did
    // not happen.
    if (state == ST_ESC && (now_ms() - esc_ms) >= KEYS_ESC_TIMEOUT_MS) {
        acc.back_edge = true;
        state         = ST_IDLE;
    }

    *out = acc;

    acc.delta       = 0;
    acc.press_edge  = false;
    acc.back_edge   = false;
    acc.save_edge   = false;
    acc.redraw_edge = false;
}
