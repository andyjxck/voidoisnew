/* PineTime sensors: BMA425 accelerometer (step counting) and
   HRS3300 PPG (heart rate). Both share TWIM1 with the touch controller.

   BMA425: raw accel @100 Hz ±2g; steps via peak detection on the
   magnitude delta over a gravity baseline (wrist-mounted). The chip's
   built-in step engine (Bosch config blob) is a later upgrade.
   HRS3300: driver config and PPG engine ported from InfiniTime (see the
   HRS3300 section). */
#include <stdint.h>
#include <math.h>
#include "board.h"
#include "voidos/hal.h"

int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *dst, unsigned n);
int i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val);
int i2c_write_regs(uint8_t addr, uint8_t reg, const uint8_t *src, unsigned n);
uint32_t hal_millis_raw(void);

#define BMA_ADDR 0x18
#define HRS_ADDR 0x44

/* ---- BMA425 ------------------------------------------------------------ */

static int bma_ok;
static int bma_engine;          /* feature engine loaded ok */
static uint8_t bma_id, bma_stat, bma_feat;   /* diag: chip id, ASIC status, feature rd/wr ok */
static uint32_t steps;
static int16_t ax, ay, az;
static int32_t base;            /* gravity baseline, mg */
static int32_t act_delta;       /* last |mag - base|, for raise-to-wake
                                   and the heart-rate motion gate */
static int armed = 1;
static uint32_t last_step_ms;

#include "bma_cfg.h"      /* BMA425 stream (chip id 0x13) */
#include "bma423_cfg.h"   /* BMA423 stream: what InfiniTime loads into a
                             BMA421 (chip id 0x11) */
static uint8_t bma_blob;       /* diag: 0 none, 3 = BMA423 stream, 5 = BMA425 */

/* Feature block access the way Bosch's bma4.c does it: in 16-byte
   chunks, each preceded by the ASIC word index (start + offset / 2) in
   0x5B (low 4 bits) / 0x5C (the rest). */
static uint16_t bma_feat_base;
static int bma_feat_xfer(uint8_t *buf, unsigned len, int write)
{
    for (unsigned off = 0; off < len; off += 16) {
        unsigned idx = bma_feat_base + off / 2;
        unsigned n = len - off < 16 ? len - off : 16;
        if (!i2c_write_reg(BMA_ADDR, 0x5B, idx & 0x0F) ||
            !i2c_write_reg(BMA_ADDR, 0x5C, (idx >> 4) & 0xFF))
            return 0;
        if (write ? !i2c_write_regs(BMA_ADDR, 0x5E, buf + off, n)
                  : !i2c_read_reg(BMA_ADDR, 0x5E, buf + off, n))
            return 0;
    }
    return 1;
}

static void bma_init(void)
{
    uint8_t id = 0;
    i2c_write_reg(BMA_ADDR, 0x7E, 0xB6);      /* soft reset */
    hal_delay_ms(2);
    if (!i2c_read_reg(BMA_ADDR, 0x00, &id, 1) || !id)
        return;
    bma_id = id;                             /* chip id 0x11/0x13/... */
    i2c_write_reg(BMA_ADDR, 0x7C, 0x00);      /* adv power-save off */
    hal_delay_ms(1);

    /* Bosch feature engine: stream the config into internal RAM,
       then start the ASIC so features come alive. Register-level
       sequence mirrors Bosch's bma4_write_config_file. The stream must
       match the chip: 2.2.0 always sent the BMA425 stream, and the
       BMA421 (id 0x11) rejected it with INTERNAL_STATUS 3. InfiniTime
       sends the BMA423 stream to id 0x11 and the BMA425 one to 0x13. */
    const uint8_t *cfg = bma423_cfg;
    unsigned cfg_size = BMA423_CFG_SIZE;
    bma_blob = 3;
    if (id == 0x13) {
        cfg = bma_cfg;
        cfg_size = BMA_CFG_SIZE;
        bma_blob = 5;
    }
    i2c_write_reg(BMA_ADDR, 0x59, 0x00);      /* INIT_CTRL: halt loader */
    int stream_ok = 1;
    for (unsigned i = 0; i < cfg_size && stream_ok; i += 16) {
        stream_ok &= i2c_write_reg(BMA_ADDR, 0x5B, (i / 2) & 0x0F);
        stream_ok &= i2c_write_reg(BMA_ADDR, 0x5C, (i / 2) >> 4);
        unsigned n = cfg_size - i < 16 ? cfg_size - i : 16;
        stream_ok &= i2c_write_regs(BMA_ADDR, 0x5E, cfg + i, n);
    }
    if (!stream_ok)
        bma_feat |= 8;                        /* diag: a chunk was dropped */
    i2c_write_reg(BMA_ADDR, 0x59, 0x01);      /* start the ASIC */
    uint8_t stat = 0;
    for (int t = 0; t < 12; t++) {            /* poll up to ~600 ms */
        hal_delay_ms(50);
        i2c_read_reg(BMA_ADDR, 0x2A, &stat, 1);   /* INTERNAL_STAT */
        if ((stat & 0x0F) == 0x01)
            break;
    }
    bma_stat = stat;
    if ((stat & 0x0F) == 0x01) {              /* ASIC_INITIALIZED */
        /* enable on-chip features: byte 0x3B bit4 = step counter,
           byte 0x40 bit0 = wrist-wear (raise gesture) */
        uint8_t feat[70];
        /* Bosch reads the feature block's start index back from 0x5B/
           0x5C once the ASIC is up (get_feature_config_start_addr) */
        uint8_t lo = 0, hi = 0;
        i2c_read_reg(BMA_ADDR, 0x5B, &lo, 1);
        i2c_read_reg(BMA_ADDR, 0x5C, &hi, 1);
        bma_feat_base = (uint16_t)((lo & 0x0F) | (hi << 4));
        if (bma_feat_xfer(feat, sizeof feat, 0)) {
            bma_feat |= 1;
            feat[0x3B] |= 0x10;
            feat[0x40] |= 0x01;
            if (bma_feat_xfer(feat, sizeof feat, 1)) {
                bma_feat |= 2;
                bma_engine = 1;
                /* latch feature flags until read, and route wrist-wear
                   so a poll can never miss a short pulse */
                i2c_write_reg(BMA_ADDR, 0x55, 0x01);
                i2c_write_reg(BMA_ADDR, 0x56, 0x08);
            }
        }
    }

    i2c_write_reg(BMA_ADDR, 0x40, 0xA8);      /* CIC, avg4, 100Hz */
    i2c_write_reg(BMA_ADDR, 0x41, 0x00);      /* ±2g */
    hal_delay_ms(1);
    i2c_write_reg(BMA_ADDR, 0x7D, 0x04);      /* accel on */
    hal_delay_ms(50);
    bma_ok = 1;
}

/* raise-to-wake detector state — see wake_sample in bma_sample */
#define WH_N 8
static int16_t whx[WH_N], why[WH_N], whz[WH_N];
static int wh_idx, wh_n;

static int32_t isqrt(int32_t v)
{
    int32_t r = 0;
    for (int32_t bit = 0x40000000; bit; bit >>= 2) {
        int32_t t = r + bit;
        r >>= 1;
        if (v >= t) {
            v -= t;
            r += bit;
        }
    }
    return r;
}

static int eng_live;   /* the Bosch counter has counted at least once */
static void bma_sample(uint32_t now)
{
    uint8_t d[6];
    if (!i2c_read_reg(BMA_ADDR, 0x12, d, 6))
        return;
    int16_t x = (int16_t)(((d[1] << 8) | d[0]) >> 4);
    int16_t y = (int16_t)(((d[3] << 8) | d[2]) >> 4);
    int16_t z = (int16_t)(((d[5] << 8) | d[4]) >> 4);
    if (x > 2047) x -= 4096;      /* sign-extend 12-bit */
    if (y > 2047) y -= 4096;
    if (z > 2047) z -= 4096;
    /* mounted rotated: swap X/Y to match the watch axes */
    ax = y; ay = x; az = z;

    /* raise-to-wake history: the open PineTime firmware detects the
       gesture in software over raw axes — the chip's wrist-wear flag
       never arrived reliably on this unit. 8 samples at 10 Hz (their
       poll rate; we decimate our 50 Hz sampler by 5). */
    static uint32_t wh_last;
    if (now - wh_last >= 95) {     /* time-based: same 10 Hz whatever the
                                      sample rate (2.2.3 drops to 10 Hz
                                      while the screen is off) */
        wh_last = now;
        /* advance THEN write: wh_idx always points at the newest
           sample, matching the reference circular-buffer indexing */
        wh_idx = (wh_idx + 1) & (WH_N - 1);
        whx[wh_idx] = x;               /* RAW axes: their frame */
        why[wh_idx] = y;
        whz[wh_idx] = z;
        if (wh_n < WH_N)
            wh_n++;
    }

    int32_t mag = isqrt((int32_t)ax * ax + (int32_t)ay * ay +
                        (int32_t)az * az);
    if (!base)
        base = mag;
    base += (mag - base) >> 5;
    int32_t delta = mag - base;
    act_delta = delta < 0 ? -delta : delta;
    if (delta < 60)
        armed = 1;

    /* two counters run side by side: the Bosch engine (when its blob
       uploaded) and our crude delta counter. If the engine's feature
       enable silently failed, its counter just reads 0 forever — the
       old code trusted it anyway and showed 0 steps all day. Prefer
       the engine while it actually counts; fall back otherwise. */
    static uint32_t fb_steps;
    if (armed && delta > 220 && now - last_step_ms > 300) {
        fb_steps++;
        armed = 0;
        last_step_ms = now;
    }
    /* the engine's counter only changes a step at a time: read it once
       a second, not on every 20 ms sample (that was a second I2C burst
       50 times a second, screen on or off) */
    static uint32_t last_eng;
    if (bma_engine && (now - last_eng >= 1000 || !last_eng)) {
        last_eng = now ? now : 1;
        uint8_t s[4];
        if (i2c_read_reg(BMA_ADDR, 0x1E, s, 4)) {
            uint32_t eng = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                           ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
            steps = eng > 0 ? eng : fb_steps;
            eng_live = eng > 0;
        }
    } else if (!bma_engine) {
        steps = fb_steps;
    }
}

/* ---- HRS3300 ----------------------------------------------------------- */

/* Driver and PPG engine follow InfiniTime (src/drivers/Hrs3300.cpp,
   src/components/heartrate/Ppg.cpp, src/heartratetask/HeartRateTask.cpp)
   register for register and step for step, because that code is what
   actually reads a pulse on PineTime hardware.

   What changed from 1.6.2 and why:
   - config was 16-bit ADC, 8x gain, 12.5 ms wait. On skin with the
     LED at 12.5 mA that rails the ADC, so the pulse (a tiny AC ripple)
     flattens out and the spectrum never passes the gates. InfiniTime
     runs 15-bit, 1x gain, 50 ms wait: one fresh sample every ~100 ms.
   - the ambient channel was subtracted from the HRS channel. The two
     channels use different gain/resolution, so the difference is not
     meaningful; InfiniTime only uses ALS to detect the watch coming
     off (ALS jumping above 2x the last good level).
   - Disable() now also turns the LED driver off (PDRIVER = 0).
   - sampling is a fixed 10 Hz grid matching the sensor's cycle, and
     the analysis is InfiniTime's: detrend + differentiate, EMA
     band-pass, Hann, FFT, 2-frame spectral average, SNR > 3 and
     DC < 0.5 gates, single-peak search, 20-frame BPM average.

   2.2 ("only tracks when I'm still"): InfiniTime's HandleSensorData
   treats ONE sample of ambient light as the watch coming off and wipes
   everything, and feeds wrist-motion garbage straight into the FFT. On
   the wrist both happen all the time, so the face kept dropping to a
   dash and needed a full 6.4 s window to come back. Now:
   - the last good BPM is held for HR_HOLD_MS after the last fresh
     spectral peak and reported as stale (greyed) instead of 0;
   - ambient light only means off-wrist after HR_AMB_OFF_N consecutive
     samples (1.5 s). Blips up to HR_AMB_PATCH_N samples are bridged
     with the last good sample; longer ones restart the window without
     touching the held value;
   - a lost lock (-1) restarts the estimator but keeps the window and
     the held value;
   - while the accelerometer sees the wrist moving, no samples are fed
     (the window would only hold arm swing); acquisition restarts
     cleanly once the wrist has been still for HR_STILL_MS. */

#define HRS_REG_ENABLE  0x01
#define HRS_REG_PDRIVER 0x0C
#define HRS_REG_RES     0x16
#define HRS_REG_HGAIN   0x17
#define HRS_PDRIVER_ON  0x2f   /* 12.5 mA, steady drive low nibble */
#define PIN_HRS_INT     30

static int hrs_ok;
static int32_t hrs_raw, hrs_als;         /* last raw samples, for diag */
static int32_t dc, ac_f;                 /* DC estimate, filtered AC (trace) */
static uint32_t beats;                   /* analysis frames run (diag) */
static uint32_t last_ibi;                /* diag: last spectrum SNR x10 */
static int bpm_valid, bpm;
static int32_t win_max;                  /* |ac| max over the last 2 s (diag) */
static int32_t win_cur;
static uint32_t win_ms;

/* manual switch (Settings / control centre) */
static int hr_on = 1;
static int hrs_powered;                /* mirrors ENABLE bit7 writes */

static void hrs_power(int on)
{
    uint8_t v = 0x50;
    i2c_read_reg(HRS_ADDR, HRS_REG_ENABLE, &v, 1);
    if (on) {
        i2c_write_reg(HRS_ADDR, HRS_REG_ENABLE, v | 0x80);
        i2c_write_reg(HRS_ADDR, HRS_REG_PDRIVER, HRS_PDRIVER_ON);
    } else {
        i2c_write_reg(HRS_ADDR, HRS_REG_ENABLE, v & ~0x80);
        i2c_write_reg(HRS_ADDR, HRS_REG_PDRIVER, 0);
    }
}

/* raw PPG ring buffer for the ECG trace (10 Hz, 12.8 s of signal) */
#define PPG_N 128
static int16_t ppg[PPG_N];
static int ppg_head;
static int16_t ppg_lo = 32767, ppg_hi = -32768;

#define HR_N        64                 /* dataLength */
#define HR_SPEC     (HR_N / 2)         /* spectrumLength */
#define HR_FS       10.0f              /* 1000 / deltaTms */
#define HR_RES      (HR_FS / HR_N)     /* freqResolution, Hz per bin */
#define HR_ROI_BEG  3                  /* round(0.5 Hz / res) */
#define HR_ROI_END  26                 /* round(4.0 Hz / res) */
#define HR_OVERLAP  5
#define HR_AVG_N    20
static uint16_t daq[HR_N];
static int daq_n, daq_enough;
static float hr_fs = HR_FS;            /* diag only */
static float vre[HR_N], vim[HR_N];
static float spec[HR_SPEC];
static int spec_n, spec_reset = 1;
static float hra[HR_AVG_N];
static int hra_i;
static float last_peak;
static uint32_t als_thr = 0xFFFF;
static uint16_t als_val;
static float last_snr;
static int last_pk_bpm;      /* diag: last frame peak, whether accepted or not */
static uint32_t next_daq_ms;

#define HR_HOLD_MS     60000u  /* show the last good BPM, greyed, this long */
#define HR_AMB_OFF_N   15      /* 1.5 s of light in a row = off the wrist */
#define HR_AMB_PATCH_N 3       /* blips up to 300 ms are bridged */
#define HR_MOVE_MG     200     /* |accel| swing over gravity that pauses HR */
#define HR_STILL_MS    2000u   /* quiet time before acquisition resumes */
#define HR_ALS_FLOOR   32      /* a near-dark ALS mustn't make thr ~0 */

static int hr_fresh;          /* the last frame found a spectral peak */
static uint32_t bpm_good_ms;  /* time of the last fresh peak */
static int amb_run;           /* consecutive samples above als_thr */
static int hr_gap;            /* window would hold a gap: restart when clean */
static uint16_t last_fed;     /* last sample that went into the window */
static uint32_t moved_ms;     /* last time the wrist moved hard */
static int moved_seen;
static uint32_t hr_holds;     /* diag: times the display went to hold */

static void ppg_reset(int daq_too)
{
    if (daq_too) {
        daq_n = 0;
        daq_enough = 0;
        amb_run = 0;
    }
    hra_i = 0;
    for (int i = 0; i < HR_AVG_N; i++)
        hra[i] = 0;
    last_peak = 0;
    als_thr = 0xFFFF;
    als_val = 0;
    spec_reset = 1;
    for (int i = 0; i < HR_SPEC; i++)
        spec[i] = 0;
}

static void dft64(float *re, float *im)
{
    for (int i = 1, j = 0; i < HR_N; i++) {
        int bit = HR_N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= HR_N; len <<= 1) {
        int half = len >> 1;
        for (int i = 0; i < HR_N; i += len) {
            for (int k = 0; k < half; k++) {
                float w_ang = -6.2831853f * k / len;
                float wr = cosf(w_ang), wi = sinf(w_ang);
                int a = i + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

static const float hann[HR_N / 2] = {
    0.0f,        0.00248461f, 0.00991376f, 0.0222136f,  0.03926189f,
    0.06088921f, 0.08688061f, 0.11697778f, 0.15088159f, 0.1882551f,
    0.22872687f, 0.27189467f, 0.31732949f, 0.36457977f, 0.41317591f,
    0.46263495f, 0.51246535f, 0.56217185f, 0.61126047f, 0.65924333f,
    0.70564355f, 0.75f,       0.79187184f, 0.83084292f, 0.86652594f,
    0.89856625f, 0.92664544f, 0.95048443f, 0.96984631f, 0.98453864f,
    0.99441541f, 0.99937846f
};

static float lin_interp(const float *y, int n, float x)
{
    /* x values are the bin indices 0..n-1 */
    if (x > (float)(n - 1))
        return y[n - 1];
    if (x <= 0.0f)
        return y[0];
    int i = 0;
    while (x > (float)i && i < n - 1)
        i++;
    float mu = x - (float)(i - 1);
    return y[i - 1] * (1 - mu) + y[i] * mu;
}

/* exactly one region above threshold in [start, end): its centre, or 0 */
static float peak_search(const float *y, float thr, float *width,
                         float start, float end, int n)
{
    int peaks = 0, enabled = 0;
    float min_bin = 0, max_bin = 0, centre = 0;
    float prev = lin_interp(y, n, start - 0.01f);
    float cur = lin_interp(y, n, start);
    float idx = start;
    while (idx < end) {
        float next = lin_interp(y, n, idx + 0.01f);
        if (cur < thr)
            enabled = 1;
        if (cur >= thr && enabled) {
            if (prev < thr) {
                min_bin = idx;
            } else if (next <= thr) {
                max_bin = idx;
                peaks++;
                *width = max_bin - min_bin;
                centre = *width / 2.0f + min_bin;
            }
        }
        prev = cur;
        cur = next;
        idx += 0.01f;
    }
    if (peaks != 1) {
        *width = 0;
        centre = 0;
    }
    return centre;
}

/* Ppg::ProcessHeartRate: -1 reset acquisition, 0 no HR, else BPM */
static int hr_process(int init)
{
    for (int i = 0; i < HR_N; i++) {
        vre[i] = (float)daq[i];
        vim[i] = 0;
    }
    /* detrend, then first difference */
    {
        float off = vre[0];
        float slope = (vre[HR_N - 1] - off) / (float)(HR_N - 1);
        for (int i = 0; i < HR_N; i++)
            vre[i] -= slope * (float)i + off;
        for (int i = 0; i < HR_N - 1; i++)
            vre[i] = vre[i + 1] - vre[i];
    }
    /* EMA band-pass, ~0.5..4 Hz at 10 Hz */
    {
        float a = 0.816f, avg;
        for (int l = 0; l < 4; l++) {
            avg = vre[0];
            for (int i = 0; i < HR_N; i++) {
                avg = a * vre[i] + (1 - a) * avg;
                vre[i] = avg;
            }
        }
        a = 0.268f;
        for (int l = 0; l < 4; l++) {
            avg = vre[0];
            for (int i = 0; i < HR_N; i++) {
                avg = a * vre[i] + (1 - a) * avg;
                vre[i] -= avg;
            }
        }
    }
    for (int i = 0, h = 0; i < HR_N; i++) {
        if (i >= HR_N / 2)
            h--;
        vre[i] *= hann[h];
        if (i < HR_N / 2)
            h++;
    }
    dft64(vre, vim);
    /* spectral average over up to 2 frames */
    if (init)
        spec_n = 0;
    for (int k = 0; k < HR_SPEC; k++) {
        float m = sqrtf(vre[k] * vre[k] + vim[k] * vim[k]);
        spec[k] = (spec[k] * spec_n + m) / (spec_n + 1);
    }
    if (spec_n < 2)
        spec_n++;

    float mx = 0, mean = 0;
    int pk = HR_ROI_BEG;
    for (int k = HR_ROI_BEG; k < HR_ROI_END; k++) {
        mean += spec[k];
        if (spec[k] > mx) {
            mx = spec[k];
            pk = k;
        }
    }
    mean /= (float)(HR_ROI_END - HR_ROI_BEG);
    float snr = mean > 0 ? mx / mean : 0;
    last_snr = snr;
    last_pk_bpm = (int)(pk * HR_RES * 60.0f + 0.5f);

    float loc = 0, width = 0;
    if (snr > 3.0f && spec[0] < 0.5f)
        loc = peak_search(spec, 0.6f * mx, &width, (float)HR_ROI_BEG,
                          (float)HR_ROI_END, HR_SPEC) * HR_RES;
    if (width > 2.5f)
        loc = 0;
    if (loc < 40.0f / 60.0f || loc > 230.0f / 60.0f)
        loc = 0;
    if (loc == 0)
        spec_reset = 1;
    hr_fresh = loc > 0;

    als_thr = (uint32_t)(als_val * 2.0f);
    if (als_thr < HR_ALS_FLOOR)
        als_thr = HR_ALS_FLOOR;

    /* 20-frame average of the non-zero readings */
    hra_i = (hra_i + 1) % HR_AVG_N;
    hra[hra_i] = loc;
    float sum = 0;
    int cnt = 0;
    for (int i = 0; i < HR_AVG_N; i++)
        if (hra[i] > 0) {
            sum += hra[i];
            cnt++;
        }
    float avg = cnt ? sum / cnt : 0;
    if (avg == 0 && last_peak > 0) {
        last_peak = 0;
        return -1;
    }
    last_peak = avg;
    return (int)(avg * 60.0f + 0.5f);
}

/* Ppg::HeartRate: -2 not enough data yet, else as hr_process */
static int hr_rate(void)
{
    if (daq_n < HR_N)
        return daq_enough ? 0 : -2;
    daq_enough = 1;
    int r = hr_process(spec_reset);
    spec_reset = 0;
    for (int i = 0; i < HR_N - HR_OVERLAP; i++)
        daq[i] = daq[i + HR_OVERLAP];
    daq_n = HR_N - HR_OVERLAP;
    beats++;
    last_ibi = (uint32_t)(last_snr * 10);
    return r;
}

static void hrs_init(void)
{
    uint8_t id;
    /* INT pin as plain input, like InfiniTime; we poll, no IRQ */
    GPIO_PIN_CNF(PIN_HRS_INT) = 0;
    if (!i2c_read_reg(HRS_ADDR, 0x00, &id, 1))
        return;
    hrs_power(0);
    hal_delay_ms(100);
    /* HRS off, 50 ms wait between conversions, LED 12.5 mA */
    i2c_write_reg(HRS_ADDR, HRS_REG_ENABLE, 0x50);
    i2c_write_reg(HRS_ADDR, HRS_REG_PDRIVER, HRS_PDRIVER_ON);
    /* HRS and ALS 15-bit: ~50 ms LED/ADC period */
    i2c_write_reg(HRS_ADDR, HRS_REG_RES, 0x77);
    /* gain 1x */
    i2c_write_reg(HRS_ADDR, HRS_REG_HGAIN, 0x00);
    hrs_powered = 0;           /* sensors_tick enables it when hr_on */
    ppg_reset(1);
    hrs_ok = 1;
}

/* restart acquisition, keeping the learned ALS threshold (a soft reset
   mustn't blind the off-wrist check for a whole frame) */
static void hr_restart(int daq_too)
{
    uint32_t thr = als_thr;
    uint16_t av = als_val;
    ppg_reset(daq_too);
    als_thr = thr;
    als_val = av;
}

/* stop calling the reading live; sensors_tick drops it after the hold */
static void hr_hold(void)
{
    if (bpm_valid) {
        bpm_valid = 0;
        hr_holds++;
    }
}

static void hrs_sample(uint32_t now)
{
    /* registers 0x08..0x0F: C1DATAM C0DATAM C0DATAH PDRIVER C1DATAH
       C1DATAL C0DATAL ... read in one burst */
    uint8_t d[8];
    if (!i2c_read_reg(HRS_ADDR, 0x08, d, 8))
        return;
    uint32_t hrs = ((uint32_t)d[1] << 8) | ((uint32_t)(d[2] & 0x0f) << 4) |
                   (d[7] & 0x0f);
    uint32_t als = ((uint32_t)(d[5] & 0x3f) << 11) | ((uint32_t)d[0] << 3) |
                   (d[6] & 0x07);
    hrs_raw = (int32_t)hrs;
    hrs_als = (int32_t)als;

    /* ECG trace: DC-removed, lightly smoothed */
    if (dc == 0)
        dc = (int32_t)hrs << 4;
    dc += (((int32_t)hrs << 4) - dc) >> 4;
    int32_t ac = (int32_t)hrs - (dc >> 4);
    ac_f += (ac - ac_f) >> 1;
    if (now - win_ms >= 2000) {
        win_max = win_cur;
        win_cur = 0;
        win_ms = now;
    }
    {
        int32_t a = ac_f < 0 ? -ac_f : ac_f;
        if (a > win_cur)
            win_cur = a;
    }
    int16_t v = (int16_t)(ac_f > 32700 ? 32700 : ac_f < -32700 ? -32700 : ac_f);
    ppg[ppg_head] = v;
    ppg_head = (ppg_head + 1) % PPG_N;
    ppg_lo = 32767;
    ppg_hi = -32768;
    for (int i = 0; i < PPG_N; i++) {
        if (ppg[i] < ppg_lo) ppg_lo = ppg[i];
        if (ppg[i] > ppg_hi) ppg_hi = ppg[i];
    }

    /* ambient light: debounced off-wrist decision */
    int ambient = als > als_thr;
    if (!ambient)
        amb_run = 0;
    else if (amb_run < HR_AMB_OFF_N)
        amb_run++;
    if (amb_run >= HR_AMB_OFF_N) {
        /* light for 1.5 s straight: really off the wrist. Full reset
           (re-learns the ALS level) and the dash shows right away. */
        ppg_reset(1);
        amb_run = 0;
        hr_gap = 0;
        bpm_valid = 0;
        bpm = 0;
        return;
    }
    int moving = moved_seen && now - moved_ms < HR_STILL_MS;
    if (moving || amb_run > HR_AMB_PATCH_N) {
        /* arm swing or a longer light leak: feeding it would only teach
           the FFT garbage. Pause, hold the reading greyed, restart the
           window once the signal is clean again. */
        hr_gap = 1;
        hr_hold();
        return;
    }
    if (hr_gap) {
        hr_restart(1);
        hr_gap = 0;
    }

    /* Ppg::Preprocess. A short blip is bridged with the last good
       sample so the 10 Hz grid stays intact. */
    uint16_t s = ambient && daq_n ? last_fed : (uint16_t)hrs;
    if (daq_n < HR_N)
        daq[daq_n++] = s;
    last_fed = s;
    if (!ambient)
        als_val = (uint16_t)als;     /* thr only learns from clean light */

    int r = hr_rate();
    if (r == -1) {
        /* lost lock: restart the estimator, keep window + held value */
        hr_restart(0);
        hr_hold();
    } else if (r > 0) {
        bpm = r;
        bpm_valid = 1;
        if (hr_fresh)
            bpm_good_ms = now;
    }
}

/* ---- public -------------------------------------------------------------- */

void sensors_init(void)
{
    bma_init();
    hrs_init();
}

static unsigned hrs_retries, bma_retries;

/* 2.2.3 battery: the HR LED ran 24/7 (screen off, off the wrist, on the
   charger) at 12.5 mA pulses with a ~50 % duty cycle. Now it is live
   only while the screen is on or a workout runs; with the screen off it
   takes a background reading every HR_BG_PERIOD_MS (stops early once a
   reading locks, gives up after HR_BG_MAX_MS) and none on the charger.
   The manual HR switch still turns it off completely. */
#define HR_BG_PERIOD_MS 300000u
#define HR_BG_MAX_MS    60000u
#define HR_BG_MIN_MS    15000u
static int hr_live = 1, scr_on = 1;
static int hr_bg_on;
static uint32_t hr_bg_start, hr_bg_next;
void sensors_set_mode(int screen_on, int hr_busy)
{
    scr_on = screen_on;
    hr_live = screen_on || hr_busy;
}
int sensors_hr_powered(void) { return hrs_powered; }

static int hr_want(uint32_t raw)
{
    if (!hr_on)
        return 0;
    if (hr_live) {
        hr_bg_on = 0;
        hr_bg_next = raw + HR_BG_PERIOD_MS;
        return 1;
    }
    if (hal_on_charger()) {
        hr_bg_on = 0;
        return 0;
    }
    if (!hr_bg_on && (int32_t)(raw - hr_bg_next) >= 0) {
        hr_bg_on = 1;
        hr_bg_start = raw;
    }
    if (hr_bg_on) {
        uint32_t el = raw - hr_bg_start;
        int got = bpm_valid && (int32_t)(bpm_good_ms - hr_bg_start) > 0;
        if (el >= HR_BG_MAX_MS || (got && el >= HR_BG_MIN_MS)) {
            hr_bg_on = 0;
            hr_bg_next = raw + HR_BG_PERIOD_MS;
        }
    }
    return hr_bg_on;
}

void sensors_tick(uint32_t now)
{
    static uint32_t last_bma, last_retry;
    /* a sensor that failed to answer at boot (bus glitch, slow power-up)
       used to stay dead until reboot; retry every 5 s */
    if ((!hrs_ok || !bma_ok || (!bma_engine && bma_retries < 3)) &&
        now - last_retry >= 5000) {
        last_retry = now;
        if (!hrs_ok) {
            hrs_retries++;
            hrs_init();
        }
        if (!bma_ok || !bma_engine) {
            bma_retries++;
            bma_init();              /* re-stream the engine, max 3 tries */
        }
    }
    /* use the unlatched clock: hal_millis() is frozen for a whole
       render frame. */
    uint32_t raw = hal_millis_raw();
    /* 50 Hz while the screen is on, HR is measuring (motion gate) or the
       software step fallback is counting; 10 Hz otherwise — raise-to-wake
       only needs its 10 Hz history and the Bosch engine counts steps on
       the chip by itself */
    uint32_t bma_iv = (!scr_on && bma_engine && eng_live && !hrs_powered) ? 100 : 20;
    if (bma_ok && now - last_bma >= bma_iv) {
        last_bma = now;
        bma_sample(now);
        if (act_delta > HR_MOVE_MG) {        /* HR motion gate */
            moved_ms = raw;
            moved_seen = 1;
        }
    }
    /* Samples sit on a fixed 100 ms grid, the sensor's own conversion
       period (InfiniTime: deltaTms = 100). A late loop takes the sample
       as soon as it can; a long stall (>400 ms) would smear the
       spectrum, so the window restarts instead (the reading is held:
       before 2.2 it stayed "live" with no estimator behind it). */
    if (hrs_ok && hr_on && hrs_powered && (int32_t)(raw - next_daq_ms) >= 0) {
        if ((int32_t)(raw - next_daq_ms) > 400) {
            hr_restart(1);
            hr_hold();
            next_daq_ms = raw;
        }
        next_daq_ms += 100;
        hrs_sample(raw);
    }
    /* held reading expires a minute after the last fresh peak */
    if (!bpm_valid && bpm > 0 && raw - bpm_good_ms >= HR_HOLD_MS)
        bpm = 0;
    /* power writes live here, not in sensors_hr_set: a blocking i2c
       transaction from inside an LVGL tap handler was the "crashed
       when I toggled heart rate" path — it ran mid-event-dispatch */
    int want = hrs_ok && hr_want(raw);
    if (hrs_ok && !want && hrs_powered) {
        hrs_power(0);                   /* off or between readings: dark */
        hrs_powered = 0;
        if (!hr_on) {                   /* manual off drops the reading */
            bpm_valid = 0;
            bpm = 0;
        } else {
            hr_hold();                  /* duty-cycled: hold it, greyed */
        }
        ppg_reset(1);
    } else if (want && !hrs_powered) {
        hrs_power(1);
        hrs_powered = 1;
        ppg_reset(1);
        dc = ac_f = 0;
        next_daq_ms = raw + 100;
    }
}

void sensors_hr_set(int on)
{
    hr_on = on ? 1 : 0;    /* tick does the actual power write and resets */
}
int sensors_hr_on(void) { return hr_on; }

int sensors_bma_ok(void)  { return bma_ok; }
int sensors_bma_z(void)   { return bma_ok ? az : 0; }
int sensors_bma_activity(void) { return bma_ok ? (int)act_delta : 0; }

/* asin in degrees: 1024 counts = 1 g, clamp like the reference impl */
static float wake_asin(int32_t v)
{
    float a = v / 1024.0f;
    if (a > 1.0f) a = 1.0f;
    if (a < -1.0f) a = -1.0f;
    return asinf(a) * 57.2957795f;
}

/* roll angle change between two (y,z) vectors, only meaningful when
   the accel reads pure gravity — the reference DegreesRolled */
static int deg_rolled(int32_t y, int32_t z, int32_t py, int32_t pz)
{
    int pa = (int)wake_asin(py), ya = (int)wake_asin(y);
    if (z < 0 && pz < 0)
        return ya - pa;
    if (pz < 0)
        return y < 0 ? -pa - ya - 180 : -pa - ya + 180;
    if (z < 0)
        return y < 0 ? pa + ya + 180 : pa + ya - 180;
    return pa - ya;
}

/* chip's wrist-wear flag when the engine loaded; kept as a bonus
   trigger alongside the software detector */
int sensors_wrist_raise(void)
{
    if (!bma_engine)
        return 0;
    /* polled from every main-loop pass while the screen is off; 10 Hz
       is plenty for a raise and saves ~30 I2C reads a second */
    static uint32_t last_rd;
    uint32_t t = hal_millis_raw();
    if (t - last_rd < 100)
        return 0;
    last_rd = t;
    uint8_t v = 0;
    if (!i2c_read_reg(BMA_ADDR, 0x1C, &v, 1))
        return 0;
    return (v & 0x08) ? 1 : 0;
}

/* software raise-to-wake, ported from the reference watch firmware:
   newest-2 vs oldest-2 means of an 8-sample 10 Hz history, low
   variance (gravity only), arm roughly down (the "up" axis' mean in a
   band), and a >45 deg roll of the plane between them.
   The reference runs it on the y/z plane — on this board the
   display-mapped frame swaps X/Y, so the same test is run on x/z
   too and the roll sign is accepted either way: axis/sign mix-ups
   used to make the gesture unreachable on some units. */
static int raise_check(const int16_t *hu, const int16_t *hz,
                       const int16_t *lateral)
{
    int i0 = wh_idx,                       /* newest */
        i1 = (wh_idx + 7) & 7,             /* 1 back */
        o0 = (wh_idx + 1) & 7,             /* oldest (~0.7 s ago) */
        o1 = (wh_idx + 2) & 7;             /* 2nd oldest */
    int32_t lm = ((int32_t)lateral[i0] + lateral[i1]) / 2;
    int32_t ym = ((int32_t)hu[i0] + hu[i1]) / 2;
    int32_t zm = ((int32_t)hz[i0] + hz[i1]) / 2;
    int32_t pym = ((int32_t)hu[o0] + hu[o1]) / 2;
    int32_t pzm = ((int32_t)hz[o0] + hz[o1]) / 2;
    int32_t d = hu[i0] - ym;
    int32_t yv = d * d;
    d = hu[i1] - ym;
    yv = (yv + d * d) / 2;
    d = hz[i0] - zm;
    int32_t zv = d * d;
    d = hz[i1] - zm;
    zv = (zv + d * d) / 2;

    if (lm > 384 || lm < -384)
        return 0;
    if (yv > 56 * 56)
        return 0;
    if (ym < -724 && zv > 56 * 56)
        return 0;
    if (ym > -64)
        return 0;
    int roll = deg_rolled(ym, zm, pym, pzm);
    return roll < -45 || roll > 45;
}

int sensors_raise_wake(void)
{
    if (!bma_ok || wh_n < WH_N)
        return 0;
    /* y/z plane (reference orientation) and x/z plane (this board's
       swapped mount) — either one waking is good enough */
    return raise_check(why, whz, whx) || raise_check(whx, whz, why);
}
int sensors_bma_engine(void) { return bma_engine; }
int sensors_bma_dbg(void) { return ((int)bma_blob << 24) | (bma_id << 16) | (bma_stat << 8) | bma_feat; }
int sensors_hrs_fs10(void) { return (int)(hr_fs * 10.0f); }
int sensors_retries(void) { return (int)(hrs_retries * 100 + bma_retries); }
int sensors_hrs_pk(void) { return last_pk_bpm; }
int sensors_hrs_snr10(void) { return (int)(last_snr * 10); }
int sensors_hrs_windows(void) { return (int)beats; }
int sensors_hrs_ok(void)  { return hrs_ok; }
int sensors_hrs_raw(void)   { return (int)hrs_raw; }
int sensors_hrs_als(void)   { return (int)hrs_als; }
int sensors_hrs_ac(void)    { return (int)win_max; }
int sensors_hrs_beats(void) { return (int)beats; }
int sensors_hrs_ibi(void)   { return (int)last_ibi; }
int sensors_steps(void)   { return bma_ok ? (int)steps : -1; }
/* live or held BPM; sensors_bpm_stale() says which */
int sensors_bpm(void)
{
    if (!hrs_ok) return -1;   /* no sensor */
    return bpm > 0 ? bpm : 0; /* 0 = measuring / off the wrist */
}
/* 1 while the BPM shown is the held last good reading (lost lock,
   moving, light leak), not a live one */
int sensors_bpm_stale(void) { return hrs_ok && bpm > 0 && !bpm_valid; }
int sensors_hr_moving(void)
{
    return moved_seen && hal_millis_raw() - moved_ms < HR_STILL_MS;
}
int sensors_hr_holds(void) { return (int)hr_holds; }

/* ECG trace data: sample i (0..PPG_N-1) oldest→newest, normalised -1..1.
   Real signal whenever there's measurable amplitude — a missing BPM
   lock mustn't hide live data behind the fake waveform. */
int sensors_ppg(int i, int *out_y100)
{
    if (!hrs_ok)
        return 0;
    int idx = (ppg_head + i) % PPG_N;
    int32_t range = ppg_hi - ppg_lo;
    if (range < 40)
        return 0;
    *out_y100 = (int)((ppg[idx] - ppg_lo) * 200 / range - 100);
    return 1;
}
