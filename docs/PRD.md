# PRD — ESP32-Micro-SDR

Product Requirements Document untuk firmware SDR audio I/Q berbasis ESP32-3248S035.
Dokumen ini adalah **sumber kebenaran fitur**: setiap baris kode yang ditulis harus
dapat ditelusuri ke salah satu FR di bawah. Jika sebuah permintaan tidak ada di sini,
jangan diimplementasikan.

---

## 1. Konteks & Visi

**Visi**: Mengubah board ESP32-3248S035 (CYD 3.5", ST7796 480×320, GT911 touch)
menjadi penerima SDR kelas audio yang memproses baseband I/Q dari **Tayloe
detector (quadrature sampling detector) eksternal**.

**Peran hardware eksternal (BUKAN tanggung jawab firmware ini)**:

| Blok | Keterangan |
| :--- | :--- |
| Tayloe detector | Mixer I/Q analog; output I dan Q berupa sinyal audio baseband |
| Si5351 + controller | Menyediakan frekuensi LO quadrature ke Tayloe. **Dikendalikan alat terpisah** — MCU firmware ini TIDAK mengontrolnya (tanpa I2C Si5351) |
| Front-end analog | Antenna, preamp, filter, buffer op-amp ke ADC. Output harus di-bias tengah (~1,65 V) dan skala ≤ ±1 V (atten 11 dB) |

**Tanggung jawab firmware ini**:
1. Sampling simultan I/Q via ADC DMA.
2. Pemrosesan DSP (DC removal, offset tuning digital, demodulasi SSB USB/LSB, AGC).
3. Output audio ke amplifier on-board (FM8002A via GPIO26) memakai modulator
   **sigma-delta (SDM) 1-bit** dari `esp_driver_sdm`.
4. Visualisasi spektrum + waterfall + UI tuning via touchscreen.

**Keputusan yang sudah terkunci** (jangan diubah tanpa revisi PRD ini):

| Aspek | Keputusan |
| :--- | :--- |
| Platform | ESP-IDF murni v6.1, target `esp32` klasik, bahasa C |
| Audio out | **Sigma-delta (SDM) 1-bit pada GPIO26** (`esp_driver_sdm`, output dirutekan via GPIO matrix) → input FM8002A. `dac_continuous`/`bsp_audio_init()` TIDAK dipakai karena berbagi I2S0 dengan `adc_continuous` (lihat §7) |
| LO | Eksternal; firmware hanya menampilkan *label* frekuensi (input via keypad) + offset NCO yang di-set user |
| Sampling | 48 kHz per kanal, span spektrum ±3 kHz |
| LCD/touch | BSP `bsp_esp323248s035` (disalin dari repo referensi `ESP32-3248S035-CYD-SDR`) |

---

## 2. Pin Mapping (terverifikasi dari BSP)

| Fungsi | GPIO | Keterangan |
| :--- | :--- | :--- |
| **Audio I (baseband)** | **36** | ADC1 channel 0 — input-only, khusus ADC |
| **Audio Q (baseband)** | **35** | ADC1 channel 7 — input-only, khusus ADC |
| TFT MOSI/SCLK/CS/DC/BL | 13 / 14 / 15 / 2 / 27 | SPI2_HOST, 80 MHz, ST7796 |
| TFT MISO | 12 | dipakai untuk read-back, tidak dipakai aplikasi |
| Touch SDA/SCL | 33 / 32 | I2C0, GT911 addr 0x5D |
| Audio out | 26 | sinyal **SDM 1-bit** → FM8002A (class-AB amplifier, selalu aktif); DAC1 tidak dipakai |
| RGB LED | 4 (R), 16 (G), 17 (B) | active-low |
| microSD | 19/23/18/5 | SPI3_HOST (opsional, future) |
| Touch INT/RST | 21 / 25 | **TIDAK dipakai driver touch** — tersedia untuk ekstensi |

Pin yang DILARANG dipakai untuk fungsi lain: GPIO 35 & 36 (khusus ADC I/Q),
GPIO 26 (DAC/audio), dan seluruh pin SPI2/I2C0 yang terdaftar di atas.

---

## 3. Functional Requirements

Notasi: **[CORE 1]** = tugas real-time, **[CORE 0]** = tugas UI.

### FR-1 — Sampling I/Q simultan (ADC DMA)
- Kedua kanal di-ADC1, mode continuous DMA (`adc_continuous`), pola scan
  CH0 (I) + CH7 (Q).
- Target 48 kHz per kanal (96 kSps agregat). Rate aktual **diukur** dari
  timestamp per blok dan dipakai sebagai dasar perhitungan NCO & bin FFT.
- Pembacaan non-blocking terhadap task lain (blocking hanya terhadap DMA),
  pool DMA internal ≥ 8 KB.
- Deinterleave: CH0 → I, CH7 → Q, konversi ke float `(raw − 2048) / 2048`.
  Catatan: urutan keluaran DMA dapat **terbalik** dari urutan array pola
  (teramati CH7 lebih dulu); pemasangan I/Q harus **tak bergantung urutan**.
- `CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y` → callback event wajib `IRAM_ATTR`.
- `adc_continuous_config_t.format` **deprecated/diabaikan** pada ESP32 (selalu
  TYPE1); `bit_width` harus eksplisit (nilai enum `ADC_BITWIDTH_*`, bukan
  `ADC_BITWIDTH_DEFAULT`); `atten` memakai enum `adc_atten_t`.
- **Acceptance**: rate terukur dalam ±2 % dari target; pasangan I/Q per
  timestamp identik (interleaved back-to-back); counter overrun DMA = 0
  setelah 10 menit.

### FR-2 — DC Removal
- High-pass 1 bentuk IIR per kanal: `y[n] = x[n] − x[n−1] + R·y[n−1]`,
  `R ≈ 0.996` (cutoff ~30 Hz @ 48 kHz).
- Dilakukan **sebelum** pencampuran NCO agar spike DC di tengah spektrum
  tidak tergeser ke frekuensi lain.
- **Acceptance**: spike di 0 Hz (setelah NCO 0 Hz) turun ≥ 30 dB dibanding
  tanpa DC removal.

### FR-3 — Software Offset Frequency (NCO)
- Pencampuran frekuensi digital: `I' = I·cos(φ) + Q·sin(φ)`,
  `Q' = Q·cos(φ) − I·sin(φ)`; akumulator fase `φ += 2π·f_offset/fs`.
- Rentang offset: **−2 MHz … +2 MHz** (`CONFIG_SDR_NCO_LIMIT_HZ`);
  step: 10 / 50 / 100 / 500 / 1k / 10k / 100k / 1M Hz.
  **Catatan fisika**: mixer NCO *digital* pada `fs = 48 kHz` hanya mampu
  merealisasikan offset sampai ±24 kHz (Nyquist) dan praktis ±3 kHz (span).
  Karena itu `SDR_NCO_LIMIT_HZ` adalah batas **rentang tuning UI / aritmetika
  label** (`lo_label + offset`); pergeseran kasar di luar band digital harus
  direalisasikan oleh LO eksternal (di luar firmware ini). Nilai digital yang
  benar-benar dicampur di-clamp ke band Nyquist/span oleh DSP di F5.
- Sumber kontrol: (a) tombol ± pada UI, (b) keypad angka LVGL (direct entry),
  (c) drag horizontal di area waterfall.
- Lookup sin/cos (LUT ≥ 1024 entri + interpolasi linear) — tanpa trig per-sampel.
- **Acceptance**: tonal pada input I/Q berpindah di layar sesuai offset;
  error frekuensi hasil tuning < 1 Hz pada 1 kHz (terverifikasi tone test).

### FR-4 — Demodulasi SSB (USB/LSB)
- Struktur **Weaver / phasing dua mixer** (bukan Hilbert — dipilih karena
  setara secara matematis dan lebih murah; lihat ARCHITECTURE.md §6):
  1. LPF kanal 2,7 kHz pada I dan Q.
  2. Decimasi halfband 48 → 24 → 12 kHz.
  3. Mixer BFO ke-2 pada 1500 Hz: USB = `e^{−j2π·1500·t}`,
     LSB = `e^{+j2π·1500·t}` (pertukaran tanda = pergantian mode).
  4. Ambil komponen I hasil mix → audio LPF ~2,0 kHz.
- **Acceptance**: sinyal uji tone pada satu sisi terdengar jelas pada mode yang
  benar dan melemah ≥ 25 dB (image sideband) pada mode yang salah.

### FR-5 — AGC + Volume + Audio Out
- AGC envelope: deteksi peak/RMS jangka pendek; attack ~5 ms, release ~200 ms;
  target level konfigurabel; gain di-clamp [g_min, g_max] dan di-smooth
  (tanpa "pumping" audible).
- Keluaran: `int8` (pulse density) → `sdm_channel_set_pulse_density()` pada
  kanal SDM (GPIO26) yang dipacu task audio berprioritas pada Core 1.
  (Jalur `bsp_audio_pcm_write()`/`dac_continuous` tidak dipakai — bentrok I2S0.)
- Volume user 0–100 %.
- **Acceptance**: input −20 dBFS dan −50 dBFS menghasilkan keluaran dalam
  ±6 dB satu sama lain; tidak ada klik/dorongan saat AGC beroperasi;
  underrun antrean audio = 0 saat steady-state.

### FR-6 — Spektrum (FFT Bar)
- FFT 1024 titik **pada IQ hasil decim 12 kHz** (11,7 Hz/bin), Hann window,
  maginitudo dalam dB.
- Tampilan: bar spektrum selebar area (±3 kHz di tengah frekuensi tuned).
- Refresh mengikuti timer UI (~30 ms); **frame boleh drop** (snapshot terbaru
  menangani yang lama).
- **Acceptance**: tonal pada offset diketahui muncul pada kolom piksel yang
  benar (±1 bin); SNR visual ≥ 40 dB.

### FR-7 — Waterfall (Air Terjun)
- Air terjun berwarna (LUT colormap 32 entri RGB565), mengalir ke bawah
  (waktu ke bawah), baris baru = spektrum terbaru.
- Target ≥ 20 fps pada resolusi penuh.
- **Acceptance**: sinyal bergerak (sweep) terlihat kontinu; tidak ada
  frame-rate collapse saat UI disentuh.

### FR-8 — Tampilan Frekuensi
- Bar atas menampilkan: frekuensi utama (label LO, default `7.030.000`,
  **diinput user via keypad — murni label visual** karena LO riil dikontrol
  alat terpisah) + offset NCO (mis. `[+1.200]`).
- Frekuensi aktual yang dirujuk = `lo_label + nco_offset`.
- **Acceptance**: perubahan offset langsung tercermin di teks; mode (USB/LSB),
  langkah step, dan S-meter terlihat.

### FR-9 — UI & Input Touch
- Layout landscape 480×320: bar frekuensi atas, waterfall, bar spektrum,
  bar bawah (tombol mode USB/LSB, ±, step, volume, keypad).
- Keypad angka LVGL untuk entry offset & label LO langsung.
- **Acceptance**: seluruh kontrol dapat dijangkau tanpa restart; debounce
  touch wajar; tidak ada ghost-touch.

### FR-10 — S-Meter
- Indikasi kekuatan sinyal dari level pasca-AGC (skala S1–S9+ atau dB),
  diperbarui ≥ 10 Hz.
- **Acceptance**: bergerak naik-turun mengikuti sinyal uji.

---

## 4. Non-Functional Requirements

| ID | Persyaratan | Nilai |
| :--- | :--- | :--- |
| NFR-1 | Pinning core | DSP di Core 1, UI di Core 0 (LVGL sudah default Core 0) |
| NFR-2 | Prioritas task | `sdr_rx` = 12, `audio_feed` = 11, LVGL = 4 |
| NFR-3 | Audio real-time | Sinyal audio **tidak boleh drop**; UI/spektrum boleh drop frame |
| NFR-4 | Waktu boot ke layar aktif | < 3 detik |
| NFR-5 | RAM internal terpakai (DSP+UI) | ≤ ~121 KB di atas IDF+LVGL; verifikasi `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` |
| NFR-6 | PSRAM | Tidak tersedia dan tidak diandalkan |
| NFR-7 | CPU | 240 MHz, optimasi `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`) |
| NFR-8 | Stabilitas | 10 menit run tanpa WDT reset, tanpa xrun audio steady-state |
| NFR-9 | Tuning parametrik | Semua konstanta runtime lewat `Kconfig.projbuild` / `sdr_config`, bukan hardcode |

---

## 5. Out of Scope (v1) — JANGAN diimplementasikan tanpa revisi PRD

- Kontrol Si5351 / VFO dari MCU (LO di luar).
- Mode AM, FM, CW, digital mode.
- Perekaman ke microSD (BSP sudah mendukung mount — cadangan M5).
- DAC I2S eksternal (pin 21/25 tersedia — cadangan kualitas audio).
- WiFi / Bluetooth / jaringan apa pun (nonaktif; hemat RAM & gangguan ADC).
- Kalibrasi I/Q imbalance (amplitudo/fase) — dicadangkan; struktur kode
  menyisakan slot koreksi setelah FR-2.
- Koreksi skew waktu antar-kanal ADC (sub-mikrodetik — tidak material untuk
  bandwidth 3 kHz).

---

## 6. Milestone & Kriteria Terima

| # | Milestone | Kriteria selesai | Cara verifikasi |
| :--- | :--- | :--- | :--- |
| M0 | BSP terpasang, UI dasar | Layar + touch hidup; TANPA inisialisasi LDR | `idf.py flash monitor` |
| M1 | ADC DMA capture | I/Q 48 kHz terukur, DC offset terlihat di log | Log rate + plot host |
| M2 | Rantai DSP → audio | Tone uji 1 kHz terdengar; USB/LSB berfungsi | Tone injection internal |
| M3 | FFT + spectrum + waterfall | ≥20 fps, tonal terlihat | `dsps_view` + visual |
| M4 | AGC + tuning + polish | Drag/keypad menyetel offset, teks freq akurat | Uji sinyal RF nyata |
| M5 | Ekstensi (opsional) | CW / squelch / rekam SD | — |

---

## 7. Risiko & Mitigasi

| Risiko | Mitigasi |
| :--- | :--- |
| `bsp_init()` memanggil LDR (ADC1_CH6) → bentrok dengan ADC1 DMA | **Dilarang memanggil `bsp_init()`**; gunakan inisialisasi granular (lihat CONVENTIONS.md) |
| **Bentrok I2S0**: `adc_continuous` dan `dac_continuous` sama-sama memakai I2S0 pada ESP32 (`ADC_HAL_DMA_I2S_HOST 0`, `DAC_DMA_PERIPH_I2S_NUM 0`) → acquire kedua gagal (`ESP_ERR_NOT_FOUND`) | **Jangan pakai `dac_continuous`/`bsp_audio_init()`**; audio out lewat SDM 1-bit (GPIO26) |
| `bsp_ldr_init()` memakai `adc_oneshot` ADC1_CH6 → bentrok dengan ADC1 continuous DMA | **Dilarang memanggil `bsp_ldr_init()`** selama ADC DMA I/Q aktif |
| Rate ADC aktual meleset dari parameter "expected" | Ukur rate aktual; kalibrasi konstanta NCO/bin dari rate terukur |
| RAM waterfall terlalu besar | Mulai 64 baris (60 KB); fallback 48 baris (45 KB) |
| ADC ESP32 nonlinier/noise | Attenuation 11 dB, bias tengah, DC block, rata-rata/decim anti-alias |
| Klik audio saat start/stop | Mulai stream dengan zero-fill; AGC attack cepat |
