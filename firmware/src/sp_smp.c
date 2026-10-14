/* SPDX-License-Identifier: GPL-3.0-only */
/* zp12 SAMPLE: sampling over USB. With ZP12_UAI the FM-1 is also a USB audio output (usb.c): what a computer or
 * a phone plays into it arrives here as 44.1 kHz 16-bit stereo, a packet a millisecond (usb_poll, TIMER5 ISR).
 * It is summed to mono and set by LEVEL; on the SAMPLE page it is heard (MON) and metered; recording, it goes
 * through the input filter (2-pole low-pass at 12 kHz), down to 27.5 kHz (the SP-12's rate, linear between the
 * input's samples) and to 12 bits, packed as the kit's waves (2 in 3 bytes) into RAM. The take plays as wave
 * SMP_WAVE on the pad it was sampled for. No hardware here: tests/sp_smp_test.c feeds it packets.
 *
 * Who does what: smp_usb (USB, TIMER5) records and moves IDLE -> REC -> DONE (ARMED -> REC at the threshold);
 * the main loop arms (smp_arm), stops (smp_stop) and takes a DONE take (smp_take); the audio ISR hears the
 * input (smp_monitor). The take lives in RAM only: it is gone at power-off (saving to flash comes next). */
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif
#define SMP_RATE 27500u
#define SMP_STEP 105096u                    /* 44100 / 27500, Q16 */
#define SMP_BYTES (72u * 1024u)              /* 72 KiB of RAM: 49152 samples, 1.79 s at 27.5 kHz */
#define SMP_MAXN (SMP_BYTES / 3u * 2u)
#define SMP_WAVE 63u                        /* the take's slot in sp_wave[] (the kit and the own samples are below) */
#define SMP_MON_N 1024u                     /* the monitor's ring, 44.1 kHz mono; a power of two */
enum { SM_IDLE, SM_ARMED, SM_REC, SM_DONE };

static uint8_t smp_buf[SMP_BYTES];
static struct {
    volatile uint8_t state;
    uint8_t active;                         /* the SAMPLE page is open: monitor, meter, record */
    int8_t level;                           /* LEVEL, dB -12..+24 */
    uint8_t thresh;                         /* THRESH 0 (off: REC starts at once) .. 99 */
    uint8_t mon;                            /* MON: the input heard on the output */
    uint8_t max10;                          /* MAX: tenths of a second a take may last */
    uint8_t pad;                            /* the pad the take is for */
    int32_t gain;                           /* Q8, from LEVEL */
    int32_t thr;                            /* THRESH as a level, 0..32767 */
    volatile uint32_t n;                    /* samples recorded */
    uint32_t maxn;
    uint32_t phase;                         /* Q16: where the next 27.5 kHz sample falls after the last input sample */
    int32_t prev, x1, x2, y1, y2;           /* the last filtered sample; the filter's state */
    volatile uint32_t peak;                 /* the loudest |sample| since the meter last looked */
    volatile uint32_t clip;                 /* packets that clipped (after LEVEL) */
    volatile uint32_t pkts;                 /* packets taken */
    int16_t mon_ring[SMP_MON_N];
    volatile uint32_t mon_w, mon_r;
} smp = {.level = 0, .thresh = 0, .mon = 1, .max10 = SMP_MAXN * 10u / SMP_RATE, .gain = 256};

/* LEVEL in dB -> Q8: 2^(dB / 6.02), the octaves shifted, the rest from sp_pow2_cents (Q16) */
static void smp_set_level(int32_t db)
{
    int32_t c, sh = 0;
    smp.level = (int8_t)sp_clamp(db, -12, 24);
    c = smp.level * 1993 / 10;              /* cents: 1200 / 6.0206 per dB */
    while (c > 1200) { c -= 1200; sh++; }
    smp.gain = (int32_t)(sp_pow2_cents(c) >> 8) << sh;
}

static void smp_set_thresh(int32_t t)
{
    smp.thresh = (uint8_t)sp_clamp(t, 0, 99);
    smp.thr = smp.thresh * smp.thresh * 32767 / 9801;   /* squared: fine steps where it matters, near silence */
}

/* one 27.5 kHz sample, -2048..2047, onto the take */
static inline void smp_put(int32_t v)
{
    uint8_t *d = smp_buf + (smp.n >> 1) * 3u;
    uint32_t u = (uint32_t)v & 0xFFFu;
    if (!(smp.n & 1u)) {
        d[0] = (uint8_t)u;
        d[1] = (uint8_t)(u >> 8);
    } else {
        d[1] = (uint8_t)((d[1] & 15u) | (u << 4));
        d[2] = (uint8_t)(u >> 4);
    }
    smp.n++;
}

/* a packet from the host: frames of 16-bit L, R (USB, TIMER5 ISR) */
static void smp_usb(const uint8_t *p, uint32_t bytes)
{
    uint32_t i, frames = bytes / 4u, pk = smp.peak, clip = 0;
    smp.pkts++;
    if (!smp.active)
        return;
    for (i = 0; i < frames; i++, p += 4) {
        int32_t l = (int16_t)(p[0] | p[1] << 8), r = (int16_t)(p[2] | p[3] << 8);
        int32_t x = ((l + r) * smp.gain) >> 9, a, y;   /* (the sum halved: >> 1, Q8: >> 8) */
        if (x > 32767) { x = 32767; clip = 1; }
        if (x < -32767) { x = -32767; clip = 1; }
        a = x < 0 ? -x : x;
        if ((uint32_t)a > pk) pk = (uint32_t)a;
        if (smp.mon && smp.mon_w - smp.mon_r < SMP_MON_N) {
            smp.mon_ring[smp.mon_w & (SMP_MON_N - 1u)] = (int16_t)x;
            RING_PUBLISH();
            smp.mon_w++;
        }
        y = (5485 * (x + smp.x2) + 10970 * smp.x1 - 2669 * smp.y1 - 2888 * smp.y2) >> 14;   /* 12 kHz, Q 0.71 */
        smp.x2 = smp.x1; smp.x1 = x;
        smp.y2 = smp.y1; smp.y1 = y;
        if (smp.state == SM_ARMED && a >= smp.thr) {   /* the threshold: the take starts with this sample */
            smp.state = SM_REC;
            smp.phase = 0;
            smp.prev = y;
        }
        if (smp.state == SM_REC) {
            while (smp.phase < 65536u) {           /* the 27.5 kHz samples between the last input sample and this */
                int32_t v = smp.prev + (int32_t)(((y - smp.prev) * (int32_t)(smp.phase >> 4)) >> 12);
                v = sp_clamp(v >> 4, -2048, 2047);
                smp_put(v);
                smp.phase += SMP_STEP;
                if (smp.n >= smp.maxn) {
                    smp.state = SM_DONE;
                    break;
                }
            }
            smp.phase -= 65536u;
        }
        smp.prev = y;
    }
    smp.peak = pk;
    smp.clip += clip;
}

/* the audio ISR: the input on the output while the SAMPLE page is open and MON is on (the host's clock and the
 * FM-1's drift apart: a ring that fills too far loses a sample, one that runs dry is silent) */
static void smp_monitor(int32_t *o, uint32_t frames)
{
    uint32_t i, r = smp.mon_r, fill = smp.mon_w - r;
    if (!smp.active || !smp.mon) {
        smp.mon_r = smp.mon_w;
        return;
    }
    if (fill > SMP_MON_N * 3u / 4u)
        r++;
    for (i = 0; i < frames && r != smp.mon_w; i++, r++) {
        int32_t s = smp.mon_ring[r & (SMP_MON_N - 1u)];
        o[2u * i] += s;
        o[2u * i + 1u] += s;
    }
    RING_PUBLISH();
    smp.mon_r = r;
}

/* the main loop: REC on the SAMPLE page. A take on its way is stopped (and kept); else the old take goes and a new
 * one waits for the threshold (THRESH OFF: starts at once) */
static void smp_arm(uint32_t pad)
{
    uint32_t i;
    if (smp.state == SM_ARMED || smp.state == SM_REC) {
        smp.state = smp.n ? SM_DONE : SM_IDLE;
        return;
    }
    sp_wave[SMP_WAVE].n = 0;                /* (first: no new voice takes the old take) */
    for (i = 0; i < SP_NCH; i++)
        if (sp_ch[i].w == &sp_wave[SMP_WAVE]) sp_ch[i].on = 0;
    smp.pad = (uint8_t)pad;
    smp.n = 0;
    smp.maxn = (uint32_t)smp.max10 * (SMP_RATE / 10u);
    if (smp.maxn > SMP_MAXN || !smp.maxn) smp.maxn = SMP_MAXN;
    RING_PUBLISH();
    smp.state = SM_ARMED;
}

/* the page closed: an armed take is dropped, one recording is kept */
static void smp_close(void)
{
    if (smp.state == SM_ARMED) smp.state = SM_IDLE;
    if (smp.state == SM_REC) smp.state = SM_DONE;
    smp.active = 0;
}

/* a finished take: its wave, returned (or 0xFF when there is none) */
static uint32_t smp_take(void)
{
    if (smp.state != SM_DONE)
        return 0xFFu;
    if (smp.n & 1u)
        smp_put(0);                         /* (the pair's other half: silence) */
    sp_wave[SMP_WAVE].d = smp_buf;
    sp_wave[SMP_WAVE].rate = SMP_RATE;
    RING_PUBLISH();
    sp_wave[SMP_WAVE].n = smp.n;
    smp.state = SM_IDLE;
    return SMP_WAVE;
}

/* the meter: the peak since the last look, 0..32767 (it falls as the main loop looks) */
static uint32_t smp_meter(void)
{
    static uint32_t shown;
    uint32_t p = smp.peak;
    smp.peak = 0;
    shown = p > shown ? p : shown - shown / 8u;
    return shown;
}
