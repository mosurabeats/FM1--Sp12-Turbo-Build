/* SPDX-License-Identifier: GPL-3.0-only */
/* zp12 SAMPLE (firmware/src/sp_smp.c) on the host: USB audio packets as a host sends them (44.1 kHz 16-bit stereo,
 * 44 / 45 frames a millisecond) into the sampler, the take read back as the sound core reads it. Checks: length
 * and rate (27.5 kHz), pitch and level kept, the threshold, MAX, LEVEL, clipping, the input filter, the monitor,
 * nothing taken with the page closed. With an argument: the take of a sweep as a WAV to listen to.
 *   cc -O2 -Wall -Wno-unused-function -o build/host/sp_smp_test tests/sp_smp_test.c -lm && build/host/sp_smp_test [take.wav] */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../firmware/src/sp_core.c"
#include "../firmware/src/sp_smp.c"

static int bad;
static void check(int ok, const char *what) { printf("smp: %-66s %s\n", what, ok ? "ok" : "FAIL"); bad += !ok; }

/* ms milliseconds of a sine (hz, amp of full scale; 0: silence) as USB packets */
static double ph;
static uint32_t acc;
static void feed(double hz, double amp, uint32_t ms)
{
    static uint8_t pk[46 * 4];
    uint32_t m, i;
    for (m = 0; m < ms; m++) {
        uint32_t n = 44u;
        acc += 100u;                                    /* 44.1 frames a ms: 44, one 45 in ten */
        if (acc >= 1000u) { acc -= 1000u; n++; }
        for (i = 0; i < n; i++) {
            int16_t v = (int16_t)lrint(amp * 32767.0 * sin(ph));
            ph += 2.0 * M_PI * hz / 44100.0;
            pk[4 * i] = (uint8_t)v; pk[4 * i + 1] = (uint8_t)((uint16_t)v >> 8);
            pk[4 * i + 2] = (uint8_t)v; pk[4 * i + 3] = (uint8_t)((uint16_t)v >> 8);
        }
        smp_usb(pk, n * 4u);
    }
}

/* the take: zero crossings (rising) per second, the peak, the RMS, over [a, b) */
static void measure(uint32_t a, uint32_t b, double *hz, int32_t *peak, double *rms)
{
    uint32_t i, up = 0;
    double s = 0;
    int32_t pk = 0, last = sp_sample(smp_buf, a);
    for (i = a; i < b; i++) {
        int32_t v = sp_sample(smp_buf, i);
        if (last < 0 && v >= 0) up++;
        last = v;
        if ((v < 0 ? -v : v) > pk) pk = v < 0 ? -v : v;
        s += (double)v * v;
    }
    *hz = up * (double)SMP_RATE / (b - a);
    *peak = pk;
    *rms = sqrt(s / (b - a));
}

static void reset(void)
{
    smp.state = SM_IDLE;
    smp.active = 1;
    smp.max10 = SMP_MAXN * 10u / SMP_RATE;
    smp_set_level(0);
    smp_set_thresh(0);
    smp.clip = 0;
}

int main(int argc, char **argv)
{
    double hz, rms, rms2;
    int32_t peak;
    uint32_t n, w;

    reset();                                            /* 1 kHz at -6 dB for 0.5 s, THRESH off: at once */
    smp_arm(5);
    check(smp.state == SM_ARMED && smp.pad == 5u, "REC arms the take for the pad");
    feed(1000, 0.5, 500);
    check(smp.state == SM_REC, "THRESH OFF: recording at once");
    smp_arm(5);                                         /* REC again: stop */
    check(smp.state == SM_DONE, "REC again stops it");
    w = smp_take();
    n = sp_wave[SMP_WAVE].n;
    check(w == SMP_WAVE && smp.state == SM_IDLE, "the take is a wave (SMP_WAVE)");
    check(n >= 13700u && n <= 13800u, "0.5 s at 27.5 kHz: ~13750 samples");
    check(sp_wave[SMP_WAVE].rate == SMP_RATE && sp_wave[SMP_WAVE].d == smp_buf, "its rate 27500, its data the RAM take");
    measure(1000, n - 100u, &hz, &peak, &rms);
    printf("     1 kHz: %.1f Hz, peak %d, rms %.1f\n", hz, peak, rms);
    check(fabs(hz - 1000.0) < 10.0, "the pitch kept (1 kHz in, 1 kHz out at 27.5 kHz)");
    check(peak > 980 && peak < 1060, "the level kept (-6 dB: ~1024 of 2048 in 12 bits)");

    reset();                                            /* the threshold: silence, then a hit */
    smp_set_thresh(30);
    smp_arm(0);
    feed(0, 0, 200);
    check(smp.state == SM_ARMED && smp.n == 0u, "THRESH: silence does not start it");
    feed(1000, 0.02, 100);
    check(smp.state == SM_ARMED, "THRESH 30: a whisper (-34 dB) does not start it");
    feed(1000, 0.8, 300);
    check(smp.state == SM_REC && smp.n >= 8200u && smp.n <= 8300u, "THRESH: starts at the hit (0.3 s taken, not 0.6)");
    smp_arm(0);
    smp_take();

    reset();                                            /* MAX: 0.2 s, then it stops by itself */
    smp.max10 = 2;
    smp_arm(1);
    feed(440, 0.5, 400);
    check(smp.state == SM_DONE && smp.n == 5500u, "MAX 0.2 s: stops at 5500 samples");
    smp_take();
    check(sp_wave[SMP_WAVE].n == 5500u, "MAX: the take 0.2 s long");

    reset();                                            /* LEVEL +6 dB: twice as loud; +24 dB: clips */
    smp_set_level(6);
    smp_arm(1);
    feed(1000, 0.25, 200);
    smp_arm(1);
    smp_take();
    measure(500, sp_wave[SMP_WAVE].n - 100u, &hz, &peak, &rms);
    check(peak > 980 && peak < 1060 && smp.clip == 0u, "LEVEL +6 dB: -12 dB in, -6 dB taken, no clip");
    reset();
    smp_set_level(24);
    smp_arm(1);
    feed(1000, 0.5, 50);
    smp_arm(1);
    smp_take();
    check(smp.clip > 0u, "LEVEL +24 dB on -6 dB: clipped (the meter shows it)");

    reset();                                            /* the input filter: 15 kHz loses more than 1 kHz */
    smp_arm(1);
    feed(1000, 0.5, 200);
    smp_arm(1);
    smp_take();
    measure(500, sp_wave[SMP_WAVE].n - 100u, &hz, &peak, &rms);
    reset();
    smp_arm(1);
    feed(15000, 0.5, 200);
    smp_arm(1);
    smp_take();
    measure(500, sp_wave[SMP_WAVE].n - 100u, &hz, &peak, &rms2);
    printf("     rms 1 kHz %.1f, 15 kHz %.1f (%.1f dB)\n", rms, rms2, 20.0 * log10(rms2 / rms));
    check(rms2 < rms * 0.71, "the input filter: 15 kHz at least 3 dB down");

    reset();                                            /* the monitor: the input on the output, ring kept in bounds */
    {
        static int32_t o[2 * 128];
        int32_t mx = 0;
        uint32_t i, k;
        smp.mon = 1;
        smp.mon_r = smp.mon_w;
        for (k = 0; k < 40u; k++) {                    /* 40 renders of 128 frames against 40 x 2.9 ms of packets */
            feed(1000, 0.5, 3);
            memset(o, 0, sizeof o);
            smp_monitor(o, 128);
            for (i = 0; i < 256u; i++) if ((o[i] < 0 ? -o[i] : o[i]) > mx) mx = o[i] < 0 ? -o[i] : o[i];
        }
        check(mx > 15000 && mx < 17000, "MON: the input heard (-6 dB) on both sides");
        check(smp.mon_w - smp.mon_r < SMP_MON_N, "MON: the ring never overflows");
        smp.mon = 0;
        feed(1000, 0.5, 3);
        memset(o, 0, sizeof o);
        smp_monitor(o, 128);
        check(o[0] == 0 && o[200] == 0, "MON OFF: silent");
    }

    reset();                                            /* the page closed: nothing taken, nothing heard */
    smp_arm(2);
    smp_close();
    check(smp.state == SM_IDLE && !smp.active, "closing the page drops an armed take");
    n = smp.pkts;
    feed(1000, 0.5, 10);
    check(smp.pkts == n + 10u && smp.n == 0u, "the page closed: packets counted, nothing recorded");
    smp.active = 1;
    smp_arm(2);
    feed(1000, 0.5, 10);
    smp_close();
    check(smp.state == SM_DONE && smp_take() == SMP_WAVE, "closing the page while recording keeps the take");

    if (argc > 1) {                                     /* a sweep 50 Hz .. 16 kHz, the take as a WAV */
        FILE *f = fopen(argv[1], "wb");
        uint32_t i, bytes;
        double f0 = 50;
        reset();
        smp_arm(0);
        for (i = 0; i < 1500u; i++) { feed(f0, 0.7, 1); f0 *= 1.0039; }
        smp_arm(0);
        smp_take();
        n = sp_wave[SMP_WAVE].n;
        bytes = n * 2u;
        fwrite("RIFF", 1, 4, f);
        { uint32_t v = 36u + bytes; fwrite(&v, 4, 1, f); }
        fwrite("WAVEfmt ", 1, 8, f);
        { uint32_t v[4] = {16, 1 | 1u << 16, SMP_RATE, SMP_RATE * 2u}; uint16_t b[2] = {2, 16}; fwrite(v, 4, 4, f); fwrite(b, 2, 2, f); }
        fwrite("data", 1, 4, f);
        fwrite(&bytes, 4, 1, f);
        for (i = 0; i < n; i++) { int16_t v = (int16_t)(sp_sample(smp_buf, i) * 16); fwrite(&v, 2, 1, f); }
        fclose(f);
        printf("     the take of a sweep: %s (%u samples)\n", argv[1], n);
    }
    printf("smp: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
