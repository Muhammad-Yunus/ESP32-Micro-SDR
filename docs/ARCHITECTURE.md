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
│               → BFO (USB/LSB) → audio LPF → AGC → volume → int16             │
│         │                         │                                          │
│         │ audio_q (ptr blok,      │ fft_snap_q (ptr snapshot, len 3,         │
│         │ len 8, drop-BARU)       │ drop-LAMA, kirim timeout 0)              │
│         ▼                         ▼                                          │
│  audio_feed_task (prio 11)            │ pool 3×1024 frame IQ @12 kHz          │
│    bsp_audio_pcm_write() ──► DAC1 GPIO26 ──► FM8002A ──► speaker              │
│    (BSP resample 48k → 200k, I2S0 internal)                                  │
└──────────────────────────────────────────────────────────────┬───────────────┘
                                                               │ pointer
┌────────────────────────── CORE 0 (UI) ───────────────────────┴───────────────┐
│  lvgl_task (prio 4, BSP)                                                     │
│    lv_timer 30 ms: drain fft_snap_q terbaru → spectrum FFT (esp-dsp)         │
│                 → render spectrum bar + waterfall                            │
│    touch: tombol ±/step/mode/volume, keypad entry, drag waterfall → offset   │
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

Aturan:
- Core 1 hanya berisi dua task di atas. Tidak ada logging berat, blocking file,
  atau LVGL di Core 1.
- Tidak ada `vTaskDelay` di `sdr_rx_task` — loop dipacing oleh blocking
  `adc_continuous_read()`.
- `audio_feed_task` dipacing oleh blocking `bsp_audio_pcm_write()`.

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
│   ├── sdr_config.h                # struct config runtime + API akses atomik (header-only boleh)
│   ├── rx_adc.c / rx_adc.h         # adc_continuous + deinterleave I/Q
│   ├── dsp_chain.c / dsp_chain.h   # seluruh DSP per blok + sdr_rx_task
│   ├── audio_out.c / audio_out.h   # pool blok audio + audio_feed_task
│   ├── spectrum.c / spectrum.h     # FFT esp-dsp → dB bins (dipanggil dari Core 0)
│   └── ui/
│       ├── ui_main.c / ui_main.h   # layout 480×320, freq text, tombol, keypad, timer UI
│       ├── ui_scope.c / ui_scope.h # widget spectrum bar + waterfall
│       └── ui_colors.c / ui_colors.h # LUT colormap waterfall (RGB565, 32 entri)
└── components/
    └── bsp_esp323248s035/          # BSP disalin apa adanya dari repo referensi; JANGAN dimodifikasi
```

### Batas modul (mencegah pembagian yang salah)

| Modul | Boleh menyentuh | DILARANG menyentuh |
| :--- | :--- | :--- |
| `rx_adc` | `esp_adc/adc_continuous.h`, GPIO 35/36 via ADC | driver DAC, LVGL, memori milik modul lain |
| `dsp_chain` | buffer IQ masukan, state filter/NCO, `sdr_config` (baca snapshot) | API BSP, LVGL, adc_continuous langsung |
| `audio_out` | pool & antrean audio, `bsp_audio_pcm_*` | ADC, DSP state, LVGL |
| `spectrum` | pool snapshot, buffer FFT miliknya, memanggil `ui_scope_push` | antrean audio, ADC |
| `ui_*` | LVGL, `sdr_config_set_*`, `ui_scope_push` | panggilan blocking DSP/ADC, `bsp_audio_pcm_write` |
| `sdr_config` | satu-satunya struktur yang dibaca/ditulis lintas core | — |
| BSP (`bsp_*`) | dianggap **black box**; hanya API publik di `bsp_esp323248s035.h` | **Jangan ubah kode BSP**; jangan panggil `bsp_init()` |

### Fungsi publik minimum per modul

```c
/* rx_adc.h */
esp_err_t rx_adc_init(void);                       // handle + config + start
void      rx_adc_deinit(void);
/* Blocking read satu blok; mengembalikan jumlah frame IQ (bukan byte raw). */
size_t    rx_adc_read(iq_t *out, size_t max_frames);
uint32_t  rx_adc_measured_fs_hz(void);             // rate aktual hasil pengukuran

/* dsp_chain.h */
esp_err_t dsp_chain_init(void);                    // load filter, LUT, state reset
void      sdr_rx_task(void *arg);                  // loop task core 1
void      dsp_chain_reset(void);

/* audio_out.h */
esp_err_t audio_out_init(void);                    // pool + antrean + task
bool      audio_out_push(const int16_t *pcm, size_t n);  // timeout 0; false = drop+xrun++
uint32_t  audio_out_xrun_count(void);

/* spectrum.h */
esp_err_t spectrum_init(void);                     // dsps_fft2r_init_fc32 + window + pool
void      spectrum_ingest(const iq_t *snapshot1024); // panggil di Core 0
bool      spectrum_get_db(float *out_db, int *out_nbins); // drain terbaru → UI

/* sdr_config.h (lintas core) */
typedef struct {
    int32_t lo_label_hz;        // label visual, default 7030000 (bukan LO riil!)
    int32_t nco_offset_hz;      // −6000..+6000, tuning riil
    int32_t nco_step_hz;        // 10/50/100/500
    uint8_t mode;               // SDR_MODE_USB / SDR_MODE_LSB
    uint8_t volume_pct;         // 0..100
    uint8_t agc_target;         // level target AGC
    int16_t lo_i, lo_q;         // (placeholder kalibrasi I/Q — future)
} sdr_config_t;
void      sdr_config_get(sdr_config_t *out);       // snapshot atomik oleh DSP
void      sdr_config_set_offset(int32_t hz);       // ditulis oleh UI
void      sdr_config_set_mode(uint8_t mode);
/* ... setter lain; penulisannya di bawah spinlock/critical section singkat ... */
```

---

## 4. Antrean, Pool & Kepemilikan Memori

| Nama | Tipe | Kapasitas | Item | Policy drop | Pool |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `audio_q` | FreeRTOS queue | 8 | `int16_t*` (blok 256 sampel) | **drop BARU** + `xrun++` (timeout 0) | 8 blok statis/heap, 256 sampel tiap blok |
| `fft_snap_q` | FreeRTOS queue | 3 | `iq_t*` (snapshot 1024 frame @12 kHz) | **drop LAMA** (kirim: kosongkan 1 dulu, lalu send timeout 0) | 3 slot × 1024 × `iq_t` |
| ADC DMA pool | driver internal | 8 KB | byte raw ADC | `on_pool_ovf` → hitung overrun | `max_store_buf_size` |

Aturan kepemilikan:
- Item di antrean adalah **pointer ke pool** — tidak ada copy data besar antar
  task. Pengirim tidak boleh memakai slot yang masih berada di antrean.
- Buffer FFT (`s_fft`, window, `s_db`) dimiliki penuh oleh `spectrum` (heap
  internal, **aligned 16 byte** — syarat assembly esp-dsp).
- Buffer DSP kerja (FIR state, decimasi) dimiliki penuh oleh `dsp_chain`.
- `sdr_config` dilindungi critical-section singkat (atau field atomik per
  scalar); tidak pernah dipertahankan sebagai pointer lintas task.

---

## 5. Aliran Data (satu blok audio)

1. DMA ADC mengisi pool internal driver (frame campuran CH0/CH7, 2 byte per
   hasil — `type1`: 12-bit data + 4-bit channel).
2. `sdr_rx_task` memanggil `rx_adc_read()` (blocking); `rx_adc` menulis
   deinterleave ke buffer kerja `iq_t[]`, `(raw−2048)/2048`.
3. `dsp_chain` memproses per blok 256 frame (lihat §6), menulis `int16[]`.
4. `audio_out_push()` menaruh pointer blok di `audio_q` (timeout 0).
5. `audio_feed_task` memblokir pada `bsp_audio_pcm_write()` — pacing alami
   (DAC menguras pada 48 kHz setelah resample internal ke 200 kHz).
6. Terpisah: setiap 1024 frame **pasca-NCO** (decimasi 12 kHz), snapshot
   disalin ke slot pool dan pointer dikirim ke `fft_snap_q`.
7. Timer UI 30 ms di Core 0: ambil snapshot terbaru → window → FFT → dB →
   `ui_scope_push` → render bar + waterfall.

---

## 6. Rantai DSP (per blok 256 frame @ 48 kHz)

| # | Tahap | Rumus / detail | Output rate |
| :--- | :--- | :--- | :--- |
| 1 | Normalisasi | `x = (raw12 − 2048) / 2048` (di `rx_adc`) | 48 kHz |
| 2 | DC removal | `y[n] = x[n] − x[n−1] + R·y[n−1]`, `R = 0.996` (~30 Hz), per kanal, **sebelum** NCO | 48 kHz |
| 3 | NCO offset | `I' = I·c + Q·s`, `Q' = Q·c − I·s`, `φ += 2π·f_offset/fs`; sin/cos dari LUT 1024 + interp linear; step per-blok (fase kontinu) | 48 kHz |
| 4 | LPF kanal + decimasi | Halfband/double-halfband 2,7 kHz, 48→24→12 kHz (`dsps_fir_dec2_f32` / `dsps_fir_f32`), state tersimpan antar blok | 12 kHz |
| 5 | BFO ke-2 (pemilih sideband) | USB: `I' = I·c + Q·s`, `Q' = Q·c − I·s` dengan `φ_B += −2π·1500/fs`; LSB: tanda `+` pada maju fase (tukar tanda sin) | 12 kHz |
| 6 | Audio LPF | FIR ~2,0 kHz pada komponen I hasil mix (band audio 300–2700 terjemah ke ±1200 Hz) | 12 kHz |
| 7 | AGC + volume | env attack 5 ms / release 200 ms; `gain = clamp(target/env)`; smooth; kelipat ×volume; clip ke `int16` | 12 kHz |

Catatan desain:
- Struktur dua-mixer (Weaver) dipilih; Hilbert FIR adalah varian setara — jika
  suatu saat dipakai, tempatkan di tahap 5 tanpa mengubah modul lain.
- Urutan penting: DC block **sebelum** NCO; LPF kanal **antara** NCO utama dan
  BFO; jika digabung jadi satu NCO saja, LPF kanal harus ikut digeser — jangan.
- Semua operasi per-blok (vector), bukan per-sampel panggilan library — untuk
  cache & pipeline.

---

## 7. Urutan Inisialisasi (`app_main.c`) — WAJIB berurutan

```
1. bsp_tft_init()        // display
2. bsp_touch_init()      // GT911
3. bsp_led_init()        // opsional
4. bsp_audio_init()      // DAC1 (mengklaim I2S0 di belakang layar)
5. bsp_audio_pcm_start(SDR_AUDIO_FS_HZ)   // mis. 48000
6. bsp_lvgl_init()       // LVGL port + task core 0
7. spectrum_init()       // dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE), window, pool snapshot
8. audio_out_init()      // pool + audio_q + audio_feed_task(core1, prio 11)
9. dsp_chain_init()      // LUT NCO, koefisien FIR, state
10. ui_init()            // objek LVGL + lv_timer UI 30 ms
11. rx_adc_init()        // ADC DMA start — TERAKHIR, agar DSP siap saat data mengalir
12. xTaskCreatePinnedToCore(sdr_rx_task, "sdr_rx", 16384, NULL, 12, NULL, core 1)
```

DILARANG:
- Memanggil `bsp_init()` (secara tidak langsung menginisialisasi LDR pada
  ADC1_CH6 — bentrok dengan ADC1 DMA; lihat `bsp.c:23` pada repo referensi).
- Menginisialisasi ulang peripheral yang sudah diambil BSP (SPI2, I2C0, I2S0).

---

## 8. Layout UI (landscape 480×320)

```
┌────────────────────────────────────────────────────────────┐
│ 7.030.000  [+1.200]  USB   S7   vol ▮▮▮▯▯                 │  ~40 px
├────────────────────────────────────────────────────────────┤
│                                                            │
│              WATERFALL (canvas 464 × 64, RGB565)           │  ~64 px + margin
│                                                            │
├────────────────────────────────────────────────────────────┤
│ |||  ||||| |||  |  ||||| ||  |||  ||||| |||||  |||||| ||||| │  ~80 px (bar spektrum)
├────────────────────────────────────────────────────────────┤
│ [USB/LSB]  [−] [+]  step 100  [Keypad]  [Vol−] [Vol+]     │  ~56 px
└────────────────────────────────────────────────────────────┘
```

- Waterfall: `lv_canvas` RGB565 464×64 (≈ 60 KB); update = `memmove` turun
  satu baris + gambar baris baru + invalidate. Fallback RAM: 48 baris.
- Spektrum: digambar pada event `LV_EVENT_DRAW_MAIN` (tanpa buffer penuh).
- Drag horizontal di waterfall mengubah `nco_offset_hz` sesuai span ±3 kHz.
- Keypad LVGL mengubah `nco_offset_hz` atau `lo_label_hz` (mode entry dipilih
  tombol).

---

## 9. Kebijakan Konkurensi (ringkas)

| Situasi | Kebijakan |
| :--- | :--- |
| DSP kehilangan blok audio | Drop blok BARU, `xrun++` — tidak pernah memblokir menunggu UI |
| UI lambat | Snapshot FFT lama di-drop — audio tetap jalan |
| Menulis config dari UI | Critical section singkat per setter; DSP mengambil snapshot per blok |
| Logging di Core 1 | Hanya ESP_LOGE pada event langka; tanpa log per blok steady-state |
| Watchdog | Task WDT default; tidak ada loop tanpa blocking/pemulihan |
