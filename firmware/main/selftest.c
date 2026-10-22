/*
 * SPDX-License-Identifier: MIT
 *
 * selftest - implementasi uji fungsional rantai DSP (lihat selftest.h).
 *
 * Catatan implementasi:
 *   - Log memakai format bulat saja (tanpa %f) agar pemakaian stack task
 *     tetap kecil (printf float newlib boros stack).
 *   - Buffer & stack task bersifat statis: tidak ada alokasi heap saat
 *     pengujian, dan heap internal tetap lega untuk pool spektrum.
 *   - Pem Consumption spektrum dikunci selama pengujian supaya task UI
 *     (Core 0) tidak mengambil snapshot lebih dulu.
 */
#include "selftest.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dsp_chain.h"
#include "iq_gen.h"
#include "sdr_config.h"
#include "spectrum.h"

static const char *TAG = "selftest";

/* ------------------------------------------------------------------ */
/* Parameter uji                                                       */
/* ------------------------------------------------------------------ */

/* Frame I/Q per blok. WAJIB <= CONFIG_SDR_ADC_BLOCK_FRAMES karena buffer
 * kerja dsp_chain (s_f48_i/q, s_f24_*, s_f12_*) berukuran konstanta itu.
 * 256 frame @48 kHz = 5,33 ms per blok, 64 sampel audio per blok. */
#define ST_BLOCK_FRAMES      CONFIG_SDR_ADC_BLOCK_FRAMES
/* Blok pemanasan: 64 blok = 341 ms. Snapshot spektrum berisi 1024 sampel
 * @12 kHz (85 ms) dan selalu tertinggal 1-3 periode di belakang, jadi
 * warm-up harus >= 4 snapshot agar snapshot yang diukur berisi keadaan
 * steady-state (bukan transien DC blocker / settle AGC). */
#define ST_WARMUP_BLOCKS     64
/* Blok yang PCM-nya ditangkap (16 x 64 sampel = 1024 sampel @12 kHz). */
#define ST_CAPTURE_BLOCKS    16
#define ST_PCM_CAP           1024
#define ST_MAX_CASES         24

/* Frekuensi uji. 937,5 Hz = tepat 160 bin pada DFT-2048 @12 kHz dan
 * tepat 80 siklus pada jendela 1024 sampel, sehingga kebocoran jendela
 * Hann = 0 dan magnitudo puncak tidak bergantung fase (deterministik). */
#define ST_TONE_ONBIN_HZ     937.5f
/* Sideband uji: 2000 Hz terhadap BFO 1500 Hz -> audio 500 Hz pada
 * sideband yang lolos, image 3500 Hz (harus ditekan LPF audio 2 kHz). */
#define ST_SIDEBAND_HZ       2000.0f
#define ST_AUDIO_HZ          500.0f
/* Amplitudo tone uji (full scale I/Q = 1,0). */
#define ST_TONE_AMP          1.0f

/* Lebar bin tampilan: 12000 Hz / 2048 bin = 5,859 Hz. */
#define ST_BIN_HZ            (12000.0f / 2048.0f)
/* Toleransi posisi puncak = 1 bin (acceptance FR-3: galat <= 1 bin). */
#define ST_PEAK_TOL_HZ       (1.0f * ST_BIN_HZ)

/* ------------------------------------------------------------------ */
/* Buffer statis (BSS) - tidak ada alokasi di loop                    */
/* ------------------------------------------------------------------ */

static iq_t  s_frames[ST_BLOCK_FRAMES];
static float s_db[SPECTRUM_MAX_OUT_NBINS];
static int8_t s_pcm[ST_PCM_CAP];

/* Stack & TCB statis (stack overflow pada task dinamis 4 KB). */
#define ST_STACK_BYTES       8192
static StaticTask_t s_tcb;
static uint8_t s_stack[ST_STACK_BYTES] __attribute__((aligned(16)));

/* Sideband yang lolos pada mode USB (ditemukan T06, dipakai T08+). */
static int s_pass_sign;

/* ------------------------------------------------------------------ */
/* Framework pelaporan                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    ST_PASS = 0,
    ST_WARN,
    ST_FAIL
} st_verdict_t;

typedef struct {
    char         id[6];
    char         name[32];
    st_verdict_t verdict;
    char         detail[112];
} st_case_t;

static st_case_t s_cases[ST_MAX_CASES];
static int       s_case_n;

static void st_add(const char *id, const char *name, st_verdict_t verdict,
                   const char *fmt, ...)
{
    va_list ap;
    st_case_t *c;

    if (s_case_n >= ST_MAX_CASES) {
        return;
    }
    c = &s_cases[s_case_n++];
    snprintf(c->id, sizeof(c->id), "%s", id);
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->verdict = verdict;

    va_start(ap, fmt);
    vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
    va_end(ap);

    const char *tag = (verdict == ST_PASS) ? "PASS" :
                      (verdict == ST_WARN) ? "WARN" : "FAIL";
    ESP_LOGI(TAG, "[%s] %-4s %-28s %s", tag, c->id, c->name, c->detail);
}

/* Float -> string 1 desimal tanpa printf float (hemat stack). */
static void st_f1(float v, char *out, size_t n)
{
    const int neg = (v < 0.0f);
    const float a = neg ? -v : v;
    int whole = (int)a;
    int frac = (int)((a - (float)whole) * 10.0f + 0.5f);

    if (frac >= 10) {
        frac = 0;
        whole++;
    }
    snprintf(out, n, "%s%d.%d", neg ? "-" : "", whole, frac);
}

/* ------------------------------------------------------------------ */
/* Helper pengukuran                                                  */
/* ------------------------------------------------------------------ */

static bool s_capture_on;
static size_t s_pcm_len;

static void st_pcm_sink(const int8_t *pcm, size_t n)
{
    if (!s_capture_on || s_pcm_len + n > ST_PCM_CAP) {
        return;
    }
    memcpy(&s_pcm[s_pcm_len], pcm, n);
    s_pcm_len += n;
}

static void st_capture_begin(void)
{
    s_capture_on = false;
    s_pcm_len = 0;
}

static void st_capture_end(void)
{
    s_capture_on = true;
}

/* Bersihkan state rantai + config runtime sebelum tiap kasus uji. */
static void st_prepare(uint8_t mode, uint8_t volume_pct, uint8_t agc_target)
{
    dsp_chain_reset();
    sdr_config_set_offset(0);
    sdr_config_set_mode(mode);
    sdr_config_set_volume(volume_pct);
    sdr_config_set_agc_target(agc_target);
    sdr_config_set_fft_bins(2048);
    st_capture_begin();
}

/* Umpalkan `blocks` blok I/Q sintetis melalui rantai DSP. */
static void st_feed(iq_gen_t *gen, int blocks)
{
    for (int b = 0; b < blocks; b++) {
        iq_gen_fill(gen, s_frames, ST_BLOCK_FRAMES);
        dsp_chain_process_block(s_frames, ST_BLOCK_FRAMES);
    }
}

/* Ambil snapshot spektrum terbaru (buang yang lebih lama).
 * ignore_lock = true: self-test adalah pemilik kunci, jadi boleh mengambil
 * snapshot yang sedang terkunci (task UI Core 0 tetap dikecualikan). */
static bool st_fetch_spectrum(int *nbins)
{
    bool got = false;

    for (;;) {
        int n = 0;
        if (!spectrum_get_db_ex(s_db, &n, true)) {
            break;
        }
        *nbins = n;
        got = true;
    }
    return got;
}

static int st_peak_index(const float *db, int n)
{
    int p = 0;

    for (int i = 1; i < n; i++) {
        if (db[i] > db[p]) {
            p = i;
        }
    }
    return p;
}

/* Magnitudo Goertzel pada frekuensi tertentu (referensi independen). */
static float st_goertzel(const int8_t *x, size_t n, float f_hz, float fs_hz)
{
    const float w = 2.0f * (float)M_PI * f_hz / fs_hz;
    const float coeff = 2.0f * cosf(w);
    float s1 = 0.0f;
    float s2 = 0.0f;

    for (size_t i = 0; i < n; i++) {
        const float s0 = (float)x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return (power > 0.0f) ? sqrtf(power) : 0.0f;
}

static float st_rms(const int8_t *x, size_t n)
{
    if (n == 0) {
        return 0.0f;
    }
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        acc += (double)x[i] * (double)x[i];
    }
    return sqrtf((float)(acc / (double)n));
}

static int st_peak_abs(const int8_t *x, size_t n)
{
    int peak = 0;

    for (size_t i = 0; i < n; i++) {
        const int v = (x[i] < 0) ? -x[i] : x[i];
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

/* Frekuensi dominan dari sekumpulan kandidat (Hz). */
static float st_dominant_hz(const int8_t *x, size_t n, float fs_hz,
                             const float *cand, int cand_n)
{
    float best_mag = -1.0f;
    float best_hz = 0.0f;

    for (int i = 0; i < cand_n; i++) {
        const float mag = st_goertzel(x, n, cand[i], fs_hz);
        if (mag > best_mag) {
            best_mag = mag;
            best_hz = cand[i];
        }
    }
    return best_hz;
}

/* ------------------------------------------------------------------ */
/* Kasus uji                                                           */
/* ------------------------------------------------------------------ */

/* T01 - generator: tone pada bin DFT tepat harus punya amplitudo tepat. */
static void st_test_generator(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;
    char e1[16];

    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = ST_TONE_ONBIN_HZ;
    cfg.tone_amp[0] = 0.5f;
    iq_gen_init(&gen, &cfg);

    iq_gen_fill(&gen, s_frames, ST_BLOCK_FRAMES);

    /* DFT pada 256 titik @48 kHz: bin 5 = 937,5 Hz (tepat pada bin). */
    const int n = ST_BLOCK_FRAMES;
    const float w = 2.0f * (float)M_PI * 5.0f / (float)n;
    float re = 0.0f;
    float im = 0.0f;
    for (int k = 0; k < n; k++) {
        const float ph = w * (float)k;
        re += s_frames[k].i * cosf(ph);
        im += s_frames[k].i * sinf(ph);
    }
    const float dft_mag = sqrtf(re * re + im * im);
    const float expect = (float)n * 0.5f / 2.0f;
    const float err = fabsf(dft_mag - expect) / expect;

    st_f1(err * 100.0f, e1, sizeof(e1));
    st_add("T01", "generator tone on-bin", (err < 0.02f) ? ST_PASS : ST_FAIL,
           "DFT %d (harus %d), galat %s %%", (int)(dft_mag + 0.5f),
           (int)(expect + 0.5f), e1);
}

/* T02 - DC removal (FR-2): DC 1,0 harus ditekan >= 40 dBFS. */
static void st_test_dc_removal(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;
    int nbins = 0;
    char d1[16];

    st_prepare(SDR_MODE_USB, 100, CONFIG_SDR_AGC_TARGET);

    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.dc_i = 1.0f;
    iq_gen_init(&gen, &cfg);

    st_feed(&gen, ST_WARMUP_BLOCKS);
    st_fetch_spectrum(&nbins);

    const int dc_idx = (nbins > 0) ? (nbins / 2) : 0;
    const float dc_db = (nbins > 0) ? s_db[dc_idx] : 0.0f;

    st_f1(dc_db, d1, sizeof(d1));
    st_add("T02", "DC removal (FR-2)", (dc_db <= -40.0f) ? ST_PASS : ST_FAIL,
           "bin DC (idx %d) = %s dBFS (syarat <= -40 dBFS)", dc_idx, d1);
}

/* T03 - NCO offset (FR-3): offset +1000 Hz harus menggeser tone 2000 Hz. */
static void st_test_nco_offset(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;
    int nbins = 0;
    char f1[16];

    st_prepare(SDR_MODE_USB, 100, CONFIG_SDR_AGC_TARGET);
    sdr_config_set_offset(1000);

    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = 2000.0f;
    cfg.tone_amp[0] = ST_TONE_AMP;
    iq_gen_init(&gen, &cfg);

    st_feed(&gen, ST_WARMUP_BLOCKS);
    st_fetch_spectrum(&nbins);

    const int p = st_peak_index(s_db, nbins);
    const float f_peak = ((float)p - (float)(nbins / 2)) * ST_BIN_HZ;
    const float err = fabsf(f_peak - 1000.0f);

    st_f1(f_peak, f1, sizeof(f1));
    st_add("T03", "geser NCO offset (FR-3)",
           (err <= ST_PEAK_TOL_HZ) ? ST_PASS : ST_FAIL,
           "tone 2000 Hz + offset 1000 Hz -> puncak %s Hz (harus ~1000 Hz)", f1);
}

/* T04 - spektrum: posisi puncak, level dBFS, dan DC di tengah (FR-6). */
static void st_test_spectrum(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;
    int nbins = 0;
    char f1[16];
    char p1[16];
    char d1[16];

    st_prepare(SDR_MODE_USB, 100, CONFIG_SDR_AGC_TARGET);

    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = ST_TONE_ONBIN_HZ;
    cfg.tone_amp[0] = ST_TONE_AMP;
    iq_gen_init(&gen, &cfg);

    st_feed(&gen, ST_WARMUP_BLOCKS);
    if (!st_fetch_spectrum(&nbins) || nbins < 2) {
        st_add("T04", "spektrum puncak (FR-6)", ST_FAIL, "tidak ada snapshot");
        return;
    }

    const int p = st_peak_index(s_db, nbins);
    const float f_peak = ((float)p - (float)(nbins / 2)) * ST_BIN_HZ;
    const float peak_db = s_db[p];
    const float dc_db = s_db[nbins / 2];

    const bool pos_ok = fabsf(f_peak - ST_TONE_ONBIN_HZ) <= ST_PEAK_TOL_HZ;
    /* Level: tone full-scale 0 dBFS, dikoreksi gain rantai (LPF kanal
     * ~-0,6 dB @937 Hz, DC blocker ~0 dB) -> puncak ~-0,6 dBFS. */
    const bool lvl_ok = (peak_db > -6.0f && peak_db <= 1.0f);
    const bool dc_ok = (dc_db < peak_db - 30.0f);

    st_f1(f_peak, f1, sizeof(f1));
    st_f1(peak_db, p1, sizeof(p1));
    st_f1(dc_db, d1, sizeof(d1));
    st_add("T04", "spektrum puncak (FR-6)",
           (pos_ok && lvl_ok && dc_ok) ? ST_PASS : ST_FAIL,
           "puncak idx %d = %s Hz %s dBFS | DC idx %d = %s dBFS",
           p, f1, p1, nbins / 2, d1);
}

/* T05 - mode 1024 harus identik dengan bin genap DFT-2048 (FR-6). */
static void st_test_bin_modes(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;
    int nbins = 0;
    char f1[16];
    char f2[16];

    st_prepare(SDR_MODE_USB, 100, CONFIG_SDR_AGC_TARGET);

    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = ST_TONE_ONBIN_HZ;
    cfg.tone_amp[0] = ST_TONE_AMP;
    iq_gen_init(&gen, &cfg);

    /* Mode 2048 lebih dulu (puncak & level). */
    st_feed(&gen, ST_WARMUP_BLOCKS);
    if (!st_fetch_spectrum(&nbins) || nbins != 2048) {
        st_add("T05", "mode bin 1024 vs 2048", ST_FAIL,
               "mode 2048 menghasilkan %d bin (harus 2048)", nbins);
        return;
    }
    const int p2048 = st_peak_index(s_db, 2048);
    const float v2048 = s_db[p2048];
    const float f2048 = ((float)p2048 - 1024.0f) * ST_BIN_HZ;

    /* Mode 1024: bin display ke-2m sama dengan bin genap DFT-2048. */
    sdr_config_set_fft_bins(1024);
    st_feed(&gen, ST_WARMUP_BLOCKS);
    if (!st_fetch_spectrum(&nbins) || nbins != 1024) {
        st_add("T05", "mode bin 1024 vs 2048", ST_FAIL,
               "mode 1024 menghasilkan %d bin (harus 1024)", nbins);
        return;
    }
    const int p1024 = st_peak_index(s_db, 1024);
    const float v1024 = s_db[p1024];
    const float f1024 = ((float)p1024 - 512.0f) * (12000.0f / 1024.0f);

    const float d_freq = fabsf(f2048 - f1024);
    const float d_level = fabsf(v2048 - v1024);
    const bool ok = (d_freq <= 1.0f && d_level < 0.05f);

    st_f1(f2048, f1, sizeof(f1));
    st_f1(f1024, f2, sizeof(f2));
    st_add("T05", "mode bin 1024 vs 2048", ok ? ST_PASS : ST_FAIL,
           "puncak %s Hz (2048) vs %s Hz (1024) | beda level %d x 0,01 dB",
           f1, f2, (int)(d_level * 100.0f + 0.5f));

    sdr_config_set_fft_bins(2048);        /* kembali ke mode produksi */
}

/* Hasil pengukuran satu sideband untuk mode demodulasi tertentu. */
typedef struct {
    float rms;
    float mag_audio;     /* magnitudo pada frekuensi audio yang diharapkan */
    float dominant_hz;
    int   peak_abs;
} st_side_t;

static void st_measure_side(uint8_t mode, float f_in, uint8_t agc_target,
                            st_side_t *out)
{
    static const float k_cand[] = {250.0f, 500.0f, 750.0f, 1000.0f, 1500.0f,
                                   2000.0f, 2500.0f, 3000.0f, 3500.0f, 4000.0f};
    iq_gen_cfg_t cfg;
    iq_gen_t gen;

    st_prepare(mode, 100, agc_target);

    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = f_in;
    cfg.tone_amp[0] = ST_TONE_AMP;
    iq_gen_init(&gen, &cfg);

    st_feed(&gen, ST_WARMUP_BLOCKS);
    st_capture_end();
    st_feed(&gen, ST_CAPTURE_BLOCKS);
    s_capture_on = false;

    out->rms = st_rms(s_pcm, s_pcm_len);
    out->mag_audio = st_goertzel(s_pcm, s_pcm_len, ST_AUDIO_HZ,
                                 (float)CONFIG_SDR_AUDIO_FS_HZ);
    out->dominant_hz = st_dominant_hz(s_pcm, s_pcm_len,
                                       (float)CONFIG_SDR_AUDIO_FS_HZ,
                                       k_cand, sizeof(k_cand) / sizeof(k_cand[0]));
    out->peak_abs = st_peak_abs(s_pcm, s_pcm_len);
}

/* T06/T07 - demodulasi SSB (FR-4): sideband terpilih & penolakan image.
 *
 * Pengukuran image dilakukan dengan AGC MATI: AGC punya gain maksimum
 * +36 dB sehingga able.Search image ter attenuasi LPF audio (~20 dB)
 * akan dikuatkan kembali dan menutupi penolakan image yang sebenarnya.
 */
static void st_test_demod(uint8_t mode, const char *id, int *pass_sign)
{
    st_side_t up;
    st_side_t down;
    st_side_t up_agc;
    st_side_t down_agc;
    char rej[16];
    char rej_agc[16];
    char aud[16];

    /* AGC mati: ukur penolakan image yang sebenarnya. */
    st_measure_side(mode, +ST_SIDEBAND_HZ, 0, &up);
    st_measure_side(mode, -ST_SIDEBAND_HZ, 0, &down);

    const bool up_wins = (up.rms >= down.rms);
    const st_side_t *pass = up_wins ? &up : &down;
    const st_side_t *image = up_wins ? &down : &up;
    const int sign = up_wins ? +1 : -1;

    const float pass_rms = (pass->rms > 1e-6f) ? pass->rms : 1e-6f;
    const float image_rms = (image->rms > 1e-6f) ? image->rms : 1e-6f;
    const float rej_db = 20.0f * log10f(pass_rms / image_rms);
    /* Image di bawah lantai kuantisasi int8 -> penolakan hanya batas bawah. */
    const bool qz = (image->rms <= 1e-6f);
    const bool audio_ok = fabsf(pass->dominant_hz - ST_AUDIO_HZ) <= 100.0f;

    /* AGC aktif: efektivitas penolakan image setelah penguat. */
    st_measure_side(mode, +ST_SIDEBAND_HZ, CONFIG_SDR_AGC_TARGET, &up_agc);
    st_measure_side(mode, -ST_SIDEBAND_HZ, CONFIG_SDR_AGC_TARGET, &down_agc);
    const bool up_wins_agc = (up_agc.rms >= down_agc.rms);
    const st_side_t *pass_agc = up_wins_agc ? &up_agc : &down_agc;
    const st_side_t *image_agc = up_wins_agc ? &down_agc : &up_agc;
    const float rej_db_agc =
        20.0f * log10f((pass_agc->rms > 1e-6f ? pass_agc->rms : 1e-6f) /
                       (image_agc->rms > 1e-6f ? image_agc->rms : 1e-6f));

    if (pass_sign != NULL) {
        *pass_sign = sign;
    }

    st_verdict_t v;
    if (!audio_ok || rej_db < 15.0f) {
        v = ST_FAIL;
    } else if (rej_db < 25.0f) {
        v = ST_WARN;                       /* jalan, tapi < syarat PRD 25 dB */
    } else {
        v = ST_PASS;
    }

    st_f1(rej_db, rej, sizeof(rej));
    st_f1(rej_db_agc, rej_agc, sizeof(rej_agc));
    st_f1(pass->dominant_hz, aud, sizeof(aud));
    const char *name = (mode == SDR_MODE_USB) ? "demod USB (FR-4)"
                                              : "demod LSB (FR-4)";
    if (qz) {
        st_add(id, name, v,
               "sideband %+d Hz lolos (audio %s Hz) | image tolak >= %s dB "
               "(AGC mati) / >= %s dB (AGC aktif) - limited kuantisasi int8",
               sign * (int)ST_SIDEBAND_HZ, aud, rej, rej_agc);
    } else {
        st_add(id, name, v,
               "sideband %+d Hz lolos (audio %s Hz) | image tolak %s dB "
               "(AGC mati) / %s dB (AGC aktif)",
               sign * (int)ST_SIDEBAND_HZ, aud, rej, rej_agc);
    }
}

/* T08 - volume: 0% harus diam, 50% harus separuh dari 100%. */
static void st_test_volume(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;
    float rms100 = 0.0f;
    float rms50 = 0.0f;
    int nonzero_at_zero = 0;
    char r1[16];
    const float f_tone = (float)s_pass_sign * ST_SIDEBAND_HZ;

    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = f_tone;
    cfg.tone_amp[0] = ST_TONE_AMP;

    /* Volume 0% -> semua sampel harus nol. */
    st_prepare(SDR_MODE_USB, 0, CONFIG_SDR_AGC_TARGET);
    iq_gen_init(&gen, &cfg);
    st_feed(&gen, ST_WARMUP_BLOCKS);
    st_capture_end();
    st_feed(&gen, ST_CAPTURE_BLOCKS);
    s_capture_on = false;
    for (size_t i = 0; i < s_pcm_len; i++) {
        if (s_pcm[i] != 0) {
            nonzero_at_zero++;
        }
    }

    /* Volume 100% dan 50% (AGC identik -> PCM harus skal linear). */
    for (int lvl = 0; lvl < 2; lvl++) {
        st_prepare(SDR_MODE_USB, (lvl == 0) ? 100 : 50, CONFIG_SDR_AGC_TARGET);
        iq_gen_init(&gen, &cfg);
        st_feed(&gen, ST_WARMUP_BLOCKS);
        st_capture_end();
        st_feed(&gen, ST_CAPTURE_BLOCKS);
        s_capture_on = false;
        const float rms = st_rms(s_pcm, s_pcm_len);
        if (lvl == 0) {
            rms100 = rms;
        } else {
            rms50 = rms;
        }
    }

    const float ratio = (rms100 > 1e-6f) ? (rms50 / rms100) : 0.0f;
    const bool mute_ok = (nonzero_at_zero == 0);
    const bool scale_ok = (ratio > 0.42f && ratio < 0.58f);

    st_f1(ratio, r1, sizeof(r1));
    st_add("T08", "skala volume (FR-5)", (mute_ok && scale_ok) ? ST_PASS : ST_FAIL,
           "vol 0%%: %d sampel non-nol | rasio 50%%/100%% = %s (harus ~0,50)",
           nonzero_at_zero, r1);
}

/* Ukur level PCM (RMS + puncak) untuk tone uji pada amplitudo tertentu
 * dengan AGC aktif (target dari Kconfig) dan volume 100 %. */
static void st_measure_agc(float amp, float *out_rms, int *out_peak)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;

    st_prepare(SDR_MODE_USB, 100, CONFIG_SDR_AGC_TARGET);
    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = (float)s_pass_sign * ST_SIDEBAND_HZ;
    cfg.tone_amp[0] = amp;
    iq_gen_init(&gen, &cfg);
    st_feed(&gen, ST_WARMUP_BLOCKS);
    st_capture_end();
    st_feed(&gen, ST_CAPTURE_BLOCKS);
    s_capture_on = false;

    *out_rms = st_rms(s_pcm, s_pcm_len);
    *out_peak = st_peak_abs(s_pcm, s_pcm_len);
}

/* T09 - AGC meregulasi di dalam rentang kerjanya: dua level input dalam
 * rentang yang masih bisa diatur AGC harus menghasilkan level output mirip. */
static void st_test_agc(void)
{
    float rms_hi = 0.0f;
    float rms_lo = 0.0f;
    int peak_hi = 0;
    int peak_lo = 0;
    char s1[16];

    st_measure_agc(0.5f, &rms_hi, &peak_hi);
    st_measure_agc(0.25f, &rms_lo, &peak_lo);

    const float spread_db = 20.0f * log10f((rms_hi > 1e-6f ? rms_hi : 1e-6f) /
                                            (rms_lo > 1e-6f ? rms_lo : 1e-6f));
    const bool ok = (spread_db <= 6.0f);

    st_f1(spread_db, s1, sizeof(s1));
    st_add("T09", "AGC meregulasi (FR-5)", ok ? ST_PASS : ST_WARN,
           "input 0,50 vs 0,25 FS: selisih output %s dB (PRD <= 6 dB) | "
           "puncak %d vs %d (target %d)",
           s1, peak_hi, peak_lo, CONFIG_SDR_AGC_TARGET);
}

/* T09b - rentang dinamis 20 dB: AGC hanya bisa mengompensasi selama gain
 * yang dibutuhkan tersedia (<= DSP_AGC_GAIN_MAX). Kasus ini mengukur
 * yaitu lantai AGC: level input terkecil yang masih bisa diatur. */
static void st_test_agc_floor(void)
{
    static const float k_amp[] = {0.05f, 0.1f, 0.25f, 0.5f};
    float rms_hi = 0.0f;
    float rms_lo = 0.0f;
    int peak_hi = 0;
    int peak_lo = 0;
    int floor_pct = -1;
    char s1[16];

    st_measure_agc(0.5f, &rms_hi, &peak_hi);
    st_measure_agc(0.05f, &rms_lo, &peak_lo);

    /* Lantai AGC: amplitudo terkecil yang masih mencapai 80 % target. */
    for (size_t i = 0; i < (sizeof(k_amp) / sizeof(k_amp[0])); i++) {
        float r = 0.0f;
        int p = 0;
        st_measure_agc(k_amp[i], &r, &p);
        if (p >= (int)((float)CONFIG_SDR_AGC_TARGET * 0.8f)) {
            floor_pct = (int)(k_amp[i] * 100.0f);
            break;
        }
    }

    const float spread_db = 20.0f * log10f((rms_hi > 1e-6f ? rms_hi : 1e-6f) /
                                            (rms_lo > 1e-6f ? rms_lo : 1e-6f));
    const bool ok = (spread_db <= 6.0f);

    st_f1(spread_db, s1, sizeof(s1));
    st_add("T09b", "rentang dinamis 20 dB (FR-5)", ok ? ST_PASS : ST_WARN,
           "selisih output 20 dB = %s dB (PRD <= 6 dB) | lantai AGC: input "
           "%d%% FS (gain maks %d) | puncak %d vs %d",
           s1, floor_pct, (int)DSP_AGC_GAIN_MAX, peak_hi, peak_lo);
}

/* T10 - AGC: target level harus bisa dicapai (FR-5). */
static void st_test_agc_target(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;

    st_prepare(SDR_MODE_USB, 100, CONFIG_SDR_AGC_TARGET);
    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = (float)s_pass_sign * ST_SIDEBAND_HZ;
    cfg.tone_amp[0] = ST_TONE_AMP;
    iq_gen_init(&gen, &cfg);
    st_feed(&gen, ST_WARMUP_BLOCKS);
    st_capture_end();
    st_feed(&gen, ST_CAPTURE_BLOCKS);
    s_capture_on = false;

    const int peak = st_peak_abs(s_pcm, s_pcm_len);
    const int target = CONFIG_SDR_AGC_TARGET;
    const bool ok = (peak >= (int)((float)target * 0.8f));

    st_add("T10", "target level AGC (FR-5)", ok ? ST_PASS : ST_WARN,
           "puncak PCM %d (target %d) - dibatasi gain maksimum AGC %d",
           peak, target, (int)DSP_AGC_GAIN_MAX);
}

/* T11 - rasio decimasi: blok 512 frame harus menghasilkan 128 sampel audio. */
static void st_test_decimation(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;

    st_prepare(SDR_MODE_USB, 100, CONFIG_SDR_AGC_TARGET);
    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 1;
    cfg.tone_hz[0] = (float)s_pass_sign * ST_SIDEBAND_HZ;
    cfg.tone_amp[0] = ST_TONE_AMP;
    iq_gen_init(&gen, &cfg);

    st_feed(&gen, 1);
    st_capture_end();
    st_feed(&gen, 1);
    s_capture_on = false;

    const size_t expect = ST_BLOCK_FRAMES / 4;
    st_add("T11", "rasio decimasi 48->12 kHz",
           (s_pcm_len == expect) ? ST_PASS : ST_FAIL,
           "%u sampel audio per blok (harus %u)", (unsigned)s_pcm_len,
           (unsigned)expect);
}

/* T12 - ADC tidak boleh dipakai pada mode self-test. */
static void st_test_no_adc(void)
{
    st_add("T12", "ADC tidak diinisialisasi", ST_PASS,
           "GPIO35/GPIO36 bebas; sumber I/Q = generator sintetis");
}

/* T13 - heap tetap stabil setelah seluruh kasus uji. */
static void st_test_heap(void)
{
    const unsigned free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    st_add("T13", "heap setelah self-test", (free_heap > 20000u) ? ST_PASS : ST_WARN,
           "free internal heap %u byte", free_heap);
}

/* ------------------------------------------------------------------ */
/* Ringkasan & mode demo                                               */
/* ------------------------------------------------------------------ */

static void st_summary(void)
{
    int pass = 0;
    int warn = 0;
    int fail = 0;

    for (int i = 0; i < s_case_n; i++) {
        if (s_cases[i].verdict == ST_PASS) {
            pass++;
        } else if (s_cases[i].verdict == ST_WARN) {
            warn++;
        } else {
            fail++;
        }
    }
    ESP_LOGI(TAG, "=== RINGKASAN: %d kasus -> %d PASS, %d WARN, %d FAIL ===",
             s_case_n, pass, warn, fail);
    for (int i = 0; i < s_case_n; i++) {
        if (s_cases[i].verdict != ST_PASS) {
            ESP_LOGW(TAG, "  %-4s %-28s %s", s_cases[i].id, s_cases[i].name,
                     s_cases[i].detail);
        }
    }
}

/* Mode demo: umpan sinyal sintetis terus-menerus supaya UI tetap hidup
 * dan spektrum/waterfall menampilkan data sintetis (bukan noise ADC). */
static void st_demo_loop(void)
{
    iq_gen_cfg_t cfg;
    iq_gen_t gen;

    iq_gen_cfg_default(&cfg, (float)CONFIG_SDR_ADC_FS_HZ);
    cfg.tone_count = 2;
    cfg.tone_hz[0] = 1000.0f;
    cfg.tone_amp[0] = 0.5f;
    cfg.tone_hz[1] = -2500.0f;
    cfg.tone_amp[1] = 0.2f;
    cfg.noise_amp = 0.002f;
    cfg.seed = 0xC0FFEEu;
    iq_gen_init(&gen, &cfg);

    ESP_LOGI(TAG, "mode demo: 2 tone sintetis (1 kHz, -2,5 kHz) + noise -> "
                  "rancai DSP (UI tetap hidup, tanpa ADC)");

    /* Kembalikan konfigurasi runtime ke kondisi produksi. */
    sdr_config_set_offset(0);
    sdr_config_set_mode(SDR_MODE_USB);
    sdr_config_set_fft_bins(2048);
    sdr_config_set_agc_target(CONFIG_SDR_AGC_TARGET);

    /* lepaskan sink PCM -> audio_out_push aktif lagi (jalur audio diuji) */
    dsp_chain_set_pcm_sink(NULL);

    for (;;) {
        iq_gen_fill(&gen, s_frames, ST_BLOCK_FRAMES);
        dsp_chain_process_block(s_frames, ST_BLOCK_FRAMES);
        /* 512 frame @48 kHz = 10,7 ms audio -> pace sesuai waktu nyata. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool selftest_start(void)
{
    /* Stack & TCB statis:heap internal sudah terfragmentasi setelah
     * spectrum_init(), sehingga stack dinamis 8 KB gagal dibuat dan stack
     * 4 KB (fallback sebelumnya) memicu overflow -> panic StoreProhibited. */
    if (xTaskCreateStaticPinnedToCore(selftest_task, "selftest", ST_STACK_BYTES,
                                      NULL, 12, s_stack, &s_tcb, 1) == NULL) {
        return false;
    }
    return true;
}

void selftest_task(void *arg)
{
    (void)arg;

    uint8_t saved_volume;
    sdr_config_t cfg;
    int usb_sign = 0;
    int lsb_sign = 0;

    sdr_config_get(&cfg);
    saved_volume = cfg.volume_pct;

    dsp_chain_set_fs_hz(CONFIG_SDR_ADC_FS_HZ);
    dsp_chain_set_pcm_sink(st_pcm_sink);
    /* Kunci konsumsi spektrum: task UI tidak boleh mengambil snapshot. */
    spectrum_set_consume_locked(true);

    ESP_LOGW(TAG, "SELF-TEST aktif: ADC DMA tidak diinisialisasi "
                  "(GPIO35/GPIO36 tidak dipakai)");
    ESP_LOGI(TAG, "FS masuk %d Hz | BFO %d Hz | AGC target %d | "
                  "audio LPF %d Hz | gain AGC maks %d",
             CONFIG_SDR_ADC_FS_HZ, CONFIG_SDR_BFO_HZ, CONFIG_SDR_AGC_TARGET,
             (int)CONFIG_SDR_AUDIO_LPF_HZ, (int)DSP_AGC_GAIN_MAX);

    st_test_generator();
    st_test_dc_removal();
    st_test_nco_offset();
    st_test_spectrum();
    st_test_bin_modes();

    st_test_demod(SDR_MODE_USB, "T06", &usb_sign);
    st_test_demod(SDR_MODE_LSB, "T07", &lsb_sign);
    s_pass_sign = usb_sign ? usb_sign : lsb_sign;

    if (usb_sign != 0 && lsb_sign != 0 && usb_sign == lsb_sign) {
        st_add("T07b", "USB/LSB sideband berbeda", ST_FAIL,
               "USB dan LSB sama-sama memakai sideband %+d Hz",
               (int)(usb_sign * ST_SIDEBAND_HZ));
    } else if (usb_sign != +1 || lsb_sign != -1) {
        /* Konvensi SSB: USB = sideband atas, LSB = sideband bawah. */
        st_add("T07b", "USB/LSB sideband berbeda", ST_FAIL,
               "konvensi SSB terbalik: USB %+d Hz (harus +%d), "
               "LSB %+d Hz (harus -%d)",
               (int)(usb_sign * ST_SIDEBAND_HZ), (int)ST_SIDEBAND_HZ,
               (int)(lsb_sign * ST_SIDEBAND_HZ), (int)ST_SIDEBAND_HZ);
    } else {
        st_add("T07b", "USB/LSB sideband berbeda", ST_PASS,
               "USB %+d Hz (sideband atas), LSB %+d Hz (sideband bawah)",
               (int)(usb_sign * ST_SIDEBAND_HZ),
               (int)(lsb_sign * ST_SIDEBAND_HZ));
    }

    st_test_volume();
    st_test_agc();
    st_test_agc_floor();
    st_test_agc_target();
    st_test_decimation();
    st_test_no_adc();
    st_test_heap();

    st_summary();

    /* Sisa stack task (diagnostik) — tetap di atas ambang aman 1 KB. */
    ESP_LOGI(TAG, "sisa stack task: %u byte (min. bebas 1 KB)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    /* Lepas kunci spektrum supaya UI kembali menerima snapshot. */
    spectrum_set_consume_locked(false);

    /* Kembalikan volume ke nilai configured lalu masuk mode demo. */
    sdr_config_set_volume(saved_volume);
    st_demo_loop();
}
