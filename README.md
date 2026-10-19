# ESP32-Micro-SDR

> Penerima SDR audio SSB di papan **ESP32-3248S035 CYD** (ESP32 WROOM-32,
> touchscreen ST7796 480×320 + GT911). Sinyal baseband I/Q datang dari
> **Tayloe detector** eksternal; firmware ini menangani sampling ADC,
> demodulasi SSB, dan komputasi FFT + UI spektrum/waterfall.

[![ESP-IDF v6.1](https://img.shields.io/badge/ESP--IDF-v6.1-24B45E?label=esp--idf&logo=espressif&logoColor=white)](https://github.com/espressif/esp-idf)
[![ESP32 WROOM](https://img.shields.io/badge/chip-ESP32%20WROOM-32-24B45E?label=chip&logo=espressif&logoColor=white)](https://www.espressif.com/en/products/modules/esp32)
[![C11](https://img.shields.io/badge/language-C11-00589A)]()
[![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)]()
[![LVGL 9.1](https://img.shields.io/badge/UI-LVGL%209.1-orange)]()
[![esp-dsp 1.8.2](https://img.shields.io/badge/DSP-esp--dsp%201.8.2-purple)]()
[![sigma-delta audio](https://img.shields.io/badge/audio--out-Sigma--Delta%201--bit-007EC6)]()
[![SSB USB/LSB](https://img.shields.io/badge/mode-SSB%20USB/LSB-red)]()
[![MIT](https://img.shields.io/badge/license-MIT-brightgreen)](LICENSE)
[![ham radio](https://img.shields.io/badge/ham%20radio-%E2%97%86%E2%97%86-gold?labelColor=black)]()

---

## Signal Chain

![Signal Chain Diagram](docs/hardware/testing_diagram.jpg)
![Signal Chain Photo](docs/hardware/testing_photo.jpg)

**Blok terkait:**

| Blok | Peran | Link |
| :--- | :--- | :--- |
| HT UVK-1 | Sumber sinyal HF yang di-down-sampling | — |
| MCL ADE-1 | Mixer down-sampling HF → baseband audio | — |
| M5 Cardputer Si5351 VFO | LO quadrature (90°) untuk Tayloe detector, dikontrol terpisah (I2C M5) | [repo](https://github.com/Muhammad-Yunus/M5Cardputer-Si5351-VFO) |
| Tayloe Detector (DIY) | Quadrature sampling detector berbasis FST3253, PCB Matrix | — |
| ESP32-Micro-SDR (repo ini) | ADC sampling, DSP chain, FFT, UI spektrum/waterfall, audio out SDM | — |

---

## Fitur

- [x] ADC DMA I/Q 48 kHz/kanal (continuous, 12-bit, pool internal 8 KB)
- [x] DC block + NCO offset digital (LUT 1024 + interp linear)
- [x] Demodulasi SSB Weaver (USB/LSB, BFO 1500 Hz) @12 kHz pasca-decimation
- [x] AGC (attack 5 ms / release 200 ms) + volume 0–100 %
- [x] Audio out sigma-delta 1-bit @10 MHz carrier (GPIO26 → FM8002A)
- [x] FFT 1024 @12 kHz, Hann window, skala dBFS (clamp bawah −148)
- [x] Line chart spektrum ±3 kHz, 1 bin/piksel, marker puncak + chip frekuensi
- [x] Waterfall 64 baris RGB565 (fallback 48), colormap 32 entri
- [x] Drag horizontal waterfall = tuning NCO offset
- [x] UI LVGL 9.1: top bar (LO label + offset), bottom bar (mode, ±, step, volume, keypad)

---

## Harware WIring & Schematics

![Wiring diagram and schematics](docs/hardware/wiring_diagram_and_schematics.jpg)
![Tayloe Detector PCD Matrix](docs/hardware/Tayloe_Detector_PCB_Matrix.jpg)

---

## Dokumentasi

| File | Isi |
| :--- | :--- |
| [`docs/PRD.md`](docs/PRD.md) | Functional & Non-Functional Requirements |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Struktur modul, task/core, antrean, aliran data, urutan init |
| [`docs/CONVENTIONS.md`](docs/CONVENTIONS.md) | Gaya kode C11, error handling, memori, Kconfig, git |
| [`docs/TECH_STACK.md`](docs/TECH_STACK.md) | Versi pustaka terverifikasi + daftar API yang boleh dipakai |
| [`AGENT.md`](AGENT.md) | Aturan wajib AI agent (PRD > ARCH > CONV > TECH) |

---

## Struktur

```
ESP32-Micro-SDR/
├── AGENT.md                     # Aturan AI agent
├── README.md
├── docs/
│   ├── PRD.md
│   ├── ARCHITECTURE.md
│   ├── CONVENTIONS.md
│   ├── TECH_STACK.md
│   └── hardware/
│       ├── testing_diagram.jpg
│       ├── testing_photo.jpg
│       ├── wiring_diagram_and_schematics.jpg
│       └── Tayloe_Detector_PCB_Matrix.jpg
└── firmware/
    ├── sdkconfig.defaults
    ├── main/
    │   ├── Kconfig.projbuild
    │   ├── app_main.c           # Init granular (tanpa bsp_init)
    │   ├── sdr_config.c/h       # Runtime config (NCO, LO label, volume, AGC)
    │   ├── rx_adc.c/h           # ADC DMA I/Q (48 kHz, 12-bit)
    │   ├── dsp_chain.c/h        # DC block, NCO, LPF/decim, Weaver, AGC, audio out
    │   ├── spectrum.c/h         # FFT 1024 pool + Hann + dBFS
    │   └── ui/
    │       ├── ui_main.c/h      # Layout, keypad, tombol
    │       ├── ui_scope.c/h     # Spectrum line chart + waterfall + drag
    │       └── ui_colors.c/h    # Palette + LUT colormap 32 entri
    ├── components/
    │   └── bsp_esp323248s035/   # BSP CYD (black box)
    └── managed_components/      # lvgl, esp-dsp (via registry)
```

---

## Build & Flash

```bash
# Prasyarat: ESP-IDF v6.1
cd firmware
idf.py -p COM10 build
idf.py -p COM10 flash monitor
```

## Project Terkait

- [M5Cardputer-Si5351-VFO](https://github.com/Muhammad-Yunus/M5Cardputer-Si5351-VFO) — LO quadrature Si5351 untuk Tayloe detector
- [ESP32-3248S035-CYD-SDR](https://github.com/Muhammad-Yunus/ESP32-3248S035-CYD-SDR) — referensi awal & BSP CYD
