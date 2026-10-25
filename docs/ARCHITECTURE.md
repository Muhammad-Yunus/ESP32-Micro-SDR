# ARCHITECTURE — ESP32-Micro-SDR

Dokumen ini mengikat **struktur modul, pembagian tugas FreeRTOS, kepemilikan
memori, dan aliran data**. Setiap modul baru HARUS mengikuti pembagian di §3 —
jangan memindahkan tanggung jawab antar file tanpa revisi dokumen ini.

---

## 1. Diagram Sistem

```
             ANALOG (eksternal)                 ESP32-3248S035 (firmware ini)
┌──────────────────────────────┐
│ Antenna → Preamp → Tayloe    │  I (bias ~1,65 V)  GPIO36/ADC1_CH0 ┐
│ QSD + LPF audio + LO Si5351  │  Q (bias ~1,65 V)  GPIO35/ADC1_CH7 ┤ ADC DMA
│ (LO dikontrol alat terpisah) │                                     │ (adc_continuous)
└──────────────────────────────┘                                     ▼
┌────────────────────────── CORE 1 (real-time) ────────────────────────────────┐
│  sdr_rx_task (prio 12)                                                       │
│    rx_adc: read DMA → deinterleave I/Q → float                               │
│    dsp_chain: DC block → NCO offset → LPF + decim 48→24→12k                  │
│               → BFO (USB/LSB) / envelope (AM) → audio LPF → AGC → volume │
│         │                         │                                          │
│         │ audio_q (ptr blok,      │ fft_snap_q (ptr snapshot, len 3,         │
│         │ len 8, drop-BARU)       │ drop-LAMA, kirim timeout 0)              │
│         ▼                         ▼                                          │
│  audio_feed_task (prio 11)            │ pool 3×1024 frame IQ @12 kHz          │
│    sdm_channel_set_pulse_density() ──► SDM 1-bit GPIO26 ──► FM8002A ──► speaker│
│    (pacing task audio; I2S0/DAC-DMA TIDAK dipakai — bentrok adc_continuous)   │
└──────────────────────────────────────────────────────────────┬───────────────┘
                                                               │ pointer
┌────────────────────────── CORE 0 (UI) ───────────────────────┴───────────────┐
│  lvgl_task (prio 4, BSP)                                                     │
│    lv_timer 30 ms: drain fft_snap_q terbaru → spectrum FFT (esp-dsp)         │
│                 → render spectrum bar + waterfall                            │
│    touch: tombol ±/step/mode/volume, keypad entry, drag waterfall → offset│
│    gear → modal Settings: mode FFT bin (1024/2048) + span LO (±250 Hz…10k) │
│    sdr_config_set_*() ──(atomik)──► dibaca sdr_rx_task per blok              │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Tabel Task (sumber kebenaran penamaan & prioritas)

| Task | Core | Prio | Stack | Entry point | Dibuat di |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `sdr_rx_task` | 1 | **12** | 16 KB | `dsp_chain.c` | `app_main.c` |
| `audio_feed_task` | 1 | **11** | 6 KB | `audio_out.c` | `app_main.c` |
| `lvgl_task` | 0 | 4 | 6 KB | BSP (`bsp_lvgl.c`) | `bsp_lvgl_init()` |
| `app_main` | 0 | 1 | 4 KB | `app_main.c` | sistem |
| `selftest` | 1 | 12 | 8 KB (**statis**) | `selftest.c` | `app_main.c` → `selftest_start()` (**hanya build `CONFIG_SDR_SELFTEST=y`**) |

Aturan:
- Core 1 hanya berisi dua task di atas. Tidak ada logging berat, blocking file,
  atau LVGL di Core 1.
- Tidak ada `vTaskDelay` di `sdr_rx_task` — loop dipacing oleh blocking
  `adc_continuous_read()`.
- `audio_feed_task` dipacing oleh laju audio (menulis satu sampel `int8` ke
  kanal SDM setiap periode sampel; sumber pacing F4).
- Task `selftest` adalah pengecualian satu-satunya: ia **menggantikan**
  `sdr_rx_task` (bukan berdampingan) dan hanya ada pada build self-test. Stack
  & TCB-nya statis karena heap internal setelah `spectrum_init()` sudah
  terfragmentasi (stack dinamis 8 KB gagal dibuat, stack 4 KB overflow →
  panic StoreProhibited).


---

## 3. Struktur Folder & Tanggung Jawab Modul

```
firmware/
├── CMakeLists.txt                  # project(firmware) — jangan diubah
├── sdkconfig.defaults              # konfigurasi build (lihat TECH_STACK.md §5)
├── main/
│   ├── CMakeLists.txt              # SRCS semua .c di bawah; REQUIRES: bsp_esp323248s035, esp_dsp, esp_adc, esp_timer, driver
│   ├── idf_component.yml           # espressif/esp-dsp ^1.8.2
│   ├── Kconfig.projbuild           # SELURUH parameter runtime (fs, ukuran FFT, span, atten, dst.)
│   ├── app_main.c                  # urutan init + pembuatan task/queue (lihat §7)
│   ├── sdr_config.c / sdr_config.h # struct config runtime + API akses atomik
│   ├── rx_adc.c / rx_adc.h         # adc_continuous + deinterleave I/Q
│   ├── dsp_chain.c / dsp_chain.h   # seluruh DSP per blok + sdr_rx_task
│   ├── audio_out.c / audio_out.h   # kanal SDM (GPIO26) + pool blok audio + audio_feed_task
│   ├── spectrum.c / spectrum.h     # FFT esp-dsp → dB bins (dipanggil dari Core 0)
│   ├── iq_gen.c / iq_gen.h         # generator I/Q sintetis (HANYA build self-test)
│   ├── selftest.c / selftest.h     # suite uji DSP on-target (HANYA build self-test)
│   └── ui/
│       ├── ui_main.c / ui_main.h   # layout 480×320, freq text, tombol, keypad, modal Settings
│       ├── ui_scope.c / ui_scope.h # widget spectrum bar + waterfall (skala runtime)
│       ├── ui_colors.c / ui_colors.h # LUT colormap waterfall (RGB565, 32 entri)
│       └── font_dseg7_36.c         # font 7-segmen DSEG7 (regenerasi: TECH_STACK §7)
└── components/
    └── bsp_esp323248s035/          # BSP disalin apa adanya dari repo referensi; JANGAN dimodifikasi
```

### Batas modul (mencegah pembagian yang salah)

| Modul | Boleh menyentuh | DILARANG menyentuh |
| :--- | :--- | :--- |
| `rx_adc` | `esp_adc/adc_continuous.h`, GPIO 35/36 via ADC | driver DAC/SDM, LVGL, memori milik modul lain |
| `dsp_chain` | buffer IQ masukan, state filter/NCO, `sdr_config` (baca snapshot) | API BSP, LVGL, adc_continuous langsung |
| `audio_out` | kanal SDM (`esp_driver_sdm`), pool & antrean audio | ADC, DSP state, LVGL, `bsp_audio_*` |
| `spectrum` | pool snapshot, buffer FFT miliknya, memanggil `ui_scope_push` | antrean audio, ADC |
| `ui_*` | LVGL, `sdr_config_set_*`, `ui_scope_push` | panggilan blocking DSP/ADC, `sdm_channel_set_pulse_density` |
| `sdr_config` | satu-satunya struktur yang dibaca/ditulis lintas core | — |
| `iq_gen` | generator sinyal sintetis (tanpa hardware) | API DSP/UI, ADC |
| `selftest` | meng-orak `dsp_chain_process_block()`, `sdr_config_set_*`, snapshot spektrum | ADC, audio_out, LVGL, `dsp_chain` internals |
| BSP (`bsp_*`) | dianggap **black box**; hanya API publik di `bsp_esp323248s035.h` | **Jangan ubah kode BSP**; jangan panggil `bsp_init()`, `bsp_audio_init()`, `bsp_ldr_init()` |

### Fungsi publik minimum per modul

```c
/* rx_adc.h */
typedef struct { float i; float q; } iq_t;
esp_err_t rx_adc_init(void);                       // handle + config + start
void      rx_adc_deinit(void);
/* Blocking read satu blok; mengembalikan jumlah frame IQ (bukan byte raw). */
size_t    rx_adc_read(iq_t *out, size_t max_frames);
uint32_t  rx_adc_measured_fs_hz(void);             // rate aktual hasil pengukuran
uint32_t  rx_adc_overrun_count(void);              // counter on_pool_ovf

/* dsp_chain.h */
esp_err_t dsp_chain_init(void);                    // load filter, LUT, state reset
void      sdr_rx_task(void *arg);                  // loop task core 1
void      dsp_chain_reset(void);

/* audio_out.h */
esp_err_t audio_out_init(void);                    // kanal SDM + pool + antrean (task di app_main)
bool      audio_out_push(const int8_t *pcm, size_t n);   // timeout 0; false = drop+xrun++
uint32_t  audio_out_xrun_count(void);
uint32_t  audio_out_update_count(void);            // total update density (diagnostik pacing)
/* Test hook (CONVENTIONS §5): CONFIG_SDR_AUDIO_TONE_HZ > 0 → audio_feed_task
   mensintesis sinus langsung pada rate density (amplitudo x volume_pct). */

/* spectrum.h */
#define SPECTRUM_FFT_N         2048   // NFFT zero-pad (>= CONFIG_SDR_FFT_N)
#define SPECTRUM_MAX_OUT_NBINS 2048   // mode 2048 = band penuh +/-FS/2
esp_err_t spectrum_init(void);                     // dsps_fft2r_init_fc32 + window + pool
void      spectrum_ingest(const iq_t *snapshot1024); // panggil di Core 1 (dsp_chain)
bool      spectrum_get_db(float *out_db, int *out_nbins); // drain terbaru → UI
                                               // out_db: *out_nbins nilai dB band
                                               // penuh terpusat DC (index tengah = DC)
void      spectrum_set_consume_locked(bool locked);   // kunci snapshot (self-test)
bool      spectrum_get_db_ex(float *out_db, int *out_nbins, bool ignore_lock);

/* dsp_chain.h — hook self-test (dipakai juga di build produksi, tanpa efek) */
size_t    dsp_chain_process_block(const iq_t *in, size_t frames); // proses 1 blok I/Q
void      dsp_chain_set_pcm_sink(dsp_pcm_sink_t sink);  // NULL = audio_out_push
void      dsp_chain_set_fs_hz(uint32_t fs_hz);           // 0 = laju terukur ADC
#define DSP_AGC_GAIN_MIN  (1.0f / 64.0f)                  // −36 dB
#define DSP_AGC_GAIN_MAX  64.0f                          // +36 dB (batas atas AGC)

/* iq_gen.h (hanya build self-test) */
void      iq_gen_cfg_default(iq_gen_cfg_t *cfg, float fs_hz);
void      iq_gen_init(iq_gen_t *gen, const iq_gen_cfg_t *cfg);
size_t    iq_gen_fill(iq_gen_t *gen, iq_t *out, size_t frames); // tone/DC/noise sintetis

/* selftest.h (hanya build self-test) */
bool      selftest_start(void);                        // buat task selftest (Core 1)

/* sdr_config.h (lintas core) */
typedef struct {
    int32_t lo_label_hz;        // label visual, default 7203000 (bukan LO riil!)
    int32_t nco_offset_hz;      // −CONFIG_SDR_NCO_LIMIT_HZ..+limit (default 2 MHz)
    int32_t nco_step_hz;        // 10/50/100/500/1k/10k/100k/1M
    uint8_t mode;               // SDR_MODE_USB / SDR_MODE_LSB / SDR_MODE_AM (default AM)
    uint8_t volume_pct;         // 0..100 (default 0 = mute, naik lewat Vol+/Vol−)
    uint8_t agc_target;         // level target AGC
    int16_t lo_i, lo_q;         // (placeholder kalibrasi I/Q — future)
    uint16_t fft_bins;          // 1024 / 2048 (mode keluaran spektrum, F6)
    int32_t span_half_hz;       // ± Hz dari LO: 250/500/1000/2000/5000/10000
} sdr_config_t;
void      sdr_config_get(sdr_config_t *out);       // snapshot atomik oleh DSP
void      sdr_config_set_offset(int32_t hz);       // ditulis oleh UI
void      sdr_config_set_mode(uint8_t mode);
void      sdr_config_set_fft_bins(uint16_t bins);  // hanya 1024/2048 diterima
void      sdr_config_set_span_half(int32_t hz);     // hanya preset yang valid
uint16_t  sdr_config_fft_bins_get(void);           // mode aktif
int32_t   sdr_config_span_half_get(void);          // preset aktif (mentah)
int32_t   sdr_config_span_half_effective(void);    // preset di-clamp ke FS/2
const int32_t *sdr_config_span_options(size_t *n); // daftar preset span
/* ... setter lain; penulisannya di bawah spinlock/critical section singkat ... */
```

Catatan mode FFT bin: `sdr_config.fft_bins` **tidak** memanggil ulang
`dsps_fft2r_init_fc32` (tabel twiddle esp-dsp bersifat global dan hanya boleh
di-init sekali). Mode 1024 diambil dari bin **genap** DFT-2048 yang sudah ada
(`X₂₀₄₈[2m] == X₁₀₂₄[m]`), sehingga identik dengan DFT-1024 tanpa tambahan
memori maupun inisialisasi.

---

## 4. Antrean, Pool & Kepemilikan Memori

| Nama | Tipe | Kapasitas | Item | Policy drop | Pool |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `audio_q` | FreeRTOS queue | 8 | `int8_t*` (blok 256 sampel) | **drop BARU** + `xrun++` (timeout 0) | 10 blok heap internal × 256 sampel (8 slot antrean + 1 perakitan + 1 sedang diputar) |
| `fft_snap_q` | FreeRTOS queue | 3 | `iq_t*` (snapshot 1024 frame @12 kHz) | **drop LAMA** (kirim: kosongkan 1 dulu, lalu send timeout 0) | 3 slot × 1024 × `iq_t` |
| ADC DMA pool | driver internal | 8 KB | byte raw ADC | `on_pool_ovf` → hitung overrun | `max_store_buf_size` |

Aturan kepemilikan:
- Item di antrean adalah **pointer ke pool** — tidak ada copy data besar antar
  task. Pengirim tidak boleh memakai slot yang masih berada di antrean.
- Buffer FFT (`s_fft`, window, `s_db`) dimiliki penuh oleh `spectrum` (heap
  internal, **aligned 16 byte** — syarat assembly esp-dsp). `s_db` menampung
  `SPECTRUM_MAX_OUT_NBINS` = 2048 float (8 KB BSS) agar mode bin 1024/2048
  tidak perlu alokasi ulang.
- Kanvas waterfall (`480 × 60` RGB565 = 56 KB) dialokasikan sekali di
  `ui_scope_init()`; bila gagal → fallback 44 baris (41 KB), bila tetap gagal
  → `ui_scope_init()` mengembalikan `ESP_ERR_NO_MEM` (UI tetap hidup tanpa
  waterfall).
- Buffer DSP kerja (FIR state, decimasi) dimiliki penuh oleh `dsp_chain`.
- `sdr_config` dilindungi critical-section singkat (atau field atomik per
  scalar); tidak pernah dipertahankan sebagai pointer lintas task.

Ukuran RAM terverifikasi di board (ESP32 klasik, tanpa PSRAM):
`heap_init` ⇒ DRAM dinamis 75 KiB (`At 3FFCD078`); free heap setelah boot
= **81 908 B**, steady-state = **86 628 B** (iris `sdr_rx` per ~2 s).

---

## 5. Aliran Data (satu blok audio)

1. DMA ADC mengisi pool internal driver (frame campuran CH0/CH7, 2 byte per
   hasil — `type1`: 12-bit data + 4-bit channel). Urutan dalam stream bisa
   terbalik dari array pola → pairing I/Q harus tak bergantung urutan.
2. `sdr_rx_task` memanggil `rx_adc_read()` (blocking); `rx_adc` menulis
   deinterleave ke buffer kerja `iq_t[]`, `(raw−2048)/2048`.
3. `dsp_chain` memproses per blok 256 frame (lihat §6), menulis `int8[]`.
4. `audio_out_push()` menaruh pointer blok di `audio_q` (timeout 0).
5. `audio_feed_task` memutar blok ke kanal SDM dengan interpolasi linear ×4
   (update density 48 kHz — citra ZOH di luar band audio): satu
   `sdm_channel_set_pulse_density()` per update, dipacing deadline absolut.
   Sebelum ditulis, density dibagi `CONFIG_SDR_AUDIO_SCALE_DIV` (default 4 =
   −12 dB) — attenuasi output speaker; tidak menyentuh skala int8 jalur DSP
   maupun ekspektasi self-test AGC.
6. Terpisah: setiap 1024 frame **pasca-NCO** (decimasi 12 kHz), snapshot
   disalin ke slot pool dan pointer dikirim ke `fft_snap_q`.
7. Timer UI 30 ms di Core 0: ambil snapshot terbaru → window → FFT → dB →
   `ui_scope_push` → render bar + waterfall.

---

## 6. Rantai DSP (per blok 256 frame @ 48 kHz)

| # | Tahap | Rumus / detail | Output rate |
| :--- | :--- | :--- | :--- |
| 1 | Normalisasi | `x = (raw12 − 2048) / 2048` (di `rx_adc`) | 48 kHz |
| 2 | DC removal | `y[n] = x[n] − x[n−1] + R·y[n−1]`, `R = 0.996` (~30 Hz), per kanal, **sebelum** NCO. **Dilewati pada mode AM** (carrier AM = komponen DC kanal I; carrier dibuang oleh DC-blocker khusus envelope di tahap 5) | 48 kHz |
| 3 | NCO offset | `I' = I·c + Q·s`, `Q' = Q·c − I·s`, `φ += 2π·f_offset/fs`; sin/cos dari LUT 1024 + interp linear; step per-blok (fase kontinu) | 48 kHz |
| 4 | LPF kanal + decimasi | Halfband/double-halfband 2,7 kHz, 48→24→12 kHz (`dsps_fir_dec2_f32` / `dsps_fir_f32`), state tersimpan antar blok | 12 kHz |
| 5 | BFO ke-2 (pemilih sideband) / envelope (AM) / FM discriminator | SSB: `I' = I·c + Q·s`, `Q' = Q·c − I·s` dengan `φ_B += 2π·f_BFO/fs`; **USB: `f_BFO = +1500`** (frekuensi turun → sideband atas lolos), **LSB: `f_BFO = −1500`** (frekuensi naik → sideband bawah lolos). AM: `env = sqrt(I² + Q²)` (envelope detection) + DC-blocker envelope buang carrier. FM: selisih fase antar sampel: `dφ[n] = atan2(I[n]·Q[n−1] − Q[n]·I[n−1], I[n]·I[n−1] + Q[n]·Q[n−1])`, keluaran = `dφ/(2π) · (1/fm_dev_hz) · K` (deviasi `fm_dev_hz` → keluaran unit). **DC blocker tahap 2 dilewati pada mode FM & AM** | 12 kHz |
| 6 | Audio LPF | FIR ~2,0 kHz pada komponen I hasil mix (band audio 300–2700 terjemah ke ±1200 Hz) | 12 kHz |
| 7 | AGC + volume + squelch | env attack 5 ms / release 200 ms; `gain = clamp(target/env, 1/64, SDR_AGC_GAIN_MAX)`; smooth; kelipat ×volume; clip ke `int8` (pulse density SDM). **Squelch otomatis** (hanya saat AGC aktif): envelope pra-gain < `target/gain_max` (= lantai AGC, §FR-5) → gerbang output difade ke 0 dalam ~330 ms; sinyal → buka ~3 ms. Mode gain manual tidak digate | 12 kHz |

Catatan desain:
- Struktur dua-mixer (Weaver) dipilih; Hilbert FIR adalah varian setara — jika
  suatu saat dipakai, tempatkan di tahap 5 tanpa mengubah modul lain.
- Urutan penting: DC block **sebelum** NCO; LPF kanal **antara** NCO utama dan
  BFO; jika digabung jadi satu NCO saja, LPF kanal harus ikut digeser — jangan.
- **Mode AM (envelope detection)**: pada IQ @12 kHz tahap 5 menghitung
  `env = sqrt(I²+Q²)`. Carrier AM (siaran) berupa komponen DC di kanal I,
  sehingga DC-blocker tahap 2 **dilewati** (agar carrier tetap tersedia);
   carrier kemudian dibuang oleh DC-blocker khusus envelope
   (`env − x[n−1] + R·y[n−1]`, tahap 5) sebelum audio LPF. Pengujian self-test
   T06a memakai AGC aktif karena amplitudo envelope AM kecil terkuantisasi
   ke 0 pada int8 tanpa penguatan (lihat PRD §6.1 T-AM).
- **Mode FM (quadrature discriminator)**: tahap 5 menghitung selisih fase
  antar sampel berurutan: `dφ = atan2(I[n]·Q[n−1] − Q[n]·I[n−1], I[n]·I[n−1]
  + Q[n]·Q[n−1])`. Keluaran = `dφ / (2π) · (1/fm_dev_hz)` sehingga deviasi
  `fm_dev_hz` (default ±5 kHz, `CONFIG_SDR_FM_DEV_HZ`) menghasilkan keluaran
  ±1.0 (skala penuh `int8` via AGC). Carrier FM tanpa modulasi menghasilkan
  `dφ ≈ 0` (keluaran ≈ 0). DC blocker tahap 2 dilewati (carrier FM = DC kanal
  I, identik AM). Self-test T07c menguji carrier FM + modulasi 1 kHz.
- Semua operasi per-blok (vector), bukan per-sampel panggilan library — untuk
  cache & pipeline.
- Implementasi (F5, `dsp_chain.c`): LUT sin 1024 + interpolasi linear (cos =
  sin + 90°); fase NCO/BFO satuan siklus, kontinu antar blok; step NCO/BFO
  memakai `rx_adc_measured_fs_hz()` (fallback target); LPF = windowed sinc
  Blackman yang didesain saat init (halfband-equivalent 32 tap tahap 4a/4b,
  audio 64 tap tahap 6); keluaran `int8` di-push ke `audio_out_push` (64
  sampel per blok 256 frame).
- Parameter tahap §6 dikumpulkan di Kconfig (NFR-9): `SDR_DC_POLE_R`,
  `SDR_CH_LPF_HZ`, `SDR_AUDIO_LPF_HZ`, `SDR_AGC_TARGET` (0 = AGC off),
  `SDR_AGC_ATTACK_MS`, `SDR_AGC_RELEASE_MS`, `SDR_AGC_GAIN_MAX` (batas gain
   linear, default 512 = +54 dB), `SDR_FM_DEV_HZ` (deviasi FM, default 5000);
   override runtime `agc_target` lewat
  `sdr_config`. Target AGC hanya tercapai bila level input ≥ target/gain_max
  (self-test T09/T09b/T10 mengukur batas ini).
- Parameter display (F6): `SDR_FFT_N` (jendela analisis, 1024),
  `SDR_AUDIO_FS_HZ` (12 000 Hz — penentu lebar bin = FS/jumlah bin dan batas
  span FS/2 = 6 kHz), `SDR_DISPLAY_SPAN_HZ` (span awal ±50 kHz, dapat diubah
  saat runtime dari Settings modal; efektif di-clamp ke FS/2).

---

## 7. Urutan Inisialisasi (`app_main.c`) — WAJIB berurutan

```
1. bsp_tft_init()        // display
2. bsp_touch_init()      // GT911
3. bsp_led_init()        // opsional
4. bsp_lvgl_init()       // LVGL port + task core 0
5. sdr_config_init()     // defaults dari Kconfig (sebelum UI)
6. ui_init()             // objek LVGL + lv_timer UI (30 ms)
7. dsp_chain_init()      // LUT NCO, koefisien FIR, state (F5)
8. spectrum_init()       // dsps_fft2r_init_fc32, window, pool snapshot (F6)
9. audio_out_init()      // kanal SDM GPIO26 + pool + antrean (F4)
10. xTaskCreatePinnedToCore(audio_feed_task, "audio_feed", 6144, NULL, 11, NULL, core 1)
11. rx_adc_init()        // ADC DMA start — TERAKHIR, agar konsumen data siap
12. xTaskCreatePinnedToCore(sdr_rx_task, "sdr_rx", 16384, NULL, 12, NULL, core 1)
```

Catatan: langkah 7–10 baru aktif saat fiturnya dibangun (F4/F5/F6); urutan relatifnya tetap.
Langkah 11 dan 12 sudah aktif sejak F3.
`spectrum_init()` memanggil `dsps_fft2r_init_fc32()` **sekali saja** — tabel
twiddle esp-dsp bersifat global; inisialisasi ulang akan menimpa tabel dan
menghasilkan spektrum salah. Mode bin 1024/2048 hanya memilih cara *menyusun
keluaran* dari FFT yang sama (lihat §3).

Pada build self-test (`CONFIG_SDR_SELFTEST=y`) langkah 11–12 diganti:
`rx_adc_init()` **tidak dipanggil** (GPIO35/GPIO36 bebas) dan
`selftest_start()` membuat task `selftest` (Core 1, prio 12, stack statis
8 KB). Rantai DSP tetap yang sama — hanya sumber bloknya `iq_gen` dan
sink PCM-nya `dsp_chain_set_pcm_sink()`.

DILARANG:
- Memanggil `bsp_init()` (secara tidak langsung menginisialisasi LDR pada
  ADC1_CH6 — bentrok dengan ADC1 DMA; lihat `bsp.c:23` pada repo referensi).
- Memanggil `bsp_audio_init()`/`bsp_audio_pcm_*` — memakai `dac_continuous`
  yang mengklaim I2S0, sama dengan `adc_continuous` (acquire kedua gagal).
- Memanggil `bsp_ldr_init()` — memakai `adc_oneshot` pada ADC1_CH6 (bentrok
  dengan ADC1 continuous DMA).
- Menginisialisasi ulang peripheral yang sudah diambil BSP (SPI2, I2C0) atau
  I2S0 (dipakai ADC DMA).

---

## 8. Layout UI (landscape 480×320)

```
┌────────────────────────────────────────────────────────────┐
│ 7.203.000  [+1.200]  AM  vol ▮▮▮▯▯                    ⚙  │  52 px (top bar)
├────────────────────────────────────────────────────────────┤
│ dBFS  |||  ||||| |||  |  ||||| ||  |||  ||||| |||||  |||||  │  56..184 (plot 442×128)
│  -20  ─────────────────────────────────────────────────    │  + strip label X 18 px
├────────────────────────────────────────────────────────────┤
│              WATERFALL (canvas 480 × 60, RGB565)           │  204..264
├────────────────────────────────────────────────────────────┤
│ [USB/LSB/AM]  [−] [+]  step 100  [Keypad]  [Vol−] [Vol+]  │  56 px (bottom bar)
└────────────────────────────────────────────────────────────┘
```

Geometri (`ui_scope.c`): top bar 52 px, chart `SCOPE_Y=56` `BAR_H=128`,
strip label X `XLABEL_H=18`, jarak `BAR_GAP=2` ⇒ waterfall mulai `WF_Y=204`.
Lebar waterfall = `WF_W = SCOPE_W + YLABEL_W + PLOT_PAD_R = 480` (full LCD),
sedangkan area plot chart 442 px (x30..471) + padding kanan 8 px.

- Waterfall: `lv_canvas` RGB565 480×60 (≈ 56 KB); update = `memmove` turun
  satu baris + gambar baris baru + invalidate. Fallback RAM: 44 baris (41 KB).
  Jumlah baris (`CONFIG_SDR_WATERFALL_ROWS` = 60) dipilih agar kanvas tepat
  mengisi area di atas bottom bar (y 204..264) tanpa baris tertutup.
- Spektrum: line chart kuning pucat 1 px digambar pada event
  `LV_EVENT_DRAW_MAIN` (polyline ≤ 442 titik, tanpa buffer penuh) — **satu
  titik per bin FFT**: nilai asli tanpa interpolasi, x = posisi frekuensi
  bin. Window Hann 1024 @12 kHz di-zero-pad ke FFT 2048 (`SPECTRUM_FFT_N`)
  → 5,86 Hz/bin; RBW asli tetap 11,7 Hz. Keluaran = **band penuh
  terpusat DC** (index tengah = DC, index 0 = −FS/2 = −6 kHz), jumlah bin
  runtime mengikuti mode FFT dari Settings modal: 2048 bin (5,86 Hz/bin) atau
  1024 bin (11,72 Hz/bin — sama dengan DFT-1024 karena
  `X₂₀₄₈[2m] == X₁₀₂₄[m]`). Bila bin dalam tampilan melebihi jumlah piksel,
  chart digambar sebagai envelope maksimum per piksel.
  Nilai per piksel (persentil floor) = maksimum bin dalam rentang pikselnya.
- **Skala tampilan runtime** (diatur dari ikon gear → Settings modal, bukan
  lagi build-time): span half-width ± Hz dari `sdr_config.span_half_hz`
  (preset 250/500/1k/2k/5k/10k Hz) dan mode FFT bin (1024/2048). Span
  di-clamp ke FS/2 = 6 kHz (`sdr_config_span_half_effective()`) karena
  sampler 12 kHz hanya mencakup ±6 kHz; preset 10 kHz tetap terpilih tetapi
  readout menampilkan nilai efektif. Grid vertikal & label Hz ikut span
  aktif: {−S, −S/2, 0, +S/2, +S} (S = span efektif). `CONFIG_SDR_DISPLAY_SPAN_HZ`
  kini hanya nilai awal saat boot.
- Skala Y spektrum: **dBFS**, dasar dinamis = persentil 25 noise floor −
  40 dB (clamp [−180, 0]) sehingga trace selalu melayang 40 dB di atas
  dasar chart; atas +10 dBFS. Grid horizontal otomatis (step 10/20/50 dB)
  + label di strip kiri (satuan `dBFS` di pojok kiri atas, label 0
  dilewati agar ruangnya untuk satuan).
- Drag horizontal di waterfall mengubah `nco_offset_hz` sesuai span aktif.
- Keypad LVGL mengubah `nco_offset_hz` atau `lo_label_hz` (mode entry dipilih
  tombol).

### Modal Settings (ikon gear, top bar kanan)

| Baris | Kontrol | Nilai | Efek |
| :--- | :--- | :--- | :--- |
| 1 | `AGC` | switch on/off | `sdr_config_set_agc_enabled()`; off = gain manual |
| 2 | `Gain` | slider −24…+54 dB | gain manual (aktif saat AGC off) |
| 3 | `Audio LPF` | slider 100…6000 Hz | desain ulang FIR audio LPF runtime |
| 4 | `ADC atten` | preset 0 / 2.5 / 6 / 12 dB | attenuation ADC I/Q |
| 5 | `Source` | preset IQ / Q / I | pilih kanal I/Q yang didemod |
| 6 | `Test` | switch + preset 250 / 1k / 2k / 4k | tone sintesis DDS langsung ke SDM (bypass antrean DSP), amplitudo × volume; OFF = demod normal |
| 7 | `Bypass` | switch on/off | `sdr_config_set_dsp_bypass()`; ON = jalur mentah ADC→speaker (I channel, DC block, decim 4:1, auto-gain → int8 → SDM); skip seluruh rantai DSP (NCO/BFO/LPF/AGC) |
| 8 | `FFT bin` | 1024 / 2048 (checkable) | `sdr_config_set_fft_bins()` → lebar bin 11,72 / 5,86 Hz/bin; readout Hz/bin di bawah tombol |
| 9 | `LO span` | label + preset 250 / 500 / 1K / 2K / 5K / 10K | `sdr_config_set_span_half()` → zoom tampilan |
| 10 | `Scope` | switch on/off | `ui_scope_set_enabled()` → pause/resume timer spectrum; DSP tetap berjalan |

Setiap perubahan skala memanggil `ui_scope_refresh()`: sinkron span efektif,
`memset` buffer waterfall (baris lama memakai skala sebelumnya), invalidate
kanvas & objek chart.


---

## 9. Kebijakan Konkurensi (ringkas)

| Situasi | Kebijakan |
| :--- | :--- |
| DSP kehilangan blok audio | Drop blok BARU, `xrun++` — tidak pernah memblokir menunggu UI |
| UI lambat | Snapshot FFT lama di-drop — audio tetap jalan |
| Menulis config dari UI | Critical section singkat per setter; DSP mengambil snapshot per blok |
| Logging di Core 1 | Hanya ESP_LOGE pada event langka; tanpa log per blok steady-state |
| Watchdog | TWDT memantau IDLE0 (Core 0). Idle-check **Core 1 dinonaktifkan** (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`): Core 1 didedikasikan streaming audio — `audio_feed_task` memutar density dengan pacing spin per-update (20,8 µs) dan hampir tidak pernah memblokir saat antrean penuh, jadi IDLE1 memang tidak dijadwalkan by-design. `sdr_rx_task` tetap memblokir pada `rx_adc_read()` (DMA). |
