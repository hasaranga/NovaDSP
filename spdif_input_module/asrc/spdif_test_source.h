// spdif_test_source.h - an IEC 60958 transmitter, for host tests only.
//
// **Nothing in a firmware image includes this.** It exists so that
// test_spdif.cpp and test_input.cpp drive the decoder from the same encoder
// rather than from two that can drift apart - if this one is wrong, both
// suites are wrong in the same direction and the disagreement that would
// have shown it up never happens.
//
// It produces the oversampled pin levels a real transmitter would put on the
// wire: arbitrary source rate (as samples per UI), arbitrary edge jitter,
// correct preambles, correct parity, and a 192-frame channel status block.

#ifndef NOVADSP_SPDIF_TEST_SOURCE_H
#define NOVADSP_SPDIF_TEST_SOURCE_H

#include <stdint.h>

// Preambles as run lengths, in UI. A leading 3 UI run is the only thing in
// the stream that is not biphase, and the run after it names which preamble
// this is: 1 -> B, 3 -> M, 2 -> W.
static const int kPreB[4] = { 3, 1, 1, 3 };   // block start, channel A
static const int kPreM[4] = { 3, 3, 1, 1 };   // channel A
static const int kPreW[4] = { 3, 2, 1, 2 };   // channel B

struct SpdifSource {
    unsigned char *buf;
    int            cap;
    int            n;          // samples written

    double   spu;              // samples per UI - this is where the source
                               // rate lives; 4.0 is 48 kHz at OVERSAMPLE 4
    double   boundary;         // next UI boundary, in samples
    int      level;
    double   jitterUi;         // peak, in UI
    uint32_t rng;

    void begin(unsigned char *b, int capacity, double samplesPerUi)
    {
        buf = b; cap = capacity; n = 0;
        spu = samplesPerUi;
        boundary = 0.0;
        level = 0;
        jitterUi = 0.0;
        rng = 12345u;
    }

    double frand()   // -1..+1
    {
        rng = rng * 1664525u + 1013904223u;
        return (double)(int32_t)rng / 2147483648.0;
    }

    // One UI at the current level. Jitter moves the *boundary*, which is what
    // jitter physically is - the level pattern itself is untouched.
    void ui()
    {
        boundary += spu;
        double edge = boundary;
        if (jitterUi != 0.0) edge += frand() * jitterUi * spu;
        while (n < cap && (double)n < edge) buf[n++] = (unsigned char)level;
    }

    void run(int lengthUi)
    {
        level ^= 1;                       // every run starts with a transition
        for (int i = 0; i < lengthUi; ++i) ui();
    }

    void preamble(const int *p) { for (int i = 0; i < 4; ++i) run(p[i]); }

    void bit(int b)
    {
        // Biphase mark: a transition at the cell boundary, plus one in the
        // middle if the bit is a 1.
        level ^= 1; ui();
        if (b) level ^= 1;
        ui();
    }

    void subframe(const int *pre, int32_t sample24, int cbit)
    {
        preamble(pre);

        int bits[28];
        for (int i = 0; i < 24; ++i) bits[i] = (sample24 >> i) & 1;  // aux+audio
        bits[24] = 0;          // V - valid
        bits[25] = 0;          // U
        bits[26] = cbit & 1;   // C
        int par = 0;
        for (int i = 0; i < 27; ++i) par ^= bits[i];
        bits[27] = par;        // even parity over slots 4..31

        for (int i = 0; i < 28; ++i) bit(bits[i]);
    }

    // One stereo frame. framePos 0 selects the B preamble, which is what
    // starts a 192-frame channel status block.
    void frame(int32_t l, int32_t r, int framePos)
    {
        subframe(framePos == 0 ? kPreB : kPreM, l & 0xFFFFFF, 0);
        subframe(kPreW,                         r & 0xFFFFFF, 0);
    }

    // Start a fresh buffer without starting a fresh signal. The level and the
    // fractional position of the next UI boundary carry over, so a long stream
    // can be generated in chunks without a seam appearing at every join -
    // which is what test_spdif_ui.cpp needs, because one sample per state
    // machine cycle makes a second of audio far too big to hold at once.
    void rebase() { boundary -= (double)n; n = 0; }

    // A stuck level - what a transmitter that stopped looks like on the wire.
    //
    // **The UI boundary moves with it.** A stopped transmitter still occupies
    // the wire, so time passes; leaving `boundary` behind would mean a source
    // resumed after an idle emitted nothing at all until the boundary caught
    // up, and the receiver under test would be blamed for it. Nothing resumed
    // from an idled source until test_spdif_ui.cpp did, which is why this was
    // only wrong in a way nobody had noticed.
    void idle(int samples)
    {
        for (int i = 0; i < samples && n < cap; ++i) buf[n++] = (unsigned char)level;
        boundary += (double)samples;
    }
};

// Pack sample levels into the words PIO would push: first sample in bit 0.
static inline int spdif_pack(const unsigned char *s, int n,
                             uint32_t *words, int maxWords)
{
    const int nw = (n / 32 < maxWords) ? n / 32 : maxWords;
    for (int w = 0; w < nw; ++w) {
        uint32_t v = 0;
        for (int b = 0; b < 32; ++b)
            if (s[w * 32 + b]) v |= (1u << b);
        words[w] = v;
    }
    return nw;
}

#endif // NOVADSP_SPDIF_TEST_SOURCE_H
