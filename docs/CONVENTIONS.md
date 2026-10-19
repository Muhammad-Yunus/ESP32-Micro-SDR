# CONVENTIONS — ESP32-Micro-SDR

Aturan kualitas kode. Berlaku untuk SELURUH file di `firmware/main/`.
Melanggar aturan di sini = revisi wajib sebelum dianggap selesai.

---

## 1. Bahasa, Gaya & Format

- Bahasa pemrograman: **C11** (bukan C++) — mengikuti BSP dan ekosistem ESP-IDF.
- Indentasi: **4 spasi**, tanpa tab. Baris maksimal ~100 karakter.
- Penamaan: `snake_case` untuk semua simbol.
  - Tipe: suffix `_t` (`sdr_config_t`, `iq_t`).
  - Variabel statis file: prefix `s_` (`s_nco_phase`, `s_adc`).
  - Makro/konstanta: `UPPER_SNAKE` dengan prefix `SDR_` atau `UI_` untuk
    milik aplikasi; konfigurasi build memakai prefix `CONFIG_SDR_` (Kconfig).
  - Nama task: `"sdr_rx"`, `"audio_feed"`.
- Header: `#pragma once` (bukan include guard konvensional).
- Setiap file diawali header SPDX:
  ```c
  /*
   * SPDX-License-Identifier: MIT
   */
  ```
- Include: urutan (1) header modul sendiri, (2) FreeRTOS/driver, (3) esp-idf,
  (4) pustaka eksternal (esp_dsp, lvgl), (5) standard. C-include dulu baru
  C++-gated header.
- Komentar hanya untuk **"mengapa"** (alasan desain, jebakan hardware).
  Jangan berkomentar "apa" yang sudah jelas dari kode. Semua komentar dalam
  Bahasa Inggris.

---

## 2. Penanganan Error

| Konteks | Aturan |
| :--- | :--- |
| Fungsi init | Gunakan `ESP_RETURN_ON_ERROR()` / `ESP_GOTO_ON_ERROR()` (header `esp_check.h`) — setara gaya BSP |
| `app_main` | `ESP_ERROR_CHECK()` boleh untuk langkah fatal-boot; setelah boot, jangan ada abort |
| Loop real-time (Core 1) | **Tidak boleh abort/mengulang tanpa batas** pada error; naikkan counter, log jarang (`ESP_LOGE` hanya saat transisi state), teruskan blok berikutnya |
| `adc_continuous_read` timeout | Tangani `ESP_ERR_TIMEOUT` sebagai idle, bukan error fatal |
| Pustaka esp-dsp | Cek return `esp_err_t` minimal saat init (mis. `dsps_fft2r_init_fc32`); pada jalur panas boleh diabaikan jika sudah tervalidasi |
| LVGL lock | Selalu `bsp_lvgl_lock()` sebelum menyentuh objek LVGL dari luar `lvgl_task`; `bsp_lvgl_unlock()` di finally-implied path |

---

## 3. Memori

- Alokasi: `heap_caps_malloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` untuk
  buffer DSP/UI; **buffer yang dipakai esp-dsp wajib aligned 16 byte**:
  ```c
  static float s_fft[SDR_FFT_N * 2] __attribute__((aligned(16)));
  ```
- **Tidak ada** `malloc`/`pvPortMalloc` di dalam loop per-blok atau ISR.
  Semua pool dialokasikan sekali saat init.
- Tidak ada VLA dan tidak ada penuh-alokasi-di-stack untuk buffer besar
  (> 1 KB di stack = salah; pakai heap statis).
- Tidak mengandalkan PSRAM (nonaktif dan tidak tersedia).
- Mengukur: tambahkan init-log `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`
  setelah setup — nilai dicatat di commit/test M3.

---

## 4. FreeRTOS

- Pembuatan task hanya di `app_main.c` (lihat ARCHITECTURE.md §7). Modul
  menyediakan entry point-nya sendiri, tetapi `xTaskCreatePinnedToCore`
  dipanggil satu tempat.
- Core & prioritas persis tabel ARCHITECTURE.md §2 — jangan menambah task
  real-time baru tanpa revisi dokumen arsitektur.
- Semua pemakaian `xQueueReceive`/`xQueueSend` menyebut timeout secara eksplisit
  (`0` untuk non-blocking, `portMAX_DELAY` hanya untuk blocking pacing yang
  memang direncanakan).
- Drop policy ada di ARCHITECTURE.md §4 — implementasi harus menaikinya dan
  menyediakan counter (xrun/overrun) yang bisa dibaca UI/log.
- ISR callback (`on_conv_done`, `on_pool_ovf`): hanya di IRAM-safe; tanpa log,
  tanpa floating point; cukup semaphore/notify/task-safe counter.
- Tidak ada `printf`/`ESP_LOGI` di `sdr_rx_task` steady-state.

---

## 5. DSP

- Proses **per-blok** (256 frame), vector-style — jangan loop library per-sampel.
- Semua state filter (FIR history, DC blocker x/y, fase NCO, env AGC) dalam
  struct state statis milik `dsp_chain`; tidak ada state global lepas.
- Fase NCO: akumulator kontinu antar blok (jangan reset per blok); normalisasi
  ke `[0, 2π)` atau gunakan fixed-point Q31 (pilihan implementor, konsisten).
- Konstanta runtime dari `Kconfig.projbuild` atau `sdr_config` — bukan angka
  ajaib di tengah kode. Angka magic hanya boleh sebagai fallback Kconfig dengan
  komentar sumber.
- Presisi: `float` (bukan `double`) di seluruh jalur DSP; `int16_t` hanya di
  interface audio_out ↔ BSP.
- Pengujian: sediakan hook tone-injection internal (bypass ADC) untuk M2 —
  jalur test tidak boleh mengaktifkan kode di production path.

---

## 6. UI (LVGL 9)

- Seluruh objek dibuat di `ui_init()` yang dipanggil dari `app_main`; tidak ada
  pembuatan objek diam-diam dari modul lain.
- Sembilan puluh sembilan: semua akses objek LVGL dari luar `lvgl_task` wajib
  di bawah `bsp_lvgl_lock()`.
- Redraw: gunakan invalidasi objek; jangan memanggil directly-draw ke panel
  `esp_lcd` dari UI (BSP/LVGL yang mengelola).
- Widget spektrum/waterfall menerima data **hanya** melalui
  `ui_scope_push()` / `spectrum_get_db()` — UI tidak tahu asal data.
- Warna: semua RGB565 lewat helper di `ui_colors.h` (termasuk colormap
  waterfall) — tanpa literal warna tersebar.

---

## 7. Kconfig & Parameter

- Seluruh parameter yang mungkin diubah saat tuning dikumpulkan di
  `main/Kconfig.projbuild` dengan menu `SDR Configuration`, prefix simbol
  `CONFIG_SDR_*`:
  ```
  CONFIG_SDR_ADC_FS_HZ=48000
  CONFIG_SDR_FFT_N=1024
  CONFIG_SDR_DISPLAY_SPAN_HZ=3000
  CONFIG_SDR_BFO_HZ=1500
  CONFIG_SDR_WATERFALL_ROWS=64
  CONFIG_SDR_ADC_ATTEN_DB=11
  ```
- Nilai default harus sama dengan yang tertulis di `sdkconfig.defaults`.
- Mengubah default = mengubah dokumen (PRD/TECH_STACK) juga.

---

## 8. Git

- Commit message gaya imperative, ringkas, bahasa Inggris:
  `feat(dsp): add NCO offset mixer with LUT sin/cos`,
  `fix(adc): handle pool overflow counter`,
  `docs: add PRD and architecture spec`.
- Satu commit = satu perubahan logis; jangan campur formatting dengan fitur.
- Jangan commit direktori `build/` (tambahkan `.gitignore` root pada M0).

---

## 9. Checklist "Definition of Done" per fitur

- [ ] Mengacu ke FR/NFR di `PRD.md` (sebutkan ID di commit/PR).
- [ ] Menghormati batas modul `ARCHITECTURE.md` §3 (tidak ada include silang terlarang).
- [ ] Error path diuji (minimal: device unplug/touch hilang tidak menabrak).
- [ ] Tidak ada `malloc` di loop, tidak ada logging per-blok.
- [ ] Build bersih tanpa warning baru; `idf.py build` lulus.
- [ ] Counter diagnostik (xrun/overrun) bertambah jika fault disuntik.
