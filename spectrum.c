#include "spectrum.h"
#include "nova_build.h"

#include <math.h>
#include <stdio.h>

#include "audio.h"
#include "cycles.h"
#define FFT_N       SPECTRUM_FFT_N       // real samples per transform
#define FFT_NC      (FFT_N / 2)          // the complex transform that does it
#define FFT_LOG     8                    // log2(FFT_NC)

_Static_assert(FFT_NC == (1 << FFT_LOG), "FFT_LOG must be log2(FFT_N / 2)");
_Static_assert(FFT_N % AUDIO_BLOCK_FRAMES == 0,
               "the window has to fill in a whole number of blocks");

// The displayed range. The bottom is set by the bin width - below ~94 Hz there
// is nothing to resolve at this transform size - and the top by where the
// material and the ADC both run out.
#define SPEC_F_LO   80.0f
#define SPEC_F_HI   16000.0f

// A periodic Hann window sums to exactly N/2, and a sine at bin centre comes
// out at |X| = A * sum(w) / 2. So this scales |X| back to amplitude, and a
// full-scale sine reads 0 dBFS in whichever band it lands in.
#define FFT_AMP_SCALE   (2.0f / ((float)FFT_N * 0.5f))
#define FFT_PWR_SCALE   (FFT_AMP_SCALE * FFT_AMP_SCALE)

static const float kTwoPi = 6.28318530717958648f;

// ---- tables, built once at init ----------------------------------------

static float    hann[FFT_N];
static float    tw_r[FFT_NC / 2], tw_i[FFT_NC / 2];   // complex-FFT twiddles
static float    ut_r[FFT_NC], ut_i[FFT_NC];           // untangle twiddles
static uint16_t brev[FFT_NC];
static uint16_t band_lo[SPECTRUM_BANDS], band_hi[SPECTRUM_BANDS];

// ---- working state (core 0 only, so none of it needs guarding) ---------

static float    win[FFT_N];          // mono, +-1.0, filled a block at a time
static int      win_fill;
static float    fr[FFT_NC], fi[FFT_NC];
static float    pwr[FFT_NC + 1];     // |X[k]|^2 for k = 0..N/2
static float    band_db[SPECTRUM_BANDS];
static uint32_t t_max;

// ---- band layout -------------------------------------------------------
//
// Log spacing from SPEC_F_LO up, except that the bottom bands come out
// narrower than a bin and would otherwise collapse onto each other. Forcing
// each band to start one bin past the end of the last makes the bottom five
// effectively linear - 94 Hz apart, which is all this transform has down there
// - and lets the log spacing take over from about 600 Hz where it fits.

static void build_bands(void)
{
    const float bin_hz = (float)AUDIO_SAMPLE_RATE / (float)FFT_N;
    const float ratio  = powf(SPEC_F_HI / SPEC_F_LO,
                              1.0f / (float)SPECTRUM_BANDS);

    float f    = SPEC_F_LO;
    int   next = 1;                       // bin 0 is DC; never show it

    for (int b = 0; b < SPECTRUM_BANDS; ++b) {
        f *= ratio;                       // upper edge of this band

        int hi = (int)(f / bin_hz);
        if (hi < next)   hi = next;       // never empty, never overlapping
        if (hi > FFT_NC) hi = FFT_NC;

        band_lo[b] = (uint16_t)next;
        band_hi[b] = (uint16_t)hi;

        next = hi + 1;
        if (next > FFT_NC) next = FFT_NC;
    }
}

// ---- the transform -----------------------------------------------------

// Iterative radix-2 decimation in time, in place over fr[]/fi[].
static void fft_complex(void)
{
    for (int i = 0; i < FFT_NC; ++i) {
        const int j = brev[i];
        if (j > i) {
            float t = fr[i]; fr[i] = fr[j]; fr[j] = t;
            t = fi[i]; fi[i] = fi[j]; fi[j] = t;
        }
    }

    for (int len = 2; len <= FFT_NC; len <<= 1) {
        const int half = len >> 1;
        const int step = FFT_NC / len;          // stride into the twiddle table

        for (int i = 0; i < FFT_NC; i += len) {
            int t = 0;
            for (int a = i; a < i + half; ++a, t += step) {
                const int   b  = a + half;
                const float wr = tw_r[t], wi = tw_i[t];

                const float xr = fr[b] * wr - fi[b] * wi;
                const float xi = fr[b] * wi + fi[b] * wr;

                fr[b] = fr[a] - xr;   fi[b] = fi[a] - xi;
                fr[a] = fr[a] + xr;   fi[a] = fi[a] + xi;
            }
        }
    }
}

static void transform(void)
{
    // Pack the windowed real signal into FFT_NC complex points - even samples
    // into the real part, odd into the imaginary. The transform of that is not
    // the spectrum of anything, but the two interleave in a way the untangle
    // below can separate, and it is what halves the cost.
    for (int n = 0; n < FFT_NC; ++n) {
        const int e = 2 * n;
        fr[n] = win[e]     * hann[e];
        fi[n] = win[e + 1] * hann[e + 1];
    }

    fft_complex();

    // X[0] and X[N/2] are both purely real and fall out of Z[0] on its own.
    {
        const float a = fr[0] + fi[0];
        const float b = fr[0] - fi[0];
        pwr[0]      = a * a;
        pwr[FFT_NC] = b * b;
    }

    // For the rest: the transform of the even samples is the part of Z that is
    // symmetric about k <-> NC-k, the odd samples the antisymmetric part, and
    // a twiddle recombines them into the real spectrum.
    for (int k = 1; k < FFT_NC; ++k) {
        const float zr = fr[k], zi = fi[k];
        const float mr = fr[FFT_NC - k], mi = -fi[FFT_NC - k];   // conjugate

        const float er = 0.5f * (zr + mr);      // even-sample transform
        const float ei = 0.5f * (zi + mi);

        const float dr = 0.5f * (zr - mr);      // odd, still to be divided by j
        const float di = 0.5f * (zi - mi);
        const float orr =  di;                  // (dr + j di) / j
        const float oii = -dr;

        const float wr = ut_r[k], wi = ut_i[k];
        const float xr = er + orr * wr - oii * wi;
        const float xi = ei + orr * wi + oii * wr;

        pwr[k] = xr * xr + xi * xi;
    }

    // Peak bin per band, not the sum across it. The top band spans forty-odd
    // bins and the bottom one a single bin, so summing would tilt the whole
    // display upwards purely by bandwidth and make broadband material look
    // like a ramp. Peak makes a tone read at its own level wherever it lands.
    for (int b = 0; b < SPECTRUM_BANDS; ++b) {
        float peak = 0.0f;
        for (int k = band_lo[b]; k <= band_hi[b]; ++k)
            if (pwr[k] > peak) peak = pwr[k];

        // The epsilon is what keeps silence at a finite -200 dB instead of
        // -inf, which would poison the decay arithmetic on the other core.
        band_db[b] = 10.0f * log10f(peak * FFT_PWR_SCALE + 1e-20f);
    }
}

// ---- entry points ------------------------------------------------------

void spectrum_init(void)
{
    for (int i = 0; i < FFT_N; ++i)
        hann[i] = 0.5f - 0.5f * cosf(kTwoPi * (float)i / (float)FFT_N);

    for (int t = 0; t < FFT_NC / 2; ++t) {
        const float a = -kTwoPi * (float)t / (float)FFT_NC;
        tw_r[t] = cosf(a);
        tw_i[t] = sinf(a);
    }

    for (int k = 0; k < FFT_NC; ++k) {
        const float a = -kTwoPi * (float)k / (float)FFT_N;
        ut_r[k] = cosf(a);
        ut_i[k] = sinf(a);
    }

    for (int i = 0; i < FFT_NC; ++i) {
        unsigned r = 0;
        for (int b = 0; b < FFT_LOG; ++b)
            r |= (((unsigned)i >> b) & 1u) << (FFT_LOG - 1 - b);
        brev[i] = (uint16_t)r;
    }

    build_bands();

    for (int b = 0; b < SPECTRUM_BANDS; ++b) band_db[b] = -200.0f;
}

void spectrum_feed(const int32_t *buf)
{
    // Mono sum of the output. One transform rather than two, and a level
    // display is not where the difference between the channels shows up.
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; i += 2) {
        const float l = (float)(buf[i]     >> 8);
        const float r = (float)(buf[i + 1] >> 8);
        win[win_fill++] = (l + r) * (0.5f / 8388608.0f);
    }

    if (win_fill < FFT_N) return;
    win_fill = 0;

    const uint32_t t0 = cyc_now();
    transform();
    const uint32_t dt = cyc_since(t0);
    if (dt > t_max) t_max = dt;
}

void spectrum_get(spectrum_frame_t *out)
{
    for (int b = 0; b < SPECTRUM_BANDS; ++b) out->band_db[b] = band_db[b];
}

uint32_t spectrum_timing_take(void)
{
    const uint32_t v = t_max;
    t_max = 0;
    return v;
}

float spectrum_band_centre_hz(int band)
{
    if (band < 0 || band >= SPECTRUM_BANDS) return 0.0f;

    const float bin_hz = (float)AUDIO_SAMPLE_RATE / (float)FFT_N;

    // Geometric, not arithmetic: the only caller wants it for a per-octave
    // slope, and on a log axis the geometric centre is the middle of the band.
    const float lo = (float)band_lo[band] * bin_hz;
    const float hi = (float)(band_hi[band] + 1) * bin_hz;

    return sqrtf(lo * hi);
}

void spectrum_report(void)
{
    const float bin_hz = (float)AUDIO_SAMPLE_RATE / (float)FFT_N;

    NOVA_LOG("  spectrum: %d-pt FFT of the effect output, %d bands "
           "%.0f Hz..%.1f kHz, %.1f ms window\n",
           FFT_N, SPECTRUM_BANDS,
           (double)((float)band_lo[0] * bin_hz),
           (double)((float)band_hi[SPECTRUM_BANDS - 1] * bin_hz / 1000.0f),
           (double)(1000.0f * (float)FFT_N / (float)AUDIO_SAMPLE_RATE));
}
