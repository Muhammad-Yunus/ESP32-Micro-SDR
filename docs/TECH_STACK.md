# TECH_STACK — ESP32-Micro-SDR

Daftar dependensi **terverifikasi** beserta versi persis. Dokumen ini ada untuk
mencegah halusinasi pustaka/API: jika sebuah pustaka atau fungsi tidak tercantum
di sini, anggap **tidak tersedia** sampai diverifikasi terhadap instalasi lokal
ESP-IDF v6.1 atau component registry.

---

## 1. Toolchain & Platform

| Komponen | Versi / nilai | Bukti verifikasi |
| :--- | :--- | :--- |
| ESP-IDF | **v6.1** (`C:/esp/v6.1/esp-idf`) | `build/project_description.json` (repo referensi & skeleton) |
| Target chip | **esp32** klasik (Xtensa dual-core, WROOM) | `CONFIG_IDF_TARGET="esp32"` |
| Compiler | xtensa-esp-elf-15.2.0 | project_description.json |
| CMake | ≥ 3.22 (terpasang 4.0.3) | `firmware/CMakeLists.txt` |
| FreeRTOS | bawaan IDF (`CONFIG_FREERTOS_HZ=1000` — non-default, di-set di sdkconfig.defaults) | |
| Bahasa | C11 (bukan C++; tidak ada Arduino) | |

---

## 2. Pustaka managed (component registry)

| Pustaka | Versi | Dipakai untuk | Cara pasang |
| :--- | :--- | :--- | :--- |
| `espressif/esp-dsp` | **^1.8.2** (terbaru saat penulisan: 1.8.2, rilis 2026-05-11) | FFT, FIR/decimasi, window, utilitas DSP | `idf.py add-dependency "espressif/esp-dsp^1.8.2"` di `firmware/main/idf_component.yml` |
| `lvgl/lvgl` | **^9.1.0** (terkunci 9.6.0~1 di repo referensi; **terverifikasi 9.6 di board** lewat log boot `bsp_lvgl: LVGL 9.6 initialized (480x320)`) | Seluruh UI | Tidak perlu ditambah manual — sudah ditarik oleh komponen BSP |

Catatan: `dependencies.lock` akan terbentuk otomatis saat build pertama —
commit file tersebut.

---

## 3. Komponen lokal

| Komponen | Asal | Status |
| :--- | :--- | :--- |
| `firmware/components/bsp_esp323248s035/` | Disalin **apa adanya** dari `C:\D\DOCUMENT_BCK\GitHub\ESP32-3248S035-CYD-SDR\firmware\components\bsp_esp323248s035\` | **Black box** — hanya API publik di `include/bsp_esp323248s035.h`; dilarang memodifikasi |
| `firmware/components/lvgl__lvgl/` | Managed component (otomatis via BSP) | Dibawa build |

Dependensi internal BSP (ikut terpasang otomatis): `idf >=5.4`, `lvgl ^9.1.0`.

---

## 4. Inventaris API yang BOLEH dipakai (terverifikasi ada di IDF v6.1)

### 4.1 Sampling I/Q — `esp_adc/adc_continuous.h` (ADC DMA)
Fungsi utama: `adc_continuous_new_handle`, `adc_continuous_register_event_callbacks`,
`adc_continuous_config`, `adc_continuous_start/stop`, `adc_continuous_read`,
`adc_continuous_deinit` (opsional: `adc_continuous_read_parse`).

Fakta ESP32 yang harus dipatuhi implementasi:

| Konstanta / fakta | Nilai di ESP32 | Sumber |
| :--- | :--- | :--- |
| `SOC_ADC_DIGI_RESULT_BYTES` | **2** (parse maju 2 byte per hasil) | `soc/soc_caps.h:145` |
| Format hasil | `adc_digi_output_data_t.type1`: `data:12`, `channel:4` | `hal/adc_types.h` (esp_hal_ana_conv) |
| `SOC_ADC_DIGI_DATA_BYTES_PER_CONV` | 4 (`conv_frame_size` wajib kelipatan 4) | `soc/soc_caps.h:146` |
| `SOC_ADC_SAMPLE_FREQ_THRES_LOW/HIGH` | 20 000 / 2 000 000 Hz (batas konfigurasi; target nyata 96 kSps agregat) | `soc/soc_caps.h:148-149` |
| Mode konversi | `ADC_CONV_SINGLE_UNIT_1` (hanya ADC1; **ADC2 tidak mendukung DMA**) | `hal/adc_types.h:84`; `soc_caps.h:142` |
| Pola kanal | `adc_digi_pattern_config_t` {unit, channel, bit_width, atten} | |
| Kanal GPIO | I = `ADC_CHANNEL_0` (GPIO36), Q = `ADC_CHANNEL_7` (GPIO35) | peta ADC ESP32 |
| `SOC_ADC_PATT_LEN_MAX` | 16 (cukup untuk 2 kanal) | `soc_caps.h:142` |
| `bit_width` wajib eksplisit | `ADC_BITWIDTH_DEFAULT` = 0 **ditolak** `adc_continuous_config` ("ADC bitwidth not supported") → pakai `ADC_BITWIDTH_12` | `adc_types.h:57`; `adc_continuous.c:464` |
| `atten` = enum | Nilai dB bukan enum; wajib `adc_atten_t`. IDF v6.1: `ADC_ATTEN_DB_0=0`, `ADC_ATTEN_DB_2_5=1`, `ADC_ATTEN_DB_6=2`, `ADC_ATTEN_DB_12=3` (nama `DB_11` **tidak ada**) | `adc_types.h:47-51`; `adc_continuous.c:515` |
| `format` deprecated | `adc_continuous_config_t.format` diabaikan di ESP32 (selalu TYPE1) | `adc_continuous.h` |
| Urutan hasil DMA | Dapat **terbalik** dari urutan array pola (teramati CH7 sebelum CH0) → pairing I/Q tak bergantung urutan | pengamatan runtime F3 |
| Callback ISR | `CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y` → `on_conv_done`/`on_pool_ovf` wajib `IRAM_ATTR` | `adc_continuous.h` |
| **Bentrok I2S0** | `adc_continuous` memakai I2S0 (`ADC_HAL_DMA_I2S_HOST 0`); `dac_continuous` juga (`DAC_DMA_PERIPH_I2S_NUM 0`) → acquire kedua gagal | `esp_driver_i2s/i2s_platform.c`; `esp_driver_dac/esp32/dac_dma.c` |

### 4.2 Audio out — `esp_driver_sdm` (sigma-delta 1-bit) di GPIO26
**TIDAK memakai** `dac_continuous`/`bsp_audio_*` karena keduanya mengklaim I2S0
(sama dengan `adc_continuous`, §4.1). Audio out memakai modulator sigma-delta:

Fungsi: `sdm_new_channel(const sdm_config_t*, sdm_channel_handle_t*)`,
`sdm_channel_enable/disable`, `sdm_channel_set_pulse_density(chan, int8_t)`,
`sdm_del_channel` — `driver/sdm.h` (`esp_driver_sdm`).
`SDM_CHANS` (kanal SDM per instance) & `SDM_CLK_SRC_DEFAULT` di `sdm_types.h`.

Fakta penting (`esp_driver_sdm/src/sdm.c`):
- Sinyal output dirutekan ke GPIO mana pun via **GPIO matrix**
  (`gpio_matrix_output(gpio_num, sig_id_matrix, ...)`, `sdm.c:375`) — GPIO26
  valid (output-capable). Jadi SDM tidak terbatas pada pin khusus.
- `sample_rate_hz` = laju update modulasi (prescale dari `src_clk_hz`); untuk
  48 kHz audio, kepadatan di-update per sampel audio di task audio.
- Densitas = `int8_t` (−128..127) → kualitas ≈ 8-bit dengan shaping noise HF.
  Board sudah punya FM8002A di GPIO26; idealnya tambah RC LPF di pin (bila
  akses memungkinkan) — amp/speaker sendiri juga meredam komponen HF.
- Bila GPIO sudah di-reserve driver lain, `sdm_new_channel` hanya memberi
  warning (`esp_gpio_reserve`), tetap jalan.

### 4.3 Display & touch — API BSP
`bsp_tft_init`, `bsp_tft_width/height`, `bsp_tft_set_rotation`,
`bsp_touch_init`, `bsp_touch_read(uint16_t *x, uint16_t *y)`,
`bsp_led_set_rgb`, `bsp_lvgl_init`, `bsp_lvgl_lock/unlock`,
`bsp_lvgl_display`, `bsp_lvgl_indev`. (semua di `bsp_esp323248s035.h`)

### 4.4 esp-dsp v1.8.2 — fungsi yang dipakai

| Fungsi | Peran | Catatan |
| :--- | :--- | :--- |
| `dsps_fft2r_init_fc32(NULL, N)` | Init twiddle FFT (sekali, di `spectrum_init`) | N = `CONFIG_DSP_MAX_FFT_SIZE` (Kconfig esp-dsp) = **2048**. **Hanya boleh dipanggil sekali** — tabel twiddle bersifat global |
| `dsps_fft2r_fc32(data, N)` | FFT kompleks radix-2 (ada varian asm `ae32` untuk ESP32) | data interleaved `Re[0],Im[0],…`, **aligned 16** |
| `dsps_bit_rev_fc32(data, N)` | Bit-reverse — dipanggil **SETELAH** `dsps_fft2r_fc32` | urutan resmi dari contoh repo |
| `dsps_cplx2reC_fc32(data, N)` | Split hasil FFT ke array re[]/im[] | |
| `dsps_wind_hann_f32(buf, N)` | Window Hann (real) | dipakai manual pada re & im saat copy |
| `dsps_fir_f32` | FIR float (LPF kanal, audio LPF) | state history ditangani pemanggil |
| `dsps_fir_dec2_f32` | FIR decimasi-by-2 (halfband 48→24→12) | verifikasi signature saat implementasi terhadap header `dsps_fir.h` lokal komponen |
| `dsps_view`, `dsp_get_cpu_cycle_count` | Debug spektrum serial & profiling | dipakai saat pengembangan |

**Urutan FFT resmi** (disalin dari contoh `espressif/esp-dsp`
`examples/fft/main/dsps_fft_main.c`):
```c
dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);   // sekali
// per frame, salin IQ × window ke buffer aligned:
dsps_fft2r_fc32(s_fft, N);        // 1. FFT
dsps_bit_rev_fc32(s_fft, N);      // 2. bit-reverse
dsps_cplx2reC_fc32(s_fft, N);     // 3. split
db[k] = 10.f*log10f((re*re + im*im) / N);              // 4. power dB
```

**Mode bin runtime 1024 vs 2048** (F6): FFT **hanya** dijalankan pada N=2048.
Mode 1024 diambil dari bin genap hasil FFT-2048 (`X₂₀₄₈[2m] == X₁₀₂₄[m]`,
terverifikasi numerik dengan galat maks 2,3e-14) → identik dengan DFT-1024 tanpa
inisialisasi ulang, tanpa tabel twiddle tambahan, dan tanpa alokasi heap
tambahan. Jangan memanggil `dsps_fft2r_init_fc32()` ulang untuk mode ini.

### 4.5 FreeRTOS & utilitas IDF
`xTaskCreatePinnedToCore`, queue (`xQueueCreate/Send/Receive/Overwrite` semu
untuk snapshot), critical section, `esp_timer_get_time`, `heap_caps_*`,
`esp_check.h`, `esp_log.h`.

---

## 5. Konfigurasi build wajib (`sdkconfig.defaults`)

```
CONFIG_IDF_TARGET="esp32"
CONFIG_FREERTOS_HZ=1000
CONFIG_COMPILER_OPTIMIZATION_PERF=y          # -O2; wajib untuk asm esp-dsp
CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096
CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y
CONFIG_DSP_MAX_FFT_SIZE=2048                 # Kconfig komponen esp-dsp
CONFIG_BSP_TFT_ROTATION_270=y
CONFIG_BSP_TFT_SPI_CLK_HZ=80000000
CONFIG_BSP_AMP_DRIVER_DAC=y
CONFIG_BSP_LVGL_TASK_CORE=0
```
Parameter aplikasi (`main/Kconfig.projbuild`, menu `SDR Configuration`) —
nilai default harus identik dengan `firmware/sdkconfig.defaults`:

```
CONFIG_SDR_ADC_FS_HZ=48000
CONFIG_SDR_FFT_N=1024                        # jendela analisis (zero-pad ke 2048)
CONFIG_SDR_AUDIO_FS_HZ=12000                 # FS post-decimasi → batas span ±6 kHz
CONFIG_SDR_DISPLAY_SPAN_HZ=50000             # default build (clamp ke FS/2 saat boot)
CONFIG_SDR_BFO_HZ=1500
CONFIG_SDR_WATERFALL_ROWS=60
CONFIG_SDR_DEFAULT_VOLUME_PCT=0                # default mute, naik lewat Vol+/Vol−
CONFIG_SDR_ADC_ATTEN_DB=11
CONFIG_SDR_ADC_POOL_BYTES=8192
CONFIG_SDR_AGC_TARGET=96                     # target AGC (0 = AGC off)
CONFIG_SDR_AGC_ATTACK_MS=5.0
CONFIG_SDR_AGC_RELEASE_MS=200.0
CONFIG_SDR_AGC_GAIN_MAX=512.0                # batas gain AGC linear (+54 dB)
CONFIG_SDR_SELFTEST=n                         # y = build self-test (FR-11), tanpa ADC
```

### 5.1 Build self-test (`CONFIG_SDR_SELFTEST=y`)

Build khusus untuk verifikasi rantai DSP on-target tanpa sinyal RF:

```
# firmware/sdkconfig.defaults  →  CONFIG_SDR_SELFTEST=y
idf.py build && idf.py -p <PORT> flash monitor
```

- `main/CMakeLists.txt` menambahkan `selftest.c` + `iq_gen.c` hanya saat
  `CONFIG_SDR_SELFTEST=y` → build produksi (`=n`) tidak menambah BSS maupun
  kode.
- `app_main.c` **tidak** memanggil `rx_adc_init()` pada build ini (GPIO35/36
  tidak tersentuh); task `selftest` (Core 1, prio 12, stack statis 8 KB)
  menyuntikkan I/Q sintetis ke rantai DSP yang sama.
- Laporan: tag log `selftest`, baris per kasus `[PASS|WARN|FAIL] Txx ...`
  ditutup `=== RINGKASAN: N kasus -> X PASS, Y WARN, Z FAIL ===`.
- Build produksi tetap perlu diverifikasi setelahnya: kembalikan
  `CONFIG_SDR_SELFTEST=n` lalu build + flash ulang.

Catatan: ukuran flash partisi default skeleton = 2 MB — verifikasi ukuran flash
sesungguhnya board dengan `esptool.py flash_id` saat M0 dan sesuaikan jika
board berisi 4/8 MB. (Board terverifikasi 2 MB; log boot: "Detected size(4096k)
larger than the size in the binary image header(2048k)" — image header 2 MB
yang dipakai.)

---

## 6. DILARANG / TIDAK TERSEDIA (anti-halusinasi)

| Item | Alasan (terverifikasi di IDF v6.1) |
| :--- | :--- |
| `driver/i2s.h` (legacy I2S) & `I2S_MODE_ADC` / `i2s_driver_install` | **Driver legacy sudah dihapus**; hanya ada `driver/i2s_std.h`, `i2s_pdm.h`, `i2s_tdm.h` (baru) |
| Sampling ADC via I2S | Tidak mungkin di IDF 6.x — gunakan `adc_continuous` (§4.1) |
| Arduino, TFT_eSPI, LovyanGFX, U8g2 | Proyek murni ESP-IDF; display via `esp_lcd` + LVGL milik BSP |
| `bsp_init()` | Memanggil `bsp_ldr_init()` (ADC1_CH6) yang bentrok dengan ADC1 DMA — pakai init granular (ARCHITECTURE.md §7) |
| `bsp_ldr_init()` | Memakai `adc_oneshot` pada ADC1_CH6 → bentrok dengan ADC1 continuous DMA (F3) |
| `dac_continuous` / `bsp_audio_init()` / `bsp_audio_pcm_*` | Mengklaim **I2S0**, sama dengan `adc_continuous` → acquire kedua gagal (`ESP_ERR_NOT_FOUND`). Audio out via SDM (§4.2) |
| `ADC_BITWIDTH_DEFAULT` di pola ADC | Nilai 0 ditolak `adc_continuous_config` → pakai `ADC_BITWIDTH_12` |
| `ADC_ATTEN_DB_11` (nama lama) | Tidak ada di IDF v6.1; enum-nya `ADC_ATTEN_DB_12` |
| Memodifikasi kode BSP | Komponen dianggap stabil/black box |
| ADC2 untuk I/Q | DMA ADC hanya didukung ADC1 di ESP32 klasik |
| GPIO 34–39 sebagai output (LED, I2S out, dsb.) | Input-only pins |
| PSRAM (`CONFIG_ESP32_SPIRAM_SUPPORT`) | Tidak aktif di board/konfigurasi ini |
| WiFi, BT, mDNS, HTTP | Tidak dipakai (RAM & interferensi ADC) |
| `double` di jalur DSP | Gunakan `float` |
| `dsps_fft2r_init_fc32()` dipanggil lebih dari sekali | Tabel twiddle esp-dsp global; inisialisasi kedua menimpa tabel → spektrum salah. Mode bin 1024/2048 achieved lewat **pemilihan bin keluar**, bukan init ulang |
| Alokasi heap per perubahan skala tampilan | Mode FFT bin & spanLO hanya mengubah field `sdr_config` + `ui_scope_refresh()`; buffer `s_db` 2048 float dialokasikan di BSS statis |
| FreeRTOS `xQueueOverwrite` untuk audio | Audio memakai drop-baru + xrun counter; overwrite hanya untuk snapshot UI |

---

## 7. Aset font (DSEG7) — regenerasi

Font 7-segmen `firmware/main/ui/font_dseg7_36.c` **dibuat otomatis** (jangan
edit bitmap manual). Sumber: DSEG7 Classic Bold, SIL OFL 1.1 (lihat
`docs/licenses/DSEG-OFL.txt`).

Langkah regenerasi:

```
# 1. Unduh font (DSEG v0.46) dari keshikan/DSEG — pakai DSEG7Classic-Bold.ttf
#    https://github.com/keshikan/DSEG/releases/download/v0.46/fonts-DSEG_v046.zip

# 2. Konversi ke sumber LVGL (subset: spasi..'.' + digit 0-9)
npx --yes lv_font_conv --font DSEG7Classic-Bold.ttf \
    -r 0x20-0x2E,0x30-0x39 --format lvgl --bpp 4 --size 36 --no-compress \
    -o firmware/main/ui/font_dseg7_36.c

# 3. WAJIB: ganti include hasil generate `#include "../../lvgl.h"` menjadi
#    `#include "lvgl.h"` agar cocok dengan include path komponen.
```

Catatan: menambah ukuran Montserrat lain (mis. 48) perlu `CONFIG_LV_FONT_*` di
`sdkconfig.defaults` lalu `idf.py set-target esp32` ulang.

---

## 8. Referensi resmi

- esp-dsp API: https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/esp-dsp-apis.html
- esp-dsp contoh FFT: https://github.com/espressif/esp-dsp/tree/master/examples/fft
- esp-dsp registry: https://components.espressif.com/components/espressif/esp-dsp
- IDF ADC & DAC: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/adc_dac.html
- IDF SDM (sigma-delta): https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sdm.html
- IDF I2S (baru): https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html
- LVGL 9: https://docs.lvgl.io/9.x/
- BSP Arduino asal (pin & urutan init ST7796/GT911): https://github.com/ardnew/ESP32-3248S035
- Repo referensi lokal (BSP teruji): `C:\D\DOCUMENT_BCK\GitHub\ESP32-3248S035-CYD-SDR\firmware\components\bsp_esp323248s035\`
- Generator koefisien FIR/Hilbert (tools pengembang, offline):
  `scipy.signal.remez` / `firwin` — hasil disimpan sebagai tabel konstan di kode.

---

## 9. Aturan verifikasi ketika ragu

1. Cek header di instalasi lokal:
   `C:\esp\v6.1\esp-idf\components\...\include\**\*.h` (grep simbol yang dicari).
2. Cek component registry untuk versi esp-dsp/lvgl terbaru.
3. Jika API tidak ditemukan di v6.1 → **tidak boleh dipakai**; cari padanan di
   §4 atau laporkan sebagai blocker, jangan mengganti dengan API dari contoh
   internet berbasis versi lama IDF.
