/*
 * SPDX-License-Identifier: MIT
 *
 * dsp_chain — sdr_rx_task (Core 1): penarik blok I/Q dari ADC DMA +
 * rantai DSP §6 (docs/ARCHITECTURE.md):
 *
 *   1. normalisasi (di rx_adc)                       48 kHz
 *   2. DC removal  y = x − x1 + R·y1 (per kanal)     48 kHz
 *   3. NCO offset  rotasi kompleks, LUT 1024         48 kHz
 *   4. LPF kanal + decimasi 48→24→12 (dsps_fird)     12 kHz
 *   5. BFO ke-2 (USB/LSB)                            12 kHz
 *   6. audio LPF ~2,0 kHz (dsps_fir)                 12 kHz
 *   7. AGC + volume → int8 → audio_out_push          12 kHz
 *
 * Loop dipacing oleh blocking `rx_adc_read()` — tanpa vTaskDelay, tanpa
 * logging per blok (docs/ARCHITECTURE.md §2 dan §9).
 */
#include "dsp_chain.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "dsps_fir.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "audio_out.h"
#include "rx_adc.h"
#include "sdr_config.h"
#include "spectrum.h"

static const char *TAG = "sdr_rx";

#define DSP_BLOCK_FRAMES      CONFIG_SDR_ADC_BLOCK_FRAMES
#define DSP_DIAG_PERIOD_US    2000000           /* 2 s — diagnostik F3/F5   */

#define DSP_PI                3.14159265358979323846f

/* NCO/BFO: LUT sin 1024 + interp linear; cos = sin (+90°). */
#define NCO_LUT_N             1024
#define NCO_QUARTER           (NCO_LUT_N / 4)
static float s_nco_lut[NCO_LUT_N + 1];          /* +1 sampel wrap          */

/* Tahap 4: dua decimasi-by-2 (halfband-equivalent windowed sinc). */
#define DSP_HB_TAPS           32
static float s_hb1[DSP_HB_TAPS];                /* desain @48 kHz          */
static float s_hb2[DSP_HB_TAPS];                /* desain @24 kHz          */
static fir_f32_t s_fir_hb1_i, s_fir_hb1_q;
static fir_f32_t s_fir_hb2_i, s_fir_hb2_q;
/* Delay line esp-dsp harus N+4 float (SIMD menulis 4 elemen tambahan) —
 * ukuran N saja menulis melewati batas (bug: menimpa delay line berikutnya). */
static float s_dl_hb1_i[DSP_HB_TAPS + 4], s_dl_hb1_q[DSP_HB_TAPS + 4];
static float s_dl_hb2_i[DSP_HB_TAPS + 4], s_dl_hb2_q[DSP_HB_TAPS + 4];

/* Tahap 6: audio LPF @12 kHz. */
#define DSP_AUD_TAPS          64
static float s_aud[DSP_AUD_TAPS];
static fir_f32_t s_fir_aud;
static float s_dl_aud[DSP_AUD_TAPS + 4];        /* dsps_fir_init_f32: N+4  */
static uint16_t s_aud_lpf_hz;                   /* cutoff aktif saat ini */

/* Desain ulang + inisialisasi koefisien LPF audio sesuai cutoff (Hz). */
static void dsp_lpf_design(float *h, int n, float fc_hz, float fs_hz);
static void dsp_aud_lpf_apply(uint16_t hz)
{
    dsp_lpf_design(s_aud, DSP_AUD_TAPS, (float)hz,
                   (float)CONFIG_SDR_AUDIO_FS_HZ);
    dsps_fir_init_f32(&s_fir_aud, s_aud, s_dl_aud, DSP_AUD_TAPS);
    s_aud_lpf_hz = hz;
}

/* Tahap 2: DC block per kanal. */
#define DSP_DC_R              ((float)CONFIG_SDR_DC_POLE_R)
static float s_dc_i_x1, s_dc_i_y1;
static float s_dc_q_x1, s_dc_q_y1;

/* AM: envelope DC block (buang carrier). */
static float s_env_dc_x1, s_env_dc_y1;

/* Tahap 3/5: fase NCO offset & BFO (satuan: siklus, [0,1)). */
static float s_nco_phase;
static float s_bfo_phase;

/* Tahap 7: AGC (batas gain didefinisikan di dsp_chain.h). */
#define DSP_AGC_SMOOTH        0.01f             /* tau ≈ 8 ms @ 12 kHz      */
static float s_agc_env;
static float s_agc_gain;
static float s_agc_alpha_a;
static float s_agc_alpha_r;

/* Squelch otomatis (gate output 0..1): AGC aktif & gain saturasi di
 * DSP_AGC_GAIN_MAX = tidak ada sinyal (hanya noise floor RF yang dinaikkan
 * AGC) → gate tertutup ke 0 agar desis tidak keluar speaker. Sinyal datang
 * (gain turun di bawah ceiling) → gate terbuka. Hanya bekerja saat AGC ON;
 * mode gain manual tidak digate (tombol Vol tetap mengendalikan). */
#define DSP_SQ_OPEN           (1.0f / 64.0f)    /* terbuka ~3 ms @12 kHz    */
#define DSP_SQ_CLOSE          (1.0f / 4096.0f)  /* tertutup ~330 ms         */
static float s_squelch;

/* F6: snapshot pasca-NCO untuk spektrum (1024 frame @12 kHz). */
#define DSP_SP_N              CONFIG_SDR_FFT_N
static iq_t s_sp_snap[DSP_SP_N];
static int  s_sp_cnt;

/* Buffer kerja per blok (BSS — bukan stack task). */
static iq_t s_block[DSP_BLOCK_FRAMES];
static float s_f48_i[DSP_BLOCK_FRAMES], s_f48_q[DSP_BLOCK_FRAMES];
static float s_f24_i[DSP_BLOCK_FRAMES / 2], s_f24_q[DSP_BLOCK_FRAMES / 2];
static float s_f12_i[DSP_BLOCK_FRAMES / 4], s_f12_q[DSP_BLOCK_FRAMES / 4];
static float s_audio_f[DSP_BLOCK_FRAMES / 4];
static int8_t s_audio_i8[DSP_BLOCK_FRAMES / 4];

/* Diagnostik. */
static uint32_t s_blocks;
static uint32_t s_frames;
static double s_sum_i;
static double s_sum_q;
static float s_peak_i;
static float s_peak_q;
static float s_audio_peak;
static int64_t s_diag_t0_us;
static uint32_t s_aud_prev;

/* Hook self-test (lihat dsp_chain.h). Default: produksi penuh. */
static dsp_pcm_sink_t s_pcm_sink;
static uint32_t s_fs_override_hz;

/* ----------------------------------------------------------------------
 * Utilitas
 * -------------------------------------------------------------------- */

/* LUT sin 0..2π (sampel wrap di index 1024 untuk interpolasi tanpa modulo). */
static void dsp_lut_init(void)
{
    for (int i = 0; i <= NCO_LUT_N; i++) {
        s_nco_lut[i] = sinf(2.0f * DSP_PI * (float)i / (float)NCO_LUT_N);
    }
}

/*
 * Koefisien LPF windowed sinc (Blackman), normalisasi DC gain = 1.
 * Dipakai untuk halfband-equivalent 2,7 kHz (decimasi) dan audio 2 kHz.
 */
static void dsp_lpf_design(float *h, int n, float fc_hz, float fs_hz)
{
    const float fc = fc_hz / fs_hz;
    const float mid = (float)(n - 1) / 2.0f;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        const float m = (float)i - mid;
        float s;
        if (fabsf(m) < 1e-6f) {
            s = 2.0f * fc;
        } else {
            s = sinf(2.0f * DSP_PI * fc * m) / (DSP_PI * m);
        }
        const float t = 2.0f * DSP_PI * (float)i / (float)(n - 1);
        const float w = 0.42f - 0.5f * cosf(t) + 0.08f * cosf(2.0f * t);
        h[i] = s * w;
        sum += h[i];
    }
    for (int i = 0; i < n; i++) {
        h[i] /= sum;
    }
}

/*
 * Rotator kompleks: I' = I·c + Q·s, Q' = Q·c − I·s dengan c/s dari LUT.
 * Dipakai NCO offset (tahap 3) dan BFO (tahap 5); fase kontinu antar blok.
 * Aman in-place (in/out boleh sama).
 */
static void dsp_rotator(float *phase, float step, const float *in_i,
                        const float *in_q, float *out_i, float *out_q, int n)
{
    float ph = *phase;
    for (int k = 0; k < n; k++) {
        const float x = ph * (float)NCO_LUT_N;
        const int i0 = (int)x;
        const float fr = x - (float)i0;
        const float s0 = s_nco_lut[i0];
        const float s = s0 + (s_nco_lut[i0 + 1] - s0) * fr;

        float xc = x + (float)NCO_QUARTER;      /* cos = sin + 90°         */
        if (xc >= (float)NCO_LUT_N) {
            xc -= (float)NCO_LUT_N;
        }
        const int ic = (int)xc;
        const float frc = xc - (float)ic;
        const float c0 = s_nco_lut[ic];
        const float c = c0 + (s_nco_lut[ic + 1] - c0) * frc;

        const float ai = in_i[k];
        const float aq = in_q[k];
        out_i[k] = ai * c + aq * s;
        out_q[k] = aq * c - ai * s;

        ph += step;
        ph -= floorf(ph);                       /* wrap ke [0,1), negatif OK */
    }
    *phase = ph;
}

/* Tahap 7 per sampel: env AGC → gain → volume → clip int8. */
static int8_t dsp_agc_and_scale(float x, float vol, float target, bool agc_on,
                                float manual_gain_lin)
{
    float g_des = 1.0f;

    if (agc_on) {
        const float a = fabsf(x);
        const float alpha = (a > s_agc_env) ? s_agc_alpha_a : s_agc_alpha_r;
        s_agc_env += (a - s_agc_env) * alpha;

        if (target > 0.0f) {
            g_des = (s_agc_env > 1e-6f) ? (target / s_agc_env) : DSP_AGC_GAIN_MAX;
            if (g_des > DSP_AGC_GAIN_MAX) {
                g_des = DSP_AGC_GAIN_MAX;
            } else if (g_des < DSP_AGC_GAIN_MIN) {
                g_des = DSP_AGC_GAIN_MIN;
            }
        }
        s_agc_gain += (g_des - s_agc_gain) * DSP_AGC_SMOOTH;
        g_des = s_agc_gain;
    } else {
        /* AGC off: tetap update s_agc_gain secara smooth ke manual gain
         * agar diagnostik tetap konsisten, tanpa mengikuti envelope input. */
        s_agc_gain += (manual_gain_lin - s_agc_gain) * DSP_AGC_SMOOTH;
        g_des = manual_gain_lin;
    }

    float y = x * g_des * vol;
    if (agc_on) {
        /* Squelch: ambang envelope PRA-gain = target/gain_max = lantai AGC
         * (keputusan PRD T-A2: sinyal di bawah lantai tidak terjamin). Env
         * di atas ambang => gate terbuka; di bawahnya (hanya noise floor,
         * AGC jenuh di ceiling) => tertutup perlahan. */
        const float thr = (target > 0.0f && DSP_AGC_GAIN_MAX > 1e-3f)
                              ? (target / DSP_AGC_GAIN_MAX)
                              : 0.0f;
        const bool signal = (thr <= 0.0f) ? true : (s_agc_env > thr);
        const float sq_rate = signal ? DSP_SQ_OPEN : DSP_SQ_CLOSE;
        s_squelch += ((float)(signal ? 1.0f : 0.0f) - s_squelch) * sq_rate;
        y *= s_squelch;
    }
    if (y > 127.0f) {
        y = 127.0f;
    } else if (y < -127.0f) {
        y = -127.0f;
    }
    return (int8_t)lroundf(y);
}

/* ----------------------------------------------------------------------
 * Proses satu blok I/Q (§6, 2 tahap 4 dkk. lihat header)
 * -------------------------------------------------------------------- */

static size_t dsp_process_block(const iq_t *in, size_t frames)
{
    if (frames < 4) {
        return 0;
    }
    const size_t n = frames & ~(size_t)1;       /* genap untuk decimasi    */

    sdr_config_t cfg;
    sdr_config_get(&cfg);

    /* Audio LPF dinamis: re-desain koefisien hanya saat cutoff berubah. */
    if (cfg.lpf_hz != s_aud_lpf_hz) {
        dsp_aud_lpf_apply(cfg.lpf_hz);
    }

    uint32_t fs = s_fs_override_hz ? s_fs_override_hz : rx_adc_measured_fs_hz();
    if (fs == 0) {
        fs = CONFIG_SDR_ADC_FS_HZ;
    }

    /* Tahap 2+3 (satu loop, vektor per blok): DC block lalu rotasi NCO.
     * AM: DC blocker DILUPLI (carrier AM = komponen DC kanal I) — envelope
     * |I+jQ| butuh carrier; DC carrier dibuang oleh DC block di envelope
     * (tahap 5). SSB: DC blocker normal per kanal. */
    const bool am_mode = (cfg.mode == SDR_MODE_AM);
    const float step_nco = (float)cfg.nco_offset_hz / (float)fs;
    float ph = s_nco_phase;
    for (size_t k = 0; k < n; k++) {
        const float xi = in[k].i;
        const float xq = in[k].q;
        float di, dq;
        if (am_mode) {
            di = xi;
            dq = xq;
        } else {
            di = xi - s_dc_i_x1 + DSP_DC_R * s_dc_i_y1;
            dq = xq - s_dc_q_x1 + DSP_DC_R * s_dc_q_y1;
            s_dc_i_x1 = xi;
            s_dc_i_y1 = di;
            s_dc_q_x1 = xq;
            s_dc_q_y1 = dq;
        }

        const float x = ph * (float)NCO_LUT_N;
        const int i0 = (int)x;
        const float fr = x - (float)i0;
        const float s0 = s_nco_lut[i0];
        const float s = s0 + (s_nco_lut[i0 + 1] - s0) * fr;
        float xc = x + (float)NCO_QUARTER;
        if (xc >= (float)NCO_LUT_N) {
            xc -= (float)NCO_LUT_N;
        }
        const int ic = (int)xc;
        const float frc = xc - (float)ic;
        const float c0 = s_nco_lut[ic];
        const float c = c0 + (s_nco_lut[ic + 1] - c0) * frc;

        s_f48_i[k] = di * c + dq * s;
        s_f48_q[k] = dq * c - di * s;

        ph += step_nco;
        ph -= floorf(ph);
    }
    s_nco_phase = ph;

    /* Tahap 4: LPF kanal + decimasi 48→24→12 (state tersimpan antar blok). */
    const int n24 = (int)(n / 2);
    const int n12 = (int)(n / 4);
    dsps_fird_f32(&s_fir_hb1_i, s_f48_i, s_f24_i, n24);
    dsps_fird_f32(&s_fir_hb1_q, s_f48_q, s_f24_q, n24);
    dsps_fird_f32(&s_fir_hb2_i, s_f24_i, s_f12_i, n12);
    dsps_fird_f32(&s_fir_hb2_q, s_f24_q, s_f12_q, n12);

    /* Tahap 4c (F6): akumulasi snapshot pasca-NCO untuk spektrum. */
    for (int k = 0; k < n12; k++) {
        s_sp_snap[s_sp_cnt].i = s_f12_i[k];
        s_sp_snap[s_sp_cnt].q = s_f12_q[k];
        if (++s_sp_cnt == DSP_SP_N) {
            spectrum_ingest(s_sp_snap);
            s_sp_cnt = 0;
        }
    }

    /* Tahap 5: demodulasi by mode.
     * SSB (USB/LSB): BFO ke-2. AM: envelope detection (|I+jQ|), tanpa BFO. */
    const float fs12 = (float)fs / 4.0f;
    if (cfg.mode == SDR_MODE_AM) {
        for (int k = 0; k < n12; k++) {
            float in1;
            switch (cfg.src) {
            case SDR_SRC_I:
                in1 = s_f12_i[k];
                break;
            case SDR_SRC_Q:
                in1 = s_f12_q[k];
                break;
            default:
            case SDR_SRC_IQ:
                in1 = sqrtf(s_f12_i[k] * s_f12_i[k] + s_f12_q[k] * s_f12_q[k]);
                break;
            }
            /* DC block carrier */
            const float denv = in1 - s_env_dc_x1 + DSP_DC_R * s_env_dc_y1;
            s_env_dc_x1 = in1;
            s_env_dc_y1 = denv;
            s_f12_i[k] = denv;
        }
        /* Tahap 6: audio LPF ~2,0 kHz pada envelope (mode AM). */
        dsps_fir_f32(&s_fir_aud, s_f12_i, s_audio_f, n12);
    } else {
        const float bfo_hz =
            (cfg.mode == SDR_MODE_USB) ? (float)CONFIG_SDR_BFO_HZ
                                       : -(float)CONFIG_SDR_BFO_HZ;
        dsp_rotator(&s_bfo_phase, bfo_hz / fs12, s_f12_i, s_f12_q,
                    s_f12_i, s_f12_q, n12);

        /* Tahap 6: audio LPF ~2,0 kHz pada komponen I hasil mix BFO. */
        dsps_fir_f32(&s_fir_aud, s_f12_i, s_audio_f, n12);
    }

    /* Tahap 7: AGC + volume → int8 → antrean audio. */
    const float vol = (float)cfg.volume_pct * 0.01f;
    const float agc_target = (float)cfg.agc_target;
    const float manual_gain =
        powf(10.0f, (float)cfg.manual_gain_db / 20.0f);
    float apk = 0.0f;
    for (int k = 0; k < n12; k++) {
        s_audio_i8[k] = dsp_agc_and_scale(s_audio_f[k], vol, agc_target,
                                          cfg.agc_enabled, manual_gain);
        const float a = fabsf(s_audio_f[k]);
        if (a > apk) {
            apk = a;
        }
    }
    s_audio_peak = apk;
    if (s_pcm_sink != NULL) {
        s_pcm_sink(s_audio_i8, (size_t)n12);
    } else {
        audio_out_push(s_audio_i8, (size_t)n12);
    }
    return frames;
}

/* ----------------------------------------------------------------------
 * API publik
 * -------------------------------------------------------------------- */

esp_err_t dsp_chain_init(void)
{
    /* Default produksi: tanpa sink PCM override, laju dari ADC. */
    s_pcm_sink = NULL;
    s_fs_override_hz = 0;

    dsp_lut_init();
    dsp_lpf_design(s_hb1, DSP_HB_TAPS, CONFIG_SDR_CH_LPF_HZ,
                   (float)CONFIG_SDR_ADC_FS_HZ);
    dsp_lpf_design(s_hb2, DSP_HB_TAPS, CONFIG_SDR_CH_LPF_HZ,
                   (float)CONFIG_SDR_ADC_FS_HZ / 2.0f);

    dsps_fird_init_f32(&s_fir_hb1_i, s_hb1, s_dl_hb1_i, DSP_HB_TAPS, 2);
    dsps_fird_init_f32(&s_fir_hb1_q, s_hb1, s_dl_hb1_q, DSP_HB_TAPS, 2);
    dsps_fird_init_f32(&s_fir_hb2_i, s_hb2, s_dl_hb2_i, DSP_HB_TAPS, 2);
    dsps_fird_init_f32(&s_fir_hb2_q, s_hb2, s_dl_hb2_q, DSP_HB_TAPS, 2);
    dsp_aud_lpf_apply(CONFIG_SDR_DEFAULT_AUDIO_LPF_HZ);

    const float fs12 = (float)CONFIG_SDR_AUDIO_FS_HZ;
    s_agc_alpha_a =
        1.0f - expf(-1.0f / ((CONFIG_SDR_AGC_ATTACK_MS / 1000.0f) * fs12));
    s_agc_alpha_r =
        1.0f - expf(-1.0f / ((CONFIG_SDR_AGC_RELEASE_MS / 1000.0f) * fs12));

    dsp_chain_reset();
    ESP_LOGI(TAG,
             "DSP F5: DC R %.3f | LPF %d tap %.0f Hz 48->24->12 | BFO +/-"
             "%d Hz | audio LPF %.0f Hz | AGC target %d gain maks %d | "
             "audio %d Hz",
             DSP_DC_R, DSP_HB_TAPS, CONFIG_SDR_CH_LPF_HZ, CONFIG_SDR_BFO_HZ,
             (float)CONFIG_SDR_DEFAULT_AUDIO_LPF_HZ, CONFIG_SDR_AGC_TARGET,
             (int)DSP_AGC_GAIN_MAX, CONFIG_SDR_AUDIO_FS_HZ);
    return ESP_OK;
}

void dsp_chain_reset(void)
{
    s_dc_i_x1 = 0.0f;
    s_dc_i_y1 = 0.0f;
    s_dc_q_x1 = 0.0f;
    s_dc_q_y1 = 0.0f;
    s_nco_phase = 0.0f;
    s_bfo_phase = 0.0f;
    s_env_dc_x1 = 0.0f;
    s_env_dc_y1 = 0.0f;
    s_agc_env = 0.0f;
    s_agc_gain = 1.0f;
    s_squelch = 1.0f;
    s_sp_cnt = 0;

    memset(s_dl_hb1_i, 0, sizeof(s_dl_hb1_i));
    memset(s_dl_hb1_q, 0, sizeof(s_dl_hb1_q));
    memset(s_dl_hb2_i, 0, sizeof(s_dl_hb2_i));
    memset(s_dl_hb2_q, 0, sizeof(s_dl_hb2_q));
    memset(s_dl_aud, 0, sizeof(s_dl_aud));
    s_fir_hb1_i.pos = 0;
    s_fir_hb1_q.pos = 0;
    s_fir_hb2_i.pos = 0;
    s_fir_hb2_q.pos = 0;
    s_fir_aud.pos = 0;

    s_blocks = 0;
    s_frames = 0;
    s_sum_i = 0.0;
    s_sum_q = 0.0;
    s_peak_i = 0.0f;
    s_peak_q = 0.0f;
    s_audio_peak = 0.0f;
    s_diag_t0_us = esp_timer_get_time();
}

size_t dsp_chain_process_block(const iq_t *in, size_t frames)
{
    return dsp_process_block(in, frames);
}

void dsp_chain_set_pcm_sink(dsp_pcm_sink_t sink)
{
    s_pcm_sink = sink;
}

void dsp_chain_set_fs_hz(uint32_t fs_hz)
{
    s_fs_override_hz = fs_hz;
}

void sdr_rx_task(void *arg)
{
    (void)arg;

    for (;;) {
        const size_t frames = rx_adc_read(s_block, DSP_BLOCK_FRAMES);
        if (frames == 0) {
            continue;       /* hanya timeout DMA; loop tetap dipacing driver */
        }

        for (size_t n = 0; n < frames; n++) {
            const float i = s_block[n].i;
            const float q = s_block[n].q;
            s_sum_i += (double)i;
            s_sum_q += (double)q;
            const float ai = (i < 0.0f) ? -i : i;
            const float aq = (q < 0.0f) ? -q : q;
            if (ai > s_peak_i) {
                s_peak_i = ai;
            }
            if (aq > s_peak_q) {
                s_peak_q = aq;
            }
        }
        s_blocks++;
        s_frames += (uint32_t)frames;

#if CONFIG_SDR_DEBUG_BYPASS_DSP
        /* DEBUG BYPASS: minimal path dari ADC menuju speaker.
         * Tujuan diagnostik: membuktikan bahwa 1 kHz dari baseband Tayloe
         * bisa keluar dari ADC read → audio_out tanpa dipotong NCO/BFO/LPF.
         *
         * Path:
         * - ambil Q sebagai sumber utama (kamu sudah lihat Q lebih kuat)
         * - DC block agar bias/level DC tidak membuang range
         * - decim 48k -> 12k (mengambil 1 dari 4) untuk audio_out 12k
         * - auto-gain sederhana dengan clipping
         */
        {
            static float imax=0,qmax=0,gain=16.0f;
            static float b_dc_q1, b_dc_q2;
            static float b_avg_q;
            static float b_gain_slow;

            size_t out = 0;
            for (size_t k = 0; k < frames; k += 4) {
                if (out >= 256) break;
                const float ii = s_block[k].i;
                const float qq = s_block[k].q;

                const float ai = (ii < 0.0f) ? -ii : ii;
                const float aq = (qq < 0.0f) ? -qq : qq;
                if (ai > imax) imax = ai;
                if (aq > qmax) qmax = aq;

                /* DC block pada Q (single pole, R=0.996). */
                const float x = qq - b_dc_q1 + 0.996f * b_dc_q2;
                b_dc_q1 = qq;
                b_dc_q2 = x;

                /* Auto-gain: target ~0.25-0.5 FS, tanpa agresif. */
                const float ref = (qmax > 0.0001f) ? qmax : 0.0001f;
                float g_des = 0.5f / ref;
                if (g_des < 1.0f) g_des = 1.0f;
                if (g_des > 256.0f) g_des = 256.0f;
                b_gain_slow += (g_des - b_gain_slow) * 0.05f;
                gain = b_gain_slow;

                float y = x * gain;
                if (y > 127.0f) y = 127.0f;
                if (y < -127.0f) y = -127.0f;
                s_audio_i8[out++] = (int8_t)lroundf(y);
            }

            static int c=0;
            if (++c%30==0) {
                ESP_LOGI("BYP","Imax=%.4f Qmax=%.4f sel=Q gain=%.1f out=%u",
                         imax, qmax, gain, (unsigned)out);
            }
            if (out > 0) {
                audio_out_push(s_audio_i8, out);
            }
        }
#else
        dsp_chain_process_block(s_block, frames);
#endif

        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_diag_t0_us >= DSP_DIAG_PERIOD_US) {
            const double n = (s_frames > 0) ? (double)s_frames : 1.0;
            const uint32_t aud = audio_out_update_count();
            ESP_LOGI(TAG,
                     "I/Q %" PRIu32 " Hz (target %d) | mean I %+.3f Q %+.3f | "
                     "peak I %.3f Q %.3f | apk %.3f | g %.1f | blk %" PRIu32
                     " | ovf %" PRIu32 " | xrun %" PRIu32 " | aud %" PRIu32
                     " Hz | heap %u",
                     rx_adc_measured_fs_hz(), CONFIG_SDR_ADC_FS_HZ,
                     s_sum_i / n, s_sum_q / n, s_peak_i, s_peak_q,
                     s_audio_peak, s_agc_gain, s_blocks,
                     rx_adc_overrun_count(),
                     audio_out_xrun_count(),
                     (aud - s_aud_prev) / (DSP_DIAG_PERIOD_US / 1000000),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_aud_prev = aud;
            s_sum_i = 0.0;
            s_sum_q = 0.0;
            s_peak_i = 0.0f;
            s_peak_q = 0.0f;
            s_audio_peak = 0.0f;
            s_blocks = 0;
            s_frames = 0;
            s_diag_t0_us = now_us;
        }
    }
}
