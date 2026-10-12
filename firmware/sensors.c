/* PineTime sensors: BMA425 accelerometer (step counting) and
   HRS3300 PPG (heart rate). Both share TWIM1 with the touch controller.

   BMA425: raw accel @100 Hz ±2g; steps via peak detection on the
   magnitude delta over a gravity baseline (wrist-mounted). The chip's
   built-in step engine (Bosch config blob) is a later upgrade.
   HRS3300: init sequence follows the wasp-os/InfiniTime-derived register
   values (datasheet); BPM via spectral analysis (DFT peak in the HR
   band) on the raw optical signal. */
#include <stdint.h>
#include <math.h>
#include "board.h"
#include "voidos/hal.h"

int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *dst, unsigned n);
int i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val);
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
static int32_t act_delta;       /* last |mag - base|, for raise-to-wake */
static int armed = 1;
static uint32_t last_step_ms;

#include "bma_cfg.h"

int i2c_write_regs(uint8_t addr, uint8_t reg, const uint8_t *src, unsigned n);

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
       sequence mirrors the published sensor driver. */
    i2c_write_reg(BMA_ADDR, 0x59, 0x00);      /* INIT_CTRL: halt loader */
    int stream_ok = 1;
    for (unsigned i = 0; i < BMA_CFG_SIZE && stream_ok; i += 16) {
        stream_ok &= i2c_write_reg(BMA_ADDR, 0x5B, (i / 2) & 0x0F);
        stream_ok &= i2c_write_reg(BMA_ADDR, 0x5C, (i / 2) >> 4);
        unsigned n = BMA_CFG_SIZE - i < 16 ? BMA_CFG_SIZE - i : 16;
        stream_ok &= i2c_write_regs(BMA_ADDR, 0x5E, bma_cfg + i, n);
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
        /* the feature block lives at RAM index 0: re-point the index
           registers, which the stream loop left at the last chunk */
        i2c_write_reg(BMA_ADDR, 0x5B, 0x00);
        i2c_write_reg(BMA_ADDR, 0x5C, 0x00);
        if (i2c_read_reg(BMA_ADDR, 0x5E, feat, 70)) {
            bma_feat |= 1;
            feat[0x3B] |= 0x10;
            feat[0x40] |= 0x01;
            i2c_write_reg(BMA_ADDR, 0x5B, 0x00);
            i2c_write_reg(BMA_ADDR, 0x5C, 0x00);
            if (i2c_write_regs(BMA_ADDR, 0x5E, feat, 70)) {
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
    static int wh_decim;
    if (++wh_decim >= 5) {
        wh_decim = 0;
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
    if (bma_engine) {
        uint8_t s[4];
        if (i2c_read_reg(BMA_ADDR, 0x1E, s, 4)) {
            uint32_t eng = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                           ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
            steps = eng > 0 ? eng : fb_steps;
        }
    } else {
        steps = fb_steps;
    }
}

/* ---- HRS3300 ----------------------------------------------------------- */

static int hrs_ok;
static int32_t hrs_raw, hrs_als;         /* last raw samples, for diag */
static int32_t dc, ac_f;                 /* DC estimate, filtered AC */
static uint32_t beats;                   /* analysis windows run (diag) */
static uint32_t last_ibi;                /* diag: last spectrum SNR x10 */
static int bpm_valid, bpm;
static int32_t win_max, prev_win;        /* |ac| max: current/last 2 s window */
static uint32_t win_ms;

/* manual switch (Settings). The auto park/probe state machine from
   the cloud port is gone: it parked live wrists on-device and made HR
   unusable. Off-wrist detection still just clears bpm_valid; power
   saving comes later once the detection is proven reliable. */
static int hr_on = 1;
static int hrs_powered = 1;            /* mirrors reg 0x01 bit7 writes */

static void hrs_power(int on)
{
    i2c_write_reg(HRS_ADDR, 0x01, on ? (0x60 | 0x80) : 0x60);
}

/* raw PPG ring buffer for the ECG trace (50 Hz, ~2.5 s of signal) */
#define PPG_N 128
static int16_t ppg[PPG_N];
static int ppg_head;
static int16_t ppg_lo = 32767, ppg_hi = -32768;

/* Spectral BPM: 64 samples @10 Hz -> detrend/differentiate -> EMA
   bandpass -> Hann -> DFT -> dominant bin in the 30-240 BPM region
   with an SNR gate -> rolling average. Same approach as the proven
   open-source PineTime stack; our own implementation of the maths. */
#define HR_N   64
#define HR_SPEC (HR_N / 2)
static uint16_t daq[HR_N];
static int daq_n;
static uint32_t daq_t[HR_N], last_daq_ms;
static float hr_fs = 10.0f;     /* measured sample rate of the window */
static float vre[HR_N], vim[HR_N];
static float spec[HR_SPEC];
static int spec_n, spec_reset = 1;
static float hra[20];
static int hra_i;
static float last_snr;
static float cand_hz[8];
static int cand_n;
static int bad_frames;       /* consecutive rejected frames (holdover) */
static int last_pk_bpm;      /* diag: raw-frame peak, whether accepted or not */

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

/* returns heart rate in Hz*100, 0 when no usable spectral peak */
static int hr_analyse(void)
{
    for (int i = 0; i < HR_N; i++) {
        vre[i] = (float)daq[i];
        vim[i] = 0;
    }
    /* 2nd-order HP (~0.5 Hz) -> AGC -> 2nd-order LP (~4 Hz) at 10 Hz,
       the filter chain that is known to work on this sensor. No
       differentiation: that tilted the spectrum toward HF noise. */
    {
        float x1 = vre[0], x2 = vre[0], y1 = 0, y2 = 0;
        for (int i = 0; i < HR_N; i++) {
            float x = vre[i];
            float y = 0.8703f * x - 1.7407f * x1 + 0.8703f * x2 +
                      1.7238f * y1 - 0.7575f * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            vre[i] = y;
        }
    }
    {
        /* AGC: normalise the window to a ~100-count envelope */
        float pk = 1.0f;
        for (int i = 0; i < HR_N; i++) {
            float a = vre[i] < 0 ? -vre[i] : vre[i];
            if (a > pk)
                pk = a;
        }
        float g = 100.0f / pk;
        for (int i = 0; i < HR_N; i++)
            vre[i] *= g;
    }
    {
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int i = 0; i < HR_N; i++) {
            float x = vre[i];
            float y = 0.1160f * x + 0.2319f * x1 + 0.1160f * x2 +
                      0.7217f * y1 - 0.1855f * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            vre[i] = y;
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
    /* gate on THIS frame's spectrum first — otherwise a stale running
       average can keep reporting a peak after the signal is gone */
    float mraw[HR_SPEC];
    float mx = 0, sum = 0;
    for (int k = 0; k < HR_SPEC; k++)
        mraw[k] = sqrtf(vre[k] * vre[k] + vim[k] * vim[k]);
    for (int k = 3; k <= 26; k++) {
        sum += mraw[k];
        if (mraw[k] > mx)
            mx = mraw[k];
    }
    float snr = sum > 0 ? mx / (sum / 24) : 0;
    last_snr = snr;
    /* raw-frame peak with parabolic interpolation, for the consistency
       tracker below and the diag screen */
    int pk = 3;
    for (int k = 3; k <= 26; k++)
        if (mraw[k] == mx) {
            pk = k;
            break;
        }
    float pkf = pk;
    if (pk > 3 && pk < 26) {
        float a = mraw[pk - 1], b = mraw[pk], c = mraw[pk + 1];
        float den = a - 2 * b + c;
        if (den != 0)
            pkf += 0.5f * (a - c) / den;
    }
    float pk_hz = pkf * hr_fs / HR_N;
    last_pk_bpm = (int)(pk_hz * 60.0f + 0.5f);
    /* consistency path: a real pulse repeats. Three consecutive frames
       whose peaks agree within 8% count even if SNR is mediocre */
    if (snr >= 3.0f && pk_hz >= 40.0f / 60.0f && pk_hz <= 200.0f / 60.0f) {
        if (cand_n == 8) {
            for (int i = 0; i < 7; i++)
                cand_hz[i] = cand_hz[i + 1];
            cand_n = 7;
        }
        cand_hz[cand_n++] = pk_hz;
    } else {
        cand_n = 0;
    }
    /* frames overlap 92%, so 8 agreeing frames = ~4 s of fresh signal */
    if (cand_n >= 8) {
        float m = 0;
        for (int i = 0; i < 8; i++)
            m += cand_hz[i];
        m /= 8.0f;
        int ok = 1;
        for (int i = 0; i < 8; i++)
            if (cand_hz[i] < m * 0.94f || cand_hz[i] > m * 1.06f)
                ok = 0;
        if (ok)
            return (int)(m * 100 + 0.5f);
    }
    /* DC gate relative to the peak: broadband junk lifts bin 0 toward
       the HR-band magnitude, a clean pulse leaves it far below */
    if (snr <= 3.5f || mraw[0] >= mx * 0.05f)
        return 0;
    /* frame passed: merge into the running average */
    if (spec_reset) {
        spec_reset = 0;
        spec_n = 0;
    }
    for (int k = 0; k < HR_SPEC; k++)
        spec[k] = (spec[k] * spec_n + mraw[k]) / (spec_n + 1);
    if (spec_n < 2)
        spec_n++;
    /* recompute peak stats on the smoothed spectrum */
    mx = 0;
    for (int k = 3; k <= 26; k++)
        if (spec[k] > mx)
            mx = spec[k];
    /* exactly one narrow blob above 60% of max */
    float thr = 0.6f * mx;
    int blobs = 0, in = 0, first = 0, last = 0;
    float cent = 0, wsum = 0;
    for (int k = 3; k <= 26; k++) {
        if (spec[k] >= thr) {
            if (!in) {
                in = 1;
                blobs++;
                first = k;
            }
            last = k;
            cent += spec[k] * k;
            wsum += spec[k];
        } else {
            in = 0;
        }
    }
    if (blobs != 1 || wsum <= 0 || (last - first) > 2)
        return 0;
    float hz = (cent / wsum) * hr_fs / HR_N;
    if (hz < 40.0f / 60.0f || hz > 230.0f / 60.0f)
        return 0;
    return (int)(hz * 100 + 0.5f);
}

static void hrs_init(void)
{
    uint8_t id;
    if (!i2c_read_reg(HRS_ADDR, 0x00, &id, 1))
        return;
    /* register values proven on this sensor by the open PineTime
       stacks: 12.5 ms wait, 16-bit HRS+ALS, 8x gain, LED 12.5 mA with
       the steady-drive low nibble */
    i2c_write_reg(HRS_ADDR, 0x01, 0x60);
    i2c_write_reg(HRS_ADDR, 0x0C, 0x2f);
    i2c_write_reg(HRS_ADDR, 0x16, 0x88);  /* HRS+ALS 16-bit */
    i2c_write_reg(HRS_ADDR, 0x17, 0x0C);  /* gain 8x */
    hal_delay_ms(100);
    i2c_write_reg(HRS_ADDR, 0x01, 0x60 | 0x80);  /* enable */
    i2c_write_reg(HRS_ADDR, 0x0C, 0x2f);
    hrs_ok = 1;
}

static void hrs_sample(uint32_t now)
{
    uint8_t d[8];
    if (!i2c_read_reg(HRS_ADDR, 0x08, d, 8))
        return;
    int32_t hrs = ((int32_t)d[1] << 8) | ((int32_t)(d[2] & 0x0f) << 4) |
                  (d[7] & 0x0f) | ((int32_t)(d[7] & 0x30) << 12);
    /* ambient channel (C1): subtract it so room light doesn't flood the
       pulse; a saturated ALS also means "not on a wrist" */
    int32_t als = ((int32_t)(d[5] & 0x3f) << 11) | ((int32_t)d[0] << 3) |
                  (d[6] & 0x07);
    hrs_raw = hrs;
    hrs_als = als;
    hrs -= als;
    if (hrs < 0)
        hrs = 0;

    /* DC removal + smoothing */
    dc += (hrs - dc) >> 7;
    int32_t ac = hrs - dc;
    ac_f += (ac - ac_f) >> 2;

    /* contact check: raw near zero or dead-flat AC = off wrist */
    if (now - win_ms >= 2000) {
        prev_win = win_max;
        win_max = 0;
        ppg_lo = 32767;
        ppg_hi = -32768;
        win_ms = now;
    }
    {
        int32_t a = ac_f < 0 ? -ac_f : ac_f;
        if (a > win_max)
            win_max = a;
    }
    /* off-wrist: near-zero signal or a dead-flat AC window. Thresholds
       are deliberately gentle — 300/30 kept real wrists dark; a still
       arm on cold skin sits under both */
    if (hrs < 120 || win_max < 20)
        bpm_valid = 0;

    /* feed the ECG trace: clamp-filtered AC, track display range */
    int16_t v = (int16_t)(ac_f > 32700 ? 32700 : ac_f < -32700 ? -32700 : ac_f);
    ppg[ppg_head] = v;
    ppg_head = (ppg_head + 1) % PPG_N;
    if (v < ppg_lo) ppg_lo = v;
    if (v > ppg_hi) ppg_hi = v;

    /* spectral DAQ: one sample per >=100 ms of wall time. The main loop
       can run far slower than 20 Hz while the display renders, so the
       window carries real timestamps and the true rate is measured */
    if (daq_n > 0 || last_daq_ms) {
        if (now - last_daq_ms < 100)
            return;
    }
    last_daq_ms = now;
    daq_t[daq_n] = now;
    daq[daq_n] = (uint16_t)hrs;
    if (daq_n < HR_N - 1) {
        daq_n++;
        return;
    }
    uint32_t span = daq_t[HR_N - 1] - daq_t[0];
    hr_fs = span ? (HR_N - 1) * 1000.0f / span : 10.0f;
    if (hr_fs < 4.0f || hr_fs > 16.0f) {   /* gap in sampling: restart */
        daq_n = 0;
        return;
    }
    int hz100 = hr_analyse();
    for (int i = 0; i < HR_N - 5; i++) { /* 0.5 s slide between runs */
        daq[i] = daq[i + 5];
        daq_t[i] = daq_t[i + 5];
    }
    daq_n = HR_N - 5;
    beats++;
    last_ibi = (uint32_t)(last_snr * 10);
    if (hz100 == 0) {
        spec_reset = 1;
        /* holdover: keep reporting the last lock for ~3 s of bad
           frames — a single noisy window used to blank the display
           and force a full re-acquisition. The window keeps sliding
           so re-lock is instant when the signal returns. */
        if (bpm_valid && ++bad_frames >= 6) {
            for (int i = 0; i < 20; i++)
                hra[i] = 0;
            bpm = 0;
            bpm_valid = 0;
            bad_frames = 0;
        }
        return;
    }
    bad_frames = 0;
    hra[hra_i++ % 20] = hz100 / 100.0f;
    float sum = 0;
    int cnt = 0;
    for (int i = 0; i < 20; i++)
        if (hra[i] > 0) {
            sum += hra[i];
            cnt++;
        }
    if (cnt >= 3) {
        bpm = (int)(60.0f * sum / cnt + 0.5f);
        bpm_valid = 1;
    }
}

/* ---- public -------------------------------------------------------------- */

void sensors_init(void)
{
    bma_init();
    hrs_init();
}

static unsigned hrs_retries, bma_retries;

void sensors_tick(uint32_t now)
{
    static uint32_t last_bma, last_hrs, last_retry;
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
    if (bma_ok && now - last_bma >= 20) {   /* 50 Hz accel */
        last_bma = now;
        bma_sample(now);
    }
    /* use the unlatched clock: hal_millis() is frozen for a whole
       render frame, which would collapse the 50 ms cadence to the
       frame rate during long redraws */
    uint32_t raw = hal_millis_raw();
    if (hrs_ok && hr_on && raw - last_hrs >= 50) {   /* 20 Hz PPG */
        last_hrs = raw;
        hrs_sample(raw);
    }
    if (hrs_ok && !hr_on && hrs_powered) {
        hrs_power(0);                   /* manual off: LED dark for good */
        hrs_powered = 0;
        bpm_valid = 0;
        bpm = 0;
        daq_n = 0;
        spec_reset = 1;
    }
}

void sensors_hr_set(int on)
{
    hr_on = on ? 1 : 0;
    if (hr_on && hrs_ok) {
        daq_n = 0;
        spec_reset = 1;
        dc = ac_f = 0;
        hrs_power(1);
        hrs_powered = 1;
    }
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
int sensors_bma_dbg(void) { return (bma_id << 16) | (bma_stat << 8) | bma_feat; }
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
int sensors_bpm(void)
{
    if (!hrs_ok) return -1;   /* no sensor */
    return bpm_valid ? bpm : 0;   /* 0 = measuring / poor contact */
}

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
